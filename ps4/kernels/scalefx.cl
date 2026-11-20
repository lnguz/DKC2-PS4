/* HD filter for the PS4 host: ScaleFX (3x) -> Lanczos2 to 1080p -> CAS.

   sfx0..sfx4 are a compute port of ScaleFX by Sp00kyFox (libretro
   slang-shaders, edge-smoothing/scalefx/shaders/scalefx-pass0..4.slang):

     ScaleFX - Pass 0..4
     Copyright (c) 2016 Sp00kyFox - ScaleFX@web.de
     Permission is hereby granted, free of charge, to any person obtaining a
     copy of this software and associated documentation files (the
     "Software"), to deal in the Software without restriction, including
     without limitation the rights to use, copy, modify, merge, publish,
     distribute, sublicense, and/or sell copies of the Software, and to permit
     persons to whom the Software is furnished to do so, subject to the
     following conditions: The above copyright notice and this permission
     notice shall be included in all copies or substantial portions of the
     Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

   Parameters as the stock preset: SFX_CLR 0.50, SFX_SAA 1, SFX_SCN 1.
   Intermediate data is kept as plain bits/integers instead of the packed
   float framebuffers the shader chain needs (pass 2: 4 bits per corner,
   pass 3: crn/mid subpixel indices 0-8 in 4 bits each).

   lanczos: Lanczos2 resampling of the 3x image to the output rectangle, with
   per-column / per-row tap indices and weights computed by the CPU, and
   anti-ringing (clamped to the 2x2 nearest texels).
   cas: AMD FidelityFX Contrast Adaptive Sharpening (the public algorithm),
   writing the final picture (and black side bars) into the display buffer. */

static inline float minf(float a, float b) { return __builtin_fminf(a, b); }
static inline float maxf(float a, float b) { return __builtin_fmaxf(a, b); }
static inline float sat(float v) { return minf(maxf(v, 0.0f), 1.0f); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

#define TID_X (__builtin_amdgcn_workgroup_id_x() * 8u + __builtin_amdgcn_workitem_id_x())
#define TID_Y (__builtin_amdgcn_workgroup_id_y() * 8u + __builtin_amdgcn_workitem_id_y())

static inline float3 rgbf(uint c)
{
    return (float3)((float)((c >> 16) & 255u) * (1.0f / 255.0f), (float)((c >> 8) & 255u) * (1.0f / 255.0f),
                    (float)(c & 255u) * (1.0f / 255.0f));
}

static inline uint packf(float3 c)
{
    uint r = (uint)(sat(c.x) * 255.0f + 0.5f), g = (uint)(sat(c.y) * 255.0f + 0.5f), b = (uint)(sat(c.z) * 255.0f + 0.5f);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* ------------------------------------------------------------ pass 0 */

/* Reference: http://www.compuphase.com/cmetric.htm */
static inline float cdist(float3 A, float3 B)
{
    float r = 0.5f * (A.x + B.x);
    float3 d = A - B;
    float3 c = (float3)(2.0f + r, 4.0f, 3.0f - r);
    return __builtin_sqrtf(c.x * d.x * d.x + c.y * d.y * d.y + c.z * d.z * d.z) / 3.0f;
}

kernel void sfx0(global const uint *src, global float4 *m0, uint w, uint h)
{
    int x = (int)TID_X, y = (int)TID_Y;
    if (x >= (int)w || y >= (int)h) return;
    const int W = (int)w - 1, H = (int)h - 1;
#define SRC(dx, dy) rgbf(src[clampi(y + (dy), 0, H) * (int)w + clampi(x + (dx), 0, W)])
    float3 A = SRC(-1, -1), B = SRC(0, -1), C = SRC(1, -1), E = SRC(0, 0), F = SRC(1, 0);
#undef SRC
    m0[y * (int)w + x] = (float4)(cdist(E, A), cdist(E, B), cdist(E, C), cdist(E, F));
}

/* ------------------------------------------------------------ pass 1 */

static inline float sfx_str(float d, float2 a, float2 b)
{
    float diff = a.x - a.y;
    float wght1 = maxf(0.5f - d, 0.0f) / 0.5f;
    float wght2 = sat((1.0f - d) + ((minf(a.x, b.x) + a.x > minf(a.y, b.y) + a.y) ? diff : -diff));
    return (wght1 * wght2) * (a.x * a.y); /* SFX_SAA == 1 */
}

kernel void sfx1(global const float4 *m0, global float4 *s1, uint w, uint h)
{
    int x = (int)TID_X, y = (int)TID_Y;
    if (x >= (int)w || y >= (int)h) return;
    const int W = (int)w - 1, H = (int)h - 1;
#define M(dx, dy) m0[clampi(y + (dy), 0, H) * (int)w + clampi(x + (dx), 0, W)]
    float4 A = M(-1, -1), B = M(0, -1);
    float4 D = M(-1, 0), E = M(0, 0), F = M(1, 0);
    float4 G = M(-1, 1), Hm = M(0, 1), I = M(1, 1);
#undef M
    float4 res;
    res.x = sfx_str(D.z, (float2)(D.w, E.y), (float2)(A.w, D.y));
    res.y = sfx_str(F.x, (float2)(E.w, E.y), (float2)(B.w, F.y));
    res.z = sfx_str(Hm.z, (float2)(E.w, Hm.y), (float2)(Hm.w, I.y));
    res.w = sfx_str(Hm.x, (float2)(D.w, Hm.y), (float2)(G.w, G.y));
    s1[y * (int)w + x] = res;
}

/* ------------------------------------------------------------ pass 2 */

/* GLSL's step-based helpers, componentwise: GE(x,y) = x > y, LE(x,y) = x < y,
   LEQ(x,y) = x <= y, as 1.0 / 0.0 */
static inline float GEs(float x, float y) { return x > y ? 1.0f : 0.0f; }
static inline float4 GE4(float4 x, float4 y) { return (float4)(GEs(x.x, y.x), GEs(x.y, y.y), GEs(x.z, y.z), GEs(x.w, y.w)); }
static inline float4 LE4(float4 x, float4 y) { return GE4(y, x); }
static inline float4 LEQ4(float4 x, float4 y)
{
    return (float4)(x.x <= y.x ? 1.0f : 0.0f, x.y <= y.y ? 1.0f : 0.0f, x.z <= y.z ? 1.0f : 0.0f, x.w <= y.w ? 1.0f : 0.0f);
}
static inline float4 min41(float4 v) { return (float4)(minf(v.x, 1.0f), minf(v.y, 1.0f), minf(v.z, 1.0f), minf(v.w, 1.0f)); }
static inline float4 min4(float4 a, float4 b) { return (float4)(minf(a.x, b.x), minf(a.y, b.y), minf(a.z, b.z), minf(a.w, b.w)); }

static inline float4 dom(float3 x, float3 y, float3 z, float3 w)
{
    return 2.0f * (float4)(x.y, y.y, z.y, w.y) - ((float4)(x.x, y.x, z.x, w.x) + (float4)(x.z, y.z, z.z, w.z));
}

static inline float clear(float2 crn, float2 a, float2 b)
{
    return (crn.x >= maxf(minf(a.x, a.y), minf(b.x, b.y))) && (crn.y >= maxf(minf(a.x, b.y), minf(b.x, a.y))) ? 1.0f : 0.0f;
}

static inline float4 junction(float4 jD)
{
    const float4 zero4 = (float4)(0.0f);
    return min41(GE4(jD, zero4) * (LEQ4(jD.yzwx, zero4) * LEQ4(jD.wxyz, zero4) + GE4(jD + jD.zwxy, jD.yzwx + jD.wxyz)));
}

kernel void sfx2(global const float4 *m0, global const float4 *s1, global uint *p2, uint w, uint h)
{
    int x = (int)TID_X, y = (int)TID_Y;
    if (x >= (int)w || y >= (int)h) return;
    const int W = (int)w - 1, H = (int)h - 1;
#define AT(dx, dy) (clampi(y + (dy), 0, H) * (int)w + clampi(x + (dx), 0, W))
    float4 A = m0[AT(-1, -1)], B = m0[AT(0, -1)];
    float4 D = m0[AT(-1, 0)], E = m0[AT(0, 0)], F = m0[AT(1, 0)];
    float4 G = m0[AT(-1, 1)], Hm = m0[AT(0, 1)], I = m0[AT(1, 1)];
    float4 As = s1[AT(-1, -1)], Bs = s1[AT(0, -1)], Cs = s1[AT(1, -1)];
    float4 Ds = s1[AT(-1, 0)], Es = s1[AT(0, 0)], Fs = s1[AT(1, 0)];
    float4 Gs = s1[AT(-1, 1)], Hs = s1[AT(0, 1)], Is = s1[AT(1, 1)];
#undef AT

    float4 jSx = (float4)(As.z, Bs.w, Es.x, Ds.y), jDx = dom(As.yzw, Bs.zwx, Es.wxy, Ds.xyz);
    float4 jSy = (float4)(Bs.z, Cs.w, Fs.x, Es.y), jDy = dom(Bs.yzw, Cs.zwx, Fs.wxy, Es.xyz);
    float4 jSz = (float4)(Es.z, Fs.w, Is.x, Hs.y), jDz = dom(Es.yzw, Fs.zwx, Is.wxy, Hs.xyz);
    float4 jSw = (float4)(Ds.z, Es.w, Hs.x, Gs.y), jDw = dom(Ds.yzw, Es.zwx, Hs.wxy, Gs.xyz);

    float4 jx = junction(jDx), jy = junction(jDy), jz = junction(jDz), jw = junction(jDw);

    float4 res;
    res.x = minf(jx.z + (1.0f - jx.y) * (1.0f - jx.w) * GEs(jSx.z, 0.0f) * (jx.x + GEs(jSx.x + jSx.z, jSx.y + jSx.w)), 1.0f);
    res.y = minf(jy.w + (1.0f - jy.z) * (1.0f - jy.x) * GEs(jSy.w, 0.0f) * (jy.y + GEs(jSy.y + jSy.w, jSy.x + jSy.z)), 1.0f);
    res.z = minf(jz.x + (1.0f - jz.w) * (1.0f - jz.y) * GEs(jSz.x, 0.0f) * (jz.z + GEs(jSz.x + jSz.z, jSz.y + jSz.w)), 1.0f);
    res.w = minf(jw.y + (1.0f - jw.x) * (1.0f - jw.z) * GEs(jSw.y, 0.0f) * (jw.w + GEs(jSw.y + jSw.w, jSw.x + jSw.z)), 1.0f);

    res = min41(res * ((float4)(jx.z, jy.w, jz.x, jw.y) + ((float4)(1.0f) - res.wxyz * res.yzwx)));

    float4 clr;
    clr.x = clear((float2)(D.z, E.x), (float2)(D.w, E.y), (float2)(A.w, D.y));
    clr.y = clear((float2)(F.x, E.z), (float2)(E.w, E.y), (float2)(B.w, F.y));
    clr.z = clear((float2)(Hm.z, I.x), (float2)(E.w, Hm.y), (float2)(Hm.w, I.y));
    clr.w = clear((float2)(Hm.x, G.z), (float2)(D.w, Hm.y), (float2)(G.w, G.y));

    float4 hh = (float4)(minf(D.w, A.w), minf(E.w, B.w), minf(E.w, Hm.w), minf(D.w, G.w));
    float4 vv = (float4)(minf(E.y, D.y), minf(E.y, F.y), minf(Hm.y, I.y), minf(Hm.y, G.y));

    float4 orien = GE4(hh + (float4)(D.w, E.w, E.w, D.w), vv + (float4)(E.y, E.y, Hm.y, Hm.y));
    float4 hori = LE4(hh, vv) * clr;
    float4 vert = GE4(hh, vv) * clr;

    /* 4 bits per corner: bit 0 corner, 1 horizontal edge, 2 vertical edge,
       3 orientation; corner k at bits 4k..4k+3 (x, y, z, w) */
    float4 bits = res + 2.0f * hori + 4.0f * vert + 8.0f * orien;
    p2[y * (int)w + x] = (uint)(bits.x + 0.5f) | ((uint)(bits.y + 0.5f) << 4) | ((uint)(bits.z + 0.5f) << 8) |
                         ((uint)(bits.w + 0.5f) << 12);
}

/* ------------------------------------------------------------ pass 3 */

#define cB(P, k) ((int)(((P) >> (4 * (k))) & 1u))
#define hB(P, k) ((int)(((P) >> (4 * (k) + 1)) & 1u))
#define vB(P, k) ((int)(((P) >> (4 * (k) + 2)) & 1u))
#define oB(P, k) ((int)(((P) >> (4 * (k) + 3)) & 1u))
#define X 0
#define Y 1
#define Z 2
#define Wc 3

kernel void sfx3(global const uint *p2, global uint *p3, uint w, uint h)
{
    int x = (int)TID_X, y = (int)TID_Y;
    if (x >= (int)w || y >= (int)h) return;
    const int Wd = (int)w - 1, Ht = (int)h - 1;
#define P(dx, dy) p2[clampi(y + (dy), 0, Ht) * (int)w + clampi(x + (dx), 0, Wd)]
    uint E = P(0, 0);
    uint D = P(-1, 0), D0 = P(-2, 0), D1 = P(-3, 0);
    uint F = P(1, 0), F0 = P(2, 0), F1 = P(3, 0);
    uint B = P(0, -1), B0 = P(0, -2), B1 = P(0, -3);
    uint H = P(0, 1), H0 = P(0, 2), H1 = P(0, 3);
#undef P

#define Ec(k) cB(E, k)
#define Eh(k) hB(E, k)
#define Ev(k) vB(E, k)
#define Eo(k) oB(E, k)
#define Dc(k) cB(D, k)
#define Dh(k) hB(D, k)
#define Do(k) oB(D, k)
#define D0c(k) cB(D0, k)
#define D0h(k) hB(D0, k)
#define D1h(k) hB(D1, k)
#define Fc(k) cB(F, k)
#define Fh(k) hB(F, k)
#define Fo(k) oB(F, k)
#define F0c(k) cB(F0, k)
#define F0h(k) hB(F0, k)
#define F1h(k) hB(F1, k)
#define Bc(k) cB(B, k)
#define Bv(k) vB(B, k)
#define Bo(k) oB(B, k)
#define B0c(k) cB(B0, k)
#define B0v(k) vB(B0, k)
#define B1v(k) vB(B1, k)
#define Hc(k) cB(H, k)
#define Hv(k) vB(H, k)
#define Ho(k) oB(H, k)
#define H0c(k) cB(H0, k)
#define H0v(k) vB(H0, k)
#define H1v(k) vB(H1, k)

    /* lvl1 corners (SFX_SCN == 1) */
    int lvl1x = Ec(X), lvl1y = Ec(Y), lvl1z = Ec(Z), lvl1w = Ec(Wc);

    /* lvl2 mid (left, right / up, down) */
    int lvl2x_x = (Ec(X) && Eh(Y)) && Dc(Z), lvl2x_y = (Ec(Y) && Eh(X)) && Fc(Wc);
    int lvl2y_x = (Ec(Y) && Ev(Z)) && Bc(Wc), lvl2y_y = (Ec(Z) && Ev(Y)) && Hc(X);
    int lvl2z_x = (Ec(Wc) && Eh(Z)) && Dc(Y), lvl2z_y = (Ec(Z) && Eh(Wc)) && Fc(X);
    int lvl2w_x = (Ec(X) && Ev(Wc)) && Bc(Z), lvl2w_y = (Ec(Wc) && Ev(X)) && Hc(Y);

    /* lvl3 corners (hori, vert) */
    int lvl3x_x = lvl2x_y && (Dh(Y) && Dh(X)) && Fh(Z), lvl3x_y = lvl2w_y && (Bv(Wc) && Bv(X)) && Hv(Z);
    int lvl3y_x = lvl2x_x && (Fh(X) && Fh(Y)) && Dh(Wc), lvl3y_y = lvl2y_y && (Bv(Z) && Bv(Y)) && Hv(Wc);
    int lvl3z_x = lvl2z_x && (Fh(Wc) && Fh(Z)) && Dh(X), lvl3z_y = lvl2y_x && (Hv(Y) && Hv(Z)) && Bv(X);
    int lvl3w_x = lvl2z_y && (Dh(Z) && Dh(Wc)) && Fh(Y), lvl3w_y = lvl2w_x && (Hv(X) && Hv(Wc)) && Bv(Y);

    /* lvl4 corners (hori, vert) */
    int lvl4x_x = (Dc(X) && Dh(Y) && Eh(X) && Eh(Y) && Fh(X) && Fh(Y)) && (D0c(Z) && D0h(Wc));
    int lvl4x_y = (Bc(X) && Bv(Wc) && Ev(X) && Ev(Wc) && Hv(X) && Hv(Wc)) && (B0c(Z) && B0v(Y));
    int lvl4y_x = (Fc(Y) && Fh(X) && Eh(Y) && Eh(X) && Dh(Y) && Dh(X)) && (F0c(Wc) && F0h(Z));
    int lvl4y_y = (Bc(Y) && Bv(Z) && Ev(Y) && Ev(Z) && Hv(Y) && Hv(Z)) && (B0c(Wc) && B0v(X));
    int lvl4z_x = (Fc(Z) && Fh(Wc) && Eh(Z) && Eh(Wc) && Dh(Z) && Dh(Wc)) && (F0c(X) && F0h(Y));
    int lvl4z_y = (Hc(Z) && Hv(Y) && Ev(Z) && Ev(Y) && Bv(Z) && Bv(Y)) && (H0c(X) && H0v(Wc));
    int lvl4w_x = (Dc(Wc) && Dh(Z) && Eh(Wc) && Eh(Z) && Fh(Wc) && Fh(Z)) && (D0c(Y) && D0h(X));
    int lvl4w_y = (Hc(Wc) && Hv(X) && Ev(Wc) && Ev(X) && Bv(Wc) && Bv(X)) && (H0c(Y) && H0v(Z));

    /* lvl5 mid (left, right / up, down) */
    int lvl5x_x = lvl4x_x && (F0h(X) && F0h(Y)) && (D1h(Z) && D1h(Wc));
    int lvl5x_y = lvl4y_x && (D0h(Y) && D0h(X)) && (F1h(Wc) && F1h(Z));
    int lvl5y_x = lvl4y_y && (H0v(Y) && H0v(Z)) && (B1v(Wc) && B1v(X));
    int lvl5y_y = lvl4z_y && (B0v(Z) && B0v(Y)) && (H1v(X) && H1v(Wc));
    int lvl5z_x = lvl4w_x && (F0h(Wc) && F0h(Z)) && (D1h(Y) && D1h(X));
    int lvl5z_y = lvl4z_x && (D0h(Z) && D0h(Wc)) && (F1h(X) && F1h(Y));
    int lvl5w_x = lvl4x_y && (H0v(X) && H0v(Wc)) && (B1v(Z) && B1v(Y));
    int lvl5w_y = lvl4w_y && (B0v(Wc) && B0v(X)) && (H1v(Y) && H1v(Z));

    /* lvl6 corners (hori, vert) */
    int lvl6x_x = lvl5x_y && (D1h(Y) && D1h(X)), lvl6x_y = lvl5w_y && (B1v(Wc) && B1v(X));
    int lvl6y_x = lvl5x_x && (F1h(X) && F1h(Y)), lvl6y_y = lvl5y_y && (B1v(Z) && B1v(Y));
    int lvl6z_x = lvl5z_x && (F1h(Wc) && F1h(Z)), lvl6z_y = lvl5y_x && (H1v(Y) && H1v(Z));
    int lvl6w_x = lvl5z_y && (D1h(Z) && D1h(Wc)), lvl6w_y = lvl5w_x && (H1v(X) && H1v(Wc));

    /* subpixels - 0 = E, 1 = D, 2 = D0, 3 = F, 4 = F0, 5 = B, 6 = B0, 7 = H, 8 = H0 */
    uint crn_x = (lvl1x && Eo(X) || lvl3x_x && Eo(Y) || lvl4x_x && Do(X) || lvl6x_x && Fo(Y)) ? 5
               : (lvl1x || lvl3x_y && !Eo(Wc) || lvl4x_y && !Bo(X) || lvl6x_y && !Ho(Wc)) ? 1
               : lvl3x_x ? 3 : lvl3x_y ? 7 : lvl4x_x ? 2 : lvl4x_y ? 6 : lvl6x_x ? 4 : lvl6x_y ? 8 : 0;
    uint crn_y = (lvl1y && Eo(Y) || lvl3y_x && Eo(X) || lvl4y_x && Fo(Y) || lvl6y_x && Do(X)) ? 5
               : (lvl1y || lvl3y_y && !Eo(Z) || lvl4y_y && !Bo(Y) || lvl6y_y && !Ho(Z)) ? 3
               : lvl3y_x ? 1 : lvl3y_y ? 7 : lvl4y_x ? 4 : lvl4y_y ? 6 : lvl6y_x ? 2 : lvl6y_y ? 8 : 0;
    uint crn_z = (lvl1z && Eo(Z) || lvl3z_x && Eo(Wc) || lvl4z_x && Fo(Z) || lvl6z_x && Do(Wc)) ? 7
               : (lvl1z || lvl3z_y && !Eo(Y) || lvl4z_y && !Ho(Z) || lvl6z_y && !Bo(Y)) ? 3
               : lvl3z_x ? 1 : lvl3z_y ? 5 : lvl4z_x ? 4 : lvl4z_y ? 8 : lvl6z_x ? 2 : lvl6z_y ? 6 : 0;
    uint crn_w = (lvl1w && Eo(Wc) || lvl3w_x && Eo(Z) || lvl4w_x && Do(Wc) || lvl6w_x && Fo(Z)) ? 7
               : (lvl1w || lvl3w_y && !Eo(X) || lvl4w_y && !Ho(Wc) || lvl6w_y && !Bo(X)) ? 1
               : lvl3w_x ? 3 : lvl3w_y ? 5 : lvl4w_x ? 2 : lvl4w_y ? 8 : lvl6w_x ? 4 : lvl6w_y ? 6 : 0;

    uint mid_x = (lvl2x_x && Eo(X) || lvl2x_y && Eo(Y) || lvl5x_x && Do(X) || lvl5x_y && Fo(Y)) ? 5
               : lvl2x_x ? 1 : lvl2x_y ? 3 : lvl5x_x ? 2 : lvl5x_y ? 4
               : (Ec(X) && Dc(Z) && Ec(Y) && Fc(Wc)) ? (Eo(X) ? (Eo(Y) ? 5 : 3) : 1) : 0;
    uint mid_y = (lvl2y_x && !Eo(Y) || lvl2y_y && !Eo(Z) || lvl5y_x && !Bo(Y) || lvl5y_y && !Ho(Z)) ? 3
               : lvl2y_x ? 5 : lvl2y_y ? 7 : lvl5y_x ? 6 : lvl5y_y ? 8
               : (Ec(Y) && Bc(Wc) && Ec(Z) && Hc(X)) ? (!Eo(Y) ? (!Eo(Z) ? 3 : 7) : 5) : 0;
    uint mid_z = (lvl2z_x && Eo(Wc) || lvl2z_y && Eo(Z) || lvl5z_x && Do(Wc) || lvl5z_y && Fo(Z)) ? 7
               : lvl2z_x ? 1 : lvl2z_y ? 3 : lvl5z_x ? 2 : lvl5z_y ? 4
               : (Ec(Z) && Fc(X) && Ec(Wc) && Dc(Y)) ? (Eo(Z) ? (Eo(Wc) ? 7 : 1) : 3) : 0;
    uint mid_w = (lvl2w_x && !Eo(X) || lvl2w_y && !Eo(Wc) || lvl5w_x && !Bo(X) || lvl5w_y && !Ho(Wc)) ? 1
               : lvl2w_x ? 5 : lvl2w_y ? 7 : lvl5w_x ? 6 : lvl5w_y ? 8
               : (Ec(Wc) && Hc(Y) && Ec(X) && Bc(Z)) ? (!Eo(Wc) ? (!Eo(X) ? 1 : 5) : 7) : 0;

    p3[y * (int)w + x] = crn_x | (crn_y << 4) | (crn_z << 8) | (crn_w << 12) | (mid_x << 16) | (mid_y << 20) |
                         (mid_z << 24) | (mid_w << 28);
}

/* ------------------------------------------------------------ pass 4 */

kernel void sfx4(global const uint *src, global const uint *p3, global uint *x3, uint w, uint h)
{
    uint ox = TID_X, oy = TID_Y;
    if (ox >= 3u * w || oy >= 3u * h) return;
    int sx = (int)(ox / 3u), sy = (int)(oy / 3u);
    uint fx = ox - (uint)sx * 3u, fy = oy - (uint)sy * 3u;
    uint E = p3[sy * (int)w + sx];
#define CRN(k) ((E >> (4 * (k))) & 15u)
#define MID(k) ((E >> (16 + 4 * (k))) & 15u)
    uint sp = fy == 0u ? (fx == 0u ? CRN(0) : fx == 1u ? MID(0) : CRN(1))
            : fy == 1u ? (fx == 0u ? MID(3) : fx == 1u ? 0u : MID(1))
                       : (fx == 0u ? CRN(3) : fx == 1u ? MID(2) : CRN(2));
#undef CRN
#undef MID
    int dx = 0, dy = 0;
    switch (sp) {
    case 1: dx = -1; break;
    case 2: dx = -2; break;
    case 3: dx = 1; break;
    case 4: dx = 2; break;
    case 5: dy = -1; break;
    case 6: dy = -2; break;
    case 7: dy = 1; break;
    case 8: dy = 2; break;
    default: break;
    }
    x3[oy * 3u * w + ox] = src[clampi(sy + dy, 0, (int)h - 1) * (int)w + clampi(sx + dx, 0, (int)w - 1)];
}

/* ------------------------------------------------------------ Lanczos2 */

/* cols[i]: the first of 4 taps for output column out_x + i; colw[i]: their
   weights (normalised); the same for rows. */
kernel void lanczos(global const uint *x3, global const int *cols, global const float4 *colw, global const int *rows,
                    global const float4 *roww, global uint *lz, uint w3, uint h3, uint out_w, uint out_h)
{
    uint ox = TID_X, oy = TID_Y;
    if (ox >= out_w || oy >= out_h) return;
    const int c0 = cols[ox], r0 = rows[oy];
    const float4 cw = colw[ox], rw = roww[oy];
    const float cwa[4] = {cw.x, cw.y, cw.z, cw.w}, rwa[4] = {rw.x, rw.y, rw.z, rw.w};
    float3 acc = (float3)(0.0f), lo = (float3)(1.0f), hi = (float3)(0.0f);
    for (int j = 0; j < 4; j++) {
        const int yy = clampi(r0 + j, 0, (int)h3 - 1);
        float3 row = (float3)(0.0f);
        for (int i = 0; i < 4; i++) {
            float3 c = rgbf(x3[yy * (int)w3 + clampi(c0 + i, 0, (int)w3 - 1)]);
            row += c * cwa[i];
            if ((i == 1 || i == 2) && (j == 1 || j == 2)) {
                lo = (float3)(minf(lo.x, c.x), minf(lo.y, c.y), minf(lo.z, c.z));
                hi = (float3)(maxf(hi.x, c.x), maxf(hi.y, c.y), maxf(hi.z, c.z));
            }
        }
        acc += row * rwa[j];
    }
    /* anti-ringing: stay within the 2x2 nearest texels */
    acc = (float3)(minf(maxf(acc.x, lo.x), hi.x), minf(maxf(acc.y, lo.y), hi.y), minf(maxf(acc.z, lo.z), hi.z));
    lz[oy * out_w + ox] = packf(acc);
}

/* ------------------------------------------------------------ CAS */

static inline float3 min3c(float3 a, float3 b) { return (float3)(minf(a.x, b.x), minf(a.y, b.y), minf(a.z, b.z)); }
static inline float3 max3c(float3 a, float3 b) { return (float3)(maxf(a.x, b.x), maxf(a.y, b.y), maxf(a.z, b.z)); }

kernel void cas(global const uint *lz, global uint *dst, uint out_x, uint out_w, uint out_h, uint pitch, uint dim,
                float sharpness)
{
    uint ox = TID_X, oy = TID_Y;
    if (ox >= pitch || oy >= out_h) return;
    global uint *out = dst + oy * pitch + ox;
    if (ox < out_x || ox >= out_x + out_w) {
        *out = 0xFF000000u;
        return;
    }
    const int x = (int)(ox - out_x), y = (int)oy, W = (int)out_w - 1, H = (int)out_h - 1;
#define L(dx, dy) rgbf(lz[clampi(y + (dy), 0, H) * (int)out_w + clampi(x + (dx), 0, W)])
    float3 a = L(-1, -1), b = L(0, -1), c = L(1, -1);
    float3 d = L(-1, 0), e = L(0, 0), f = L(1, 0);
    float3 g = L(-1, 1), h = L(0, 1), i = L(1, 1);
#undef L
    float3 mn = min3c(min3c(min3c(d, e), min3c(f, b)), h);
    float3 mn2 = min3c(min3c(min3c(mn, a), min3c(c, g)), i);
    mn += mn2;
    float3 mx = max3c(max3c(max3c(d, e), max3c(f, b)), h);
    float3 mx2 = max3c(max3c(max3c(mx, a), max3c(c, g)), i);
    mx += mx2;
    float3 amp;
    amp.x = sat(minf(mn.x, 2.0f - mx.x) / maxf(mx.x, 1e-5f));
    amp.y = sat(minf(mn.y, 2.0f - mx.y) / maxf(mx.y, 1e-5f));
    amp.z = sat(minf(mn.z, 2.0f - mx.z) / maxf(mx.z, 1e-5f));
    amp = (float3)(__builtin_sqrtf(amp.x), __builtin_sqrtf(amp.y), __builtin_sqrtf(amp.z));
    const float peak = -1.0f / (8.0f - 3.0f * sharpness);
    float3 wgt = amp * peak;
    float3 window = (b + d) + (f + h);
    float3 res = (window * wgt + e) / ((float3)(1.0f) + 4.0f * wgt);
    if (dim) res *= 0.25f;
    *out = packf(res);
}
