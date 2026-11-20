/* The PS4 host's screen scaler: one thread per 1920x1080 output pixel.

   src: the PPU frame, src_w x src_h words 0x00RRGGBB (garlic memory).
   dst: the sceVideoOut buffer being filled (linear, pitch words per row).
   The picture covers columns out_x .. out_x + out_w - 1 and all rows;
   everything else is black.

   mode 0, PIXELS: sharp bilinear. Each SNES pixel is a crisp block; only
   the one output pixel on a block edge is blended, so every block has the
   same size even though 1080 / 224 is not an integer.
   mode 1, HD: xBR level 2 (Hyllian's xbr-lv2 algorithm, corner type C) at
   the real output scale: edges between flat colours are detected in a 5x5
   neighbourhood and redrawn as smooth 30/45/60 degree lines.
   dim: the host menu is open, the picture is drawn at a quarter brightness. */

static inline float clampf(float v, float lo, float hi) { return fmin(fmax(v, lo), hi); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

typedef struct {
    float r, g, b;
} rgb;

static inline rgb unpack(uint c)
{
    rgb o = {(float)((c >> 16) & 255u), (float)((c >> 8) & 255u), (float)(c & 255u)};
    return o;
}

static inline uint pack(rgb c, uint dim)
{
    float k = dim ? 0.25f : 1.0f;
    uint r = (uint)clampf(c.r * k + 0.5f, 0.0f, 255.0f);
    uint g = (uint)clampf(c.g * k + 0.5f, 0.0f, 255.0f);
    uint b = (uint)clampf(c.b * k + 0.5f, 0.0f, 255.0f);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static inline rgb mixc(rgb a, rgb b, float t)
{
    rgb o = {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
    return o;
}

static inline uint fetch(global const uint *src, int x, int y, int w, int h)
{
    x = clampi(x, 0, w - 1);
    y = clampi(y, 0, h - 1);
    return src[y * w + x];
}

/* luma on a 0..48 scale, as xBR weighs it (Rec. 709 x 48) */
static inline float luma(uint c)
{
    return (float)((c >> 16) & 255u) * (10.2048f / 255.0f) +
           (float)((c >> 8) & 255u) * (34.3296f / 255.0f) +
           (float)(c & 255u) * (3.4656f / 255.0f);
}

#define EQ_THRESHOLD 15.0f
#define LV2_COEF 2.0f

static inline int eqv(float a, float b) { return fabs(a - b) < EQ_THRESHOLD; }
static inline int neqv(float a, float b) { return !(fabs(a - b) < EQ_THRESHOLD); }
static inline float df(float a, float b) { return fabs(a - b); }
static inline float cdf(rgb a, rgb b) { return fabs(a.r - b.r) + fabs(a.g - b.g) + fabs(a.b - b.b); }
static inline float sat(float v) { return clampf(v, 0.0f, 1.0f); }

static inline rgb sharp(global const uint *src, float u, float v, int w, int h, float sx, float sy)
{
    float uu = u - 0.5f, vv = v - 0.5f;
    float ix = floor(uu), iy = floor(vv);
    float fx = sat((uu - ix - 0.5f) * sx + 0.5f);
    float fy = sat((vv - iy - 0.5f) * sy + 0.5f);
    int x = (int)ix, y = (int)iy;
    rgb a = unpack(fetch(src, x, y, w, h)), b = unpack(fetch(src, x + 1, y, w, h));
    rgb c = unpack(fetch(src, x, y + 1, w, h)), d = unpack(fetch(src, x + 1, y + 1, w, h));
    return mixc(mixc(a, b, fx), mixc(c, d, fx), fy);
}

static inline __attribute__((always_inline)) rgb xbr(global const uint *src, float u, float v, int w, int h, float scale)
{
    int x = (int)floor(u), y = (int)floor(v);
    float fpx = u - (float)x, fpy = v - (float)y;

    /*      A1 B1 C1
         A0 A  B  C  C4
         D0 D  E  F  F4
         G0 G  H  I  I4
            G5 H5 I5      */
    uint cA1 = fetch(src, x - 1, y - 2, w, h), cB1 = fetch(src, x, y - 2, w, h), cC1 = fetch(src, x + 1, y - 2, w, h);
    uint cA0 = fetch(src, x - 2, y - 1, w, h), cA = fetch(src, x - 1, y - 1, w, h), cB = fetch(src, x, y - 1, w, h);
    uint cC = fetch(src, x + 1, y - 1, w, h), cC4 = fetch(src, x + 2, y - 1, w, h);
    uint cD0 = fetch(src, x - 2, y, w, h), cD = fetch(src, x - 1, y, w, h), cE = fetch(src, x, y, w, h);
    uint cF = fetch(src, x + 1, y, w, h), cF4 = fetch(src, x + 2, y, w, h);
    uint cG0 = fetch(src, x - 2, y + 1, w, h), cG = fetch(src, x - 1, y + 1, w, h), cH = fetch(src, x, y + 1, w, h);
    uint cI = fetch(src, x + 1, y + 1, w, h), cI4 = fetch(src, x + 2, y + 1, w, h);
    uint cG5 = fetch(src, x - 1, y + 2, w, h), cH5 = fetch(src, x, y + 2, w, h), cI5 = fetch(src, x + 1, y + 2, w, h);

    float A1 = luma(cA1), B1 = luma(cB1), C1 = luma(cC1), A0 = luma(cA0), A = luma(cA), B = luma(cB);
    float C = luma(cC), C4 = luma(cC4), D0 = luma(cD0), D = luma(cD), E = luma(cE), F = luma(cF);
    float F4 = luma(cF4), G0 = luma(cG0), G = luma(cG), H = luma(cH), I = luma(cI), I4 = luma(cI4);
    float G5 = luma(cG5), H5 = luma(cH5), I5 = luma(cI5);

    /* the four corners of E, each as xBR's bottom-right case rotated */
    const float b[4] = {B, D, H, F}, c[4] = {C, A, G, I}, d[4] = {D, H, F, B};
    const float f[4] = {F, B, D, H}, g[4] = {G, I, C, A}, hh[4] = {H, F, B, D};
    const float i[4] = {I, C, A, G}, i4[4] = {I4, C1, A0, G5}, i5[4] = {I5, C4, A1, G0};
    const float h5[4] = {H5, F4, B1, D0}, f4[4] = {F4, B1, D0, H5};

    const float Ao[4] = {1.0f, -1.0f, -1.0f, 1.0f}, Bo[4] = {1.0f, 1.0f, -1.0f, -1.0f}, Co[4] = {1.5f, 0.5f, -0.5f, 0.5f};
    const float Bx[4] = {0.5f, 2.0f, -0.5f, -2.0f}, Cx[4] = {1.0f, 1.0f, -0.5f, 0.0f};
    const float By[4] = {2.0f, 0.5f, -2.0f, -0.5f}, Cy[4] = {2.0f, 0.0f, -1.0f, 0.5f};
    const float delta = 1.0f / scale;
    const float deltaL[4] = {0.5f * delta, delta, 0.5f * delta, delta};
    const float deltaU[4] = {delta, 0.5f * delta, delta, 0.5f * delta};

    float maximos[4];
    int px[4];
    for (int k = 0; k < 4; k++) {
        float fx = Ao[k] * fpy + Bo[k] * fpx;
        float fx_left = Ao[k] * fpy + Bx[k] * fpx;
        float fx_up = Ao[k] * fpy + By[k] * fpx;

        int lv0 = (E != f[k]) && (E != hh[k]);
        int lv1 = lv0 && ((neqv(f[k], b[k]) && neqv(f[k], c[k])) || (neqv(hh[k], d[k]) && neqv(hh[k], g[k])) ||
                          (eqv(E, i[k]) && ((neqv(f[k], f4[k]) && neqv(f[k], i4[k])) ||
                                             (neqv(hh[k], h5[k]) && neqv(hh[k], i5[k])))) ||
                          eqv(E, g[k]) || eqv(E, c[k]));
        int lv2_left = (E != g[k]) && (d[k] != g[k]);
        int lv2_up = (E != c[k]) && (b[k] != c[k]);

        float fx45i = sat((fx + delta - Co[k] - 0.25f) / (2.0f * delta));
        float fx45 = sat((fx + delta - Co[k]) / (2.0f * delta));
        float fx30 = sat((fx_left + deltaL[k] - Cx[k]) / (2.0f * deltaL[k]));
        float fx60 = sat((fx_up + deltaU[k] - Cy[k]) / (2.0f * deltaU[k]));

        float wd1 = df(E, c[k]) + df(E, g[k]) + df(i[k], h5[k]) + df(i[k], f4[k]) + 4.0f * df(hh[k], f[k]);
        float wd2 = df(hh[k], d[k]) + df(hh[k], i5[k]) + df(f[k], i4[k]) + df(f[k], b[k]) + 4.0f * df(E, i[k]);

        int edri = (wd1 <= wd2) && lv0;
        int edr = (wd1 < wd2) && lv1;
        int edr_left = (LV2_COEF * df(f[k], g[k]) <= df(hh[k], c[k])) && lv2_left && edr;
        int edr_up = (df(f[k], g[k]) >= LV2_COEF * df(hh[k], c[k])) && lv2_up && edr;

        fx45 = edr ? fx45 : 0.0f;
        fx30 = edr_left ? fx30 : 0.0f;
        fx60 = edr_up ? fx60 : 0.0f;
        fx45i = edri ? fx45i : 0.0f;

        px[k] = df(E, f[k]) <= df(E, hh[k]);
        maximos[k] = fmax(fmax(fx30, fx60), fmax(fx45, fx45i));
    }

    rgb e = unpack(cE);
    rgb rb = unpack(cB), rd = unpack(cD), rf = unpack(cF), rh = unpack(cH);
    rgb res1 = mixc(e, px[0] ? rf : rh, maximos[0]);
    res1 = mixc(res1, px[2] ? rd : rb, maximos[2]);
    rgb res2 = mixc(e, px[1] ? rb : rf, maximos[1]);
    res2 = mixc(res2, px[3] ? rh : rd, maximos[3]);
    return cdf(e, res2) >= cdf(e, res1) ? res2 : res1;
}

kernel void scale(global const uint *src, global uint *dst, uint src_w, uint src_h, uint out_x, uint out_w,
                  uint out_h, uint pitch, uint mode, uint dim)
{
    uint ox = __builtin_amdgcn_workgroup_id_x() * 8u + __builtin_amdgcn_workitem_id_x();
    uint oy = __builtin_amdgcn_workgroup_id_y() * 8u + __builtin_amdgcn_workitem_id_y();
    if (oy >= out_h || ox >= pitch) return;
    global uint *out = dst + oy * pitch + ox;
    if (ox < out_x || ox >= out_x + out_w) {
        *out = 0xFF000000u;
        return;
    }
    const int w = (int)src_w, h = (int)src_h;
    const float sx = (float)out_w / (float)src_w, sy = (float)out_h / (float)src_h;
    const float u = ((float)(ox - out_x) + 0.5f) / sx;
    const float v = ((float)oy + 0.5f) / sy;
    rgb c = mode == 1u ? xbr(src, u, v, w, h, sy) : sharp(src, u, v, w, h, sx, sy);
    *out = pack(c, dim);
}
