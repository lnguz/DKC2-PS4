/*
 * DKC2Recomp PS4 host (OpenOrbis).
 *
 * Runs the portable snesrecomp runtime with native console backends:
 *  - video: the PPU frame (256/308/342 x 224, 0x00RRGGBB) scaled with
 *    nearest-neighbour maps to 1920x1080 sceVideoOut buffers. SNES pixels
 *    keep the 8:7 shape of a 4:3 TV, so 16:9 (342 px) fills the screen.
 *  - audio: 32,040 Hz game audio resampled to 48 kHz into a ring buffer;
 *    a thread feeds sceAudioOut. The resample ratio follows the ring fill
 *    so the game can stay locked to the 60 Hz display.
 *  - input: DualShock 4 of the first two logged-in users (players 1-2).
 *  - R3 opens a small host menu (aspect, edges, save states, exit).
 *
 * Everything mutable lives in /data/DKC2 (ROM, SRAM, states, config, log).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/AudioOut.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "dkc2_game.h"
#include "dkc2_kongs.h"
#include "dkc2_music.h"
#include "dkc2_video.h"
#include "verified_rom.h"

#include "font.h"
#include "gnm.h"

/* Real prototypes: some OpenOrbis headers declare these without parameters. */
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
typedef struct {
  int32_t event_num;
  uint8_t is_system_ui_overlaid;
  uint8_t is_in_background_execution;
  uint8_t is_cpu_mode7_cpu_normal;
  uint8_t is_game_live_streaming_on_air;
  uint8_t is_out_of_vr_play_area;
  uint8_t reserved[125];
} Ps4SystemStatus; /* layout from shadPS4 */
int sceSystemServiceGetStatus(Ps4SystemStatus *status);

#define DATA_DIR "/data/DKC2"
#define LOG_PATH DATA_DIR "/debug.log"
#define CONFIG_PATH DATA_DIR "/ps4.cfg"
#define VERSION_TEXT "1.02"

/* ------------------------------------------------------------------ log */

static FILE *g_log;

static void Log(const char *format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof line, format, args);
  va_end(args);
  if (g_log) {
    fprintf(g_log, "[%8.3f] %s\n",
            (double)sceKernelGetProcessTime() / 1000000.0, line);
    fflush(g_log);
  }
}

static void OpenLog(void) {
  mkdir(DATA_DIR, 0777);
  g_log = fopen(LOG_PATH, "w");
  /* The runtime reports through stdout/stderr. The console refuses dup2 onto
   * descriptors 1 and 2, so the descriptor inside musl's FILE (offset 0x78,
   * the field fileno() reads; validated in the Downhill port) is pointed at
   * the log instead, only when the layout checks out. */
  if (g_log) {
    int fd = open(LOG_PATH, O_WRONLY | O_APPEND);
    if (fd >= 0 && fileno(stderr) == 2 && fileno(stdout) == 1 &&
        *(int *)((char *)stderr + 0x78) == 2 &&
        *(int *)((char *)stdout + 0x78) == 1) {
      fflush(stdout);
      fflush(stderr);
      *(int *)((char *)stderr + 0x78) = fd;
      *(int *)((char *)stdout + 0x78) = fd;
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
  }
}

/* --------------------------------------------------------------- config */

typedef struct {
  int aspect;  /* index into kAspects */
  int edge;    /* Dkc2VideoEdgePolicy */
  int slot;    /* 0..4 */
  int filter;  /* kFilterPixels or kFilterHd (GPU scaler) */
} Ps4Config;

enum { kFilterPixels, kFilterHd, kFilterCount };
static const char *const kFilterNames[] = {"PIXELES", "HD"};

static const Dkc2VideoAspect kAspects[] = {
    kDkc2VideoAspect16x9, kDkc2VideoAspect16x10, kDkc2VideoAspectNative};
static const char *const kAspectNames[] = {"16:9", "16:10", "4:3"};
enum { kAspectCount = 3 };
static const char *const kEdgeNames[] = {"REFLEJAR", "BARRAS", "DESPLAZAR",
                                         "DESLIZAR"};

static Ps4Config g_config = {0, kDkc2VideoEdgeGlide, 0, kFilterHd};

static void LoadConfig(void) {
  FILE *f = fopen(CONFIG_PATH, "r");
  if (!f) return;
  char key[32];
  int value;
  while (fscanf(f, "%31[^=]=%d\n", key, &value) == 2) {
    if (!strcmp(key, "aspect") && value >= 0 && value < kAspectCount)
      g_config.aspect = value;
    else if (!strcmp(key, "edge") && value >= 0 &&
             value < kDkc2VideoEdgePolicyCount)
      g_config.edge = value;
    else if (!strcmp(key, "slot") && value >= 0 && value < 5)
      g_config.slot = value;
    else if (!strcmp(key, "filter") && value >= 0 && value < kFilterCount)
      g_config.filter = value;
  }
  fclose(f);
}

static void SaveConfig(void) {
  FILE *f = fopen(CONFIG_PATH, "w");
  if (!f) return;
  fprintf(f, "aspect=%d\nedge=%d\nslot=%d\nfilter=%d\n", g_config.aspect,
          g_config.edge, g_config.slot, g_config.filter);
  fclose(f);
}

/* ---------------------------------------------------------------- video */

enum {
  kDisplayWidth = 1920,
  kDisplayHeight = 1080,
  kDisplayBuffers = 3,
  kDisplayBufferSize = kDisplayWidth * kDisplayHeight * 4,
  kDirectAlign = 0x200000,
  kSourceMaxWidth = 448,
  kSourceHeight = 224,
};

static int g_video = -1;
static OrbisKernelEqueue g_flip_queue;
static uint32_t *g_display[kDisplayBuffers];
static int g_current_buffer;
static int64_t g_flip_count;
static uint16_t g_column_map[kDisplayWidth];
static uint16_t g_row_map[kDisplayHeight];
static int g_out_x, g_out_width, g_mapped_width = -1;
static int g_buffer_out_x[kDisplayBuffers];

static bool VideoInit(void) {
  g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0,
                            NULL);
  if (g_video < 0) {
    Log("sceVideoOutOpen failed: 0x%08X", (unsigned)g_video);
    return false;
  }
  if (sceKernelCreateEqueue(&g_flip_queue, "dkc2 flip") != 0) return false;
  sceVideoOutAddFlipEvent(g_flip_queue, g_video, NULL);

  size_t size = ((size_t)kDisplayBufferSize * kDisplayBuffers + kDirectAlign -
                 1) / kDirectAlign * kDirectAlign;
  off_t offset;
  void *memory = NULL;
  if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size,
                                    kDirectAlign, 3, &offset) != 0 ||
      sceKernelMapDirectMemory(&memory, size, 0x33, 0, offset,
                               kDirectAlign) != 0) {
    Log("display memory allocation failed");
    return false;
  }
  for (int i = 0; i < kDisplayBuffers; i++) {
    g_display[i] = (uint32_t *)((char *)memory + (size_t)i * kDisplayBufferSize);
    memset(g_display[i], 0, kDisplayBufferSize);
    g_buffer_out_x[i] = -1;
  }
  OrbisVideoOutBufferAttribute attribute;
  memset(&attribute, 0, sizeof attribute);
  sceVideoOutSetBufferAttribute(&attribute, 0x80000000, 1, 0, kDisplayWidth,
                                kDisplayHeight, kDisplayWidth);
  if (sceVideoOutRegisterBuffers(g_video, 0, (void **)g_display,
                                 kDisplayBuffers, &attribute) != 0) {
    Log("sceVideoOutRegisterBuffers failed");
    return false;
  }
  sceVideoOutSetFlipRate(g_video, 0);
  for (int y = 0; y < kDisplayHeight; y++)
    g_row_map[y] = (uint16_t)(y * kSourceHeight / kDisplayHeight);
  return true;
}

static void WaitForFlip(int64_t wanted) {
  if (wanted <= 0) return;
  for (;;) {
    OrbisVideoOutFlipStatus status;
    if (sceVideoOutGetFlipStatus(g_video, &status) != 0 ||
        (int64_t)status.flipArg >= wanted)
      return;
    OrbisKernelEvent event;
    int count;
    if (sceKernelWaitEqueue(g_flip_queue, &event, 1, &count, NULL) != 0)
      return;
  }
}

/* SNES pixels are 8:7: 256 px span 1440 of 1080 lines (4:3). */
static void MapColumns(int source_width) {
  int width = source_width * 1440 / 256;
  if (width > kDisplayWidth) width = kDisplayWidth;
  g_out_width = width;
  g_out_x = (kDisplayWidth - width) / 2;
  for (int x = 0; x < width; x++)
    g_column_map[x] = (uint16_t)(x * source_width / width);
  g_mapped_width = source_width;
}

/* Returns the buffer being filled; FinishFrame flips it. */
static uint32_t *BeginFrame(void) {
  WaitForFlip(g_flip_count - 1); /* at most one flip in flight */
  return g_display[g_current_buffer];
}

static void FinishFrame(void) {
  g_flip_count++;
  sceVideoOutSubmitFlip(g_video, g_current_buffer, ORBIS_VIDEO_OUT_FLIP_VSYNC,
                        g_flip_count);
  g_current_buffer = (g_current_buffer + 1) % kDisplayBuffers;
}

/* Frames travel from the emulation thread to the presenter through a triple
 * buffer: the emulator draws into one, the presenter reads another, and the
 * third holds the newest finished frame. */
typedef struct {
  uint32_t pixels[kSourceMaxWidth * kSourceHeight];
  int width;
} Frame;

static Frame g_frames[3];
static int g_frame_draw = 0, g_frame_ready = 1, g_frame_show = 2;
static bool g_frame_fresh;
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;

/* Emulation side: the frame just drawn becomes the newest, and the PPU
 * draws the next one into the spare buffer. */
static void PublishFrame(void) {
  pthread_mutex_lock(&g_frame_lock);
  g_frames[g_frame_draw].width = Dkc2VideoWidth();
  int finished = g_frame_draw;
  g_frame_draw = g_frame_ready;
  g_frame_ready = finished;
  g_frame_fresh = true;
  pthread_mutex_unlock(&g_frame_lock);
  Dkc2BeginDrawing((uint8_t *)g_frames[g_frame_draw].pixels,
                   (size_t)Dkc2VideoWidth() * 4);
}

/* Presenter side: the newest frame (or the last one again). */
static const Frame *TakeFrame(void) {
  pthread_mutex_lock(&g_frame_lock);
  if (g_frame_fresh) {
    int newest = g_frame_ready;
    g_frame_ready = g_frame_show;
    g_frame_show = newest;
    g_frame_fresh = false;
  }
  pthread_mutex_unlock(&g_frame_lock);
  return &g_frames[g_frame_show];
}

static void DrawGame(uint32_t *display, const Frame *frame, bool dim) {
  const int source_width = frame->width;
  const uint32_t *pixels = frame->pixels;
  if (source_width != g_mapped_width) MapColumns(source_width);
  if (g_buffer_out_x[g_current_buffer] != g_out_x) {
    memset(display, 0, kDisplayBufferSize); /* clear stale side bars */
    g_buffer_out_x[g_current_buffer] = g_out_x;
  }
  static uint32_t row[kDisplayWidth];
  int converted = -1;
  uint32_t *destination = display + g_out_x;
  for (int y = 0; y < kDisplayHeight; y++) {
    if (g_row_map[y] != converted) {
      converted = g_row_map[y];
      const uint32_t *source = pixels + (size_t)converted * source_width;
      if (dim) {
        for (int x = 0; x < g_out_width; x++)
          row[x] = 0xFF000000u | ((source[g_column_map[x]] >> 2) & 0x3F3F3Fu);
      } else {
        for (int x = 0; x < g_out_width; x++)
          row[x] = 0xFF000000u | source[g_column_map[x]];
      }
    }
    memcpy(destination, row, (size_t)g_out_width * 4);
    destination += kDisplayWidth;
  }
}

/* GPU scaler.
 *  PIXELS: ps4/kernels/scale.cl mode 0 (sharp bilinear), one dispatch.
 *  HD: ps4/kernels/scalefx.cl, ScaleFX 3x (5 passes) -> Lanczos2 to the
 *      output size -> CAS sharpening into the display buffer.
 * Falls back to DrawGame on the CPU when the GPU path is unavailable. */
static bool g_gpu, g_gpu_hd;
static GnmKernel g_scale_kernel, g_sfx_kernel[5], g_lanczos_kernel, g_cas_kernel;
static uint32_t *g_gpu_source;
static uint64_t g_gpu_wait_us;

enum { kHdScale = 3, kLanczosTaps = 4 };
static const float kCasSharpness = 0.5f;

typedef struct { float v[4]; } Taps;
static float (*g_sfx_m0)[4], (*g_sfx_s1)[4];
static uint32_t *g_sfx_p2, *g_sfx_p3, *g_sfx_x3, *g_lanczos_out;
static int32_t *g_lz_cols, *g_lz_rows;
static Taps *g_lz_colw, *g_lz_roww;
static int g_lz_source_width = -1, g_lz_out_width = -1;

static void *GarlicAlloc(size_t size) { return gnm_alloc_garlic(size, 256); }

static void GpuInit(void) {
  g_gpu = gnm_init() && gnm_kernel("scale", &g_scale_kernel) &&
          (g_gpu_source = (uint32_t *)GarlicAlloc(sizeof g_frames[0].pixels));
  if (g_gpu) {
    static const char *const names[5] = {"sfx0", "sfx1", "sfx2", "sfx3", "sfx4"};
    const size_t texels = (size_t)kSourceMaxWidth * kSourceHeight;
    g_gpu_hd = gnm_kernel("lanczos", &g_lanczos_kernel) &&
               gnm_kernel("cas", &g_cas_kernel);
    for (int i = 0; i < 5; i++)
      g_gpu_hd = g_gpu_hd && gnm_kernel(names[i], &g_sfx_kernel[i]);
    g_gpu_hd = g_gpu_hd &&
        (g_sfx_m0 = GarlicAlloc(texels * 16)) && (g_sfx_s1 = GarlicAlloc(texels * 16)) &&
        (g_sfx_p2 = GarlicAlloc(texels * 4)) && (g_sfx_p3 = GarlicAlloc(texels * 4)) &&
        (g_sfx_x3 = GarlicAlloc(texels * kHdScale * kHdScale * 4)) &&
        (g_lanczos_out = GarlicAlloc((size_t)kDisplayWidth * kDisplayHeight * 4)) &&
        (g_lz_cols = GarlicAlloc(kDisplayWidth * 4)) &&
        (g_lz_colw = GarlicAlloc(kDisplayWidth * sizeof(Taps))) &&
        (g_lz_rows = GarlicAlloc(kDisplayHeight * 4)) &&
        (g_lz_roww = GarlicAlloc(kDisplayHeight * sizeof(Taps)));
  }
  Log("GPU scaler: %s, HD %s", g_gpu ? "on" : "off (CPU nearest)",
      g_gpu_hd ? "ScaleFX + Lanczos + CAS" : "xBR");
}

/* Lanczos2 taps from a source of `source` texels onto `out` pixels. */
static float Lanczos2(float x) {
  const float pi = 3.14159265358979f;
  if (x < 0) x = -x;
  if (x < 1e-5f) return 1.0f;
  if (x >= 2.0f) return 0.0f;
  return 2.0f * sinf(pi * x) * sinf(pi * x * 0.5f) / (pi * pi * x * x);
}

static void LanczosTable(int source, int out, int32_t *first, Taps *weights) {
  for (int i = 0; i < out; i++) {
    const float u = ((float)i + 0.5f) * (float)source / (float)out - 0.5f;
    const float base = floorf(u);
    const float f = u - base;
    Taps t;
    float sum = 0.0f;
    for (int k = 0; k < kLanczosTaps; k++) {
      t.v[k] = Lanczos2(f - (float)(k - 1));
      sum += t.v[k];
    }
    for (int k = 0; k < kLanczosTaps; k++) t.v[k] /= sum;
    first[i] = (int32_t)base - 1;
    weights[i] = t; /* sequential writes: fine for write-combined memory */
  }
}

static void DispatchGrid(const GnmKernel *kernel, const void *args, size_t size,
                         int width, int height) {
  gnm_dispatch(kernel, args, (uint32_t)size, (uint32_t)(width + 7) / 8,
               (uint32_t)(height + 7) / 8, 8, 8);
}

static void QueueHd(uint32_t *display, int w, bool dim) {
  const int h = kSourceHeight;
  if (w != g_lz_source_width || g_out_width != g_lz_out_width) {
    LanczosTable(w * kHdScale, g_out_width, g_lz_cols, g_lz_colw);
    LanczosTable(h * kHdScale, kDisplayHeight, g_lz_rows, g_lz_roww);
    g_lz_source_width = w;
    g_lz_out_width = g_out_width;
  }
#define P(x) ((uint64_t)(uintptr_t)(x))
  struct { uint64_t a, b; uint32_t w, h; } a0 = {P(g_gpu_source), P(g_sfx_m0), w, h};
  struct { uint64_t a, b; uint32_t w, h; } a1 = {P(g_sfx_m0), P(g_sfx_s1), w, h};
  struct { uint64_t a, b, c; uint32_t w, h; } a2 = {P(g_sfx_m0), P(g_sfx_s1), P(g_sfx_p2), w, h};
  struct { uint64_t a, b; uint32_t w, h; } a3 = {P(g_sfx_p2), P(g_sfx_p3), w, h};
  struct { uint64_t a, b, c; uint32_t w, h; } a4 = {P(g_gpu_source), P(g_sfx_p3), P(g_sfx_x3), w, h};
  struct {
    uint64_t x3, cols, colw, rows, roww, out;
    uint32_t w3, h3, out_w, out_h;
  } al = {P(g_sfx_x3), P(g_lz_cols), P(g_lz_colw), P(g_lz_rows), P(g_lz_roww),
          P(g_lanczos_out), (uint32_t)(w * kHdScale), (uint32_t)(h * kHdScale),
          (uint32_t)g_out_width, kDisplayHeight};
  struct {
    uint64_t lz, dst;
    uint32_t out_x, out_w, out_h, pitch, dim;
    float sharpness;
  } ac = {P(g_lanczos_out), P(display), (uint32_t)g_out_x, (uint32_t)g_out_width,
          kDisplayHeight, kDisplayWidth, dim ? 1u : 0u, kCasSharpness};
#undef P
  DispatchGrid(&g_sfx_kernel[0], &a0, sizeof a0, w, h);
  gnm_barrier();
  DispatchGrid(&g_sfx_kernel[1], &a1, sizeof a1, w, h);
  gnm_barrier();
  DispatchGrid(&g_sfx_kernel[2], &a2, sizeof a2, w, h);
  gnm_barrier();
  DispatchGrid(&g_sfx_kernel[3], &a3, sizeof a3, w, h);
  gnm_barrier();
  DispatchGrid(&g_sfx_kernel[4], &a4, sizeof a4, w * kHdScale, h * kHdScale);
  gnm_barrier();
  DispatchGrid(&g_lanczos_kernel, &al, sizeof al, g_out_width, kDisplayHeight);
  gnm_barrier();
  DispatchGrid(&g_cas_kernel, &ac, sizeof ac, kDisplayWidth, kDisplayHeight);
}

static bool DrawGameGpu(uint32_t *display, const Frame *frame, bool dim) {
  const int source_width = frame->width;
  if (source_width != g_mapped_width) MapColumns(source_width);
  memcpy(g_gpu_source, frame->pixels, (size_t)source_width * kSourceHeight * 4);
  struct {
    uint64_t src, dst;
    uint32_t src_w, src_h, out_x, out_w, out_h, pitch, mode, dim;
  } args = {(uint64_t)(uintptr_t)g_gpu_source, (uint64_t)(uintptr_t)display,
            (uint32_t)source_width, kSourceHeight, (uint32_t)g_out_x,
            (uint32_t)g_out_width, kDisplayHeight, kDisplayWidth,
            (uint32_t)g_config.filter, dim ? 1u : 0u};
  const uint64_t start = sceKernelGetProcessTime();
  gnm_begin();
  if (g_config.filter == kFilterHd && g_gpu_hd)
    QueueHd(display, source_width, dim);
  else
    gnm_dispatch(&g_scale_kernel, &args, sizeof args, kDisplayWidth / 8,
                 (kDisplayHeight + 7) / 8, 8, 8);
  if (!gnm_finish()) {
    Log("GPU scaler failed: falling back to the CPU");
    g_gpu = false;
    return false;
  }
  g_gpu_wait_us += sceKernelGetProcessTime() - start;
  g_buffer_out_x[g_current_buffer] = -1; /* the GPU wrote the side bars */
  return true;
}

/* Text: 5x7 glyphs drawn as scale x scale blocks (write-only to WC memory). */
static void FillRect(uint32_t *display, int x, int y, int w, int h,
                     uint32_t color) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > kDisplayWidth) w = kDisplayWidth - x;
  if (y + h > kDisplayHeight) h = kDisplayHeight - y;
  for (int j = 0; j < h; j++) {
    uint32_t *p = display + (size_t)(y + j) * kDisplayWidth + x;
    for (int i = 0; i < w; i++) p[i] = color;
  }
}

static int TextWidth(const char *text, int scale) {
  return (int)strlen(text) * 6 * scale - scale;
}

static void DrawText(uint32_t *display, int x, int y, int scale,
                     uint32_t color, const char *text) {
  for (; *text; text++, x += 6 * scale) {
    const uint8_t *glyph = FontGlyph(*text);
    if (!glyph) continue;
    for (int row = 0; row < 7; row++)
      for (int col = 0; col < 5; col++)
        if (glyph[row] & (0x10 >> col))
          FillRect(display, x + col * scale, y + row * scale, scale, scale,
                   color);
  }
}

static void DrawTextCentered(uint32_t *display, int y, int scale,
                             uint32_t color, const char *text) {
  DrawText(display, (kDisplayWidth - TextWidth(text, scale)) / 2, y, scale,
           color, text);
}

/* ---------------------------------------------------------------- audio */

enum {
  kSnesRate = 32040,
  kOutputRate = 48000,
  kOutputBlock = 256,
  kRingFrames = 8192, /* power of two */
  kRingTarget = 2400, /* ~50 ms of buffered audio */
  kCatchUpFill = 1100, /* below this, run an extra frame (one frame = ~800) */
};
static const double kVideoRate = 60.098811862;

static int16_t g_ring[kRingFrames * 2];
static volatile uint32_t g_ring_write, g_ring_read;
static int g_audio = -1;
static volatile bool g_audio_running;
static pthread_t g_audio_thread;

static uint32_t RingFill(void) {
  return __atomic_load_n(&g_ring_write, __ATOMIC_ACQUIRE) -
         __atomic_load_n(&g_ring_read, __ATOMIC_ACQUIRE);
}

static void *AudioThread(void *unused) {
  (void)unused;
  static int16_t blocks[2][kOutputBlock * 2];
  int current = 0;
  while (g_audio_running) {
    int16_t *out = blocks[current];
    uint32_t read = g_ring_read;
    uint32_t available = __atomic_load_n(&g_ring_write, __ATOMIC_ACQUIRE) - read;
    if (available >= kOutputBlock) {
      for (int i = 0; i < kOutputBlock; i++) {
        uint32_t index = (read + (uint32_t)i) & (kRingFrames - 1);
        out[i * 2] = g_ring[index * 2];
        out[i * 2 + 1] = g_ring[index * 2 + 1];
      }
      __atomic_store_n(&g_ring_read, read + kOutputBlock, __ATOMIC_RELEASE);
    } else {
      memset(out, 0, sizeof blocks[0]); /* underrun or paused */
    }
    sceAudioOutOutput(g_audio, out);
    current ^= 1;
  }
  return NULL;
}

static bool AudioInit(void) {
  sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
  int rc = sceAudioOutInit();
  if (rc != 0 && (unsigned)rc != 0x8026000Eu) {
    Log("sceAudioOutInit failed: 0x%08X", (unsigned)rc);
    return false;
  }
  int user = ORBIS_USER_SERVICE_USER_ID_SYSTEM;
  int initial = -1;
  if (sceUserServiceGetInitialUser(&initial) == 0 && initial > 0)
    user = initial;
  g_audio = sceAudioOutOpen(user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0,
                            kOutputBlock, kOutputRate,
                            ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
  if (g_audio <= 0)
    g_audio = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM,
                              ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kOutputBlock,
                              kOutputRate,
                              ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
  if (g_audio <= 0) {
    Log("sceAudioOutOpen failed: 0x%08X", (unsigned)g_audio);
    g_audio = -1;
    return false;
  }
  g_audio_running = true;
  if (pthread_create(&g_audio_thread, NULL, AudioThread, NULL) != 0) {
    g_audio_running = false;
    return false;
  }
  return true;
}

/* Linear resampler state (carried across frames). */
static double g_resample_position;
static int16_t g_previous_left, g_previous_right;

static void AudioReset(void) {
  g_resample_position = 0.0;
  g_previous_left = g_previous_right = 0;
}

static void PushAudio(const int16_t *samples, int frames) {
  if (g_audio < 0 || frames <= 0) return;
  /* The emulation thread runs on the audio clock (it emulates whenever the
   * ring is below its target), so the resampling ratio is exact. */
  const double step = (double)kSnesRate / kOutputRate;
  uint32_t write = g_ring_write;
  const uint32_t space = kRingFrames - RingFill();
  uint32_t produced = 0;
  double position = g_resample_position;
  while (position < (double)frames && produced < space) {
    const int index = (int)position;
    const double t = position - index;
    const int16_t l0 = index > 0 ? samples[(index - 1) * 2] : g_previous_left;
    const int16_t r0 = index > 0 ? samples[(index - 1) * 2 + 1] : g_previous_right;
    const int16_t l1 = samples[index * 2];
    const int16_t r1 = samples[index * 2 + 1];
    const uint32_t slot = (write + produced) & (kRingFrames - 1);
    g_ring[slot * 2] = (int16_t)(l0 + (l1 - l0) * t);
    g_ring[slot * 2 + 1] = (int16_t)(r0 + (r1 - r0) * t);
    produced++;
    position += step;
  }
  g_resample_position = position - frames;
  if (g_resample_position < 0.0) g_resample_position = 0.0;
  g_previous_left = samples[(frames - 1) * 2];
  g_previous_right = samples[(frames - 1) * 2 + 1];
  __atomic_store_n(&g_ring_write, write + produced, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------- input */

enum { kPlayers = 2, kStickDeadzone = 48 };

typedef struct {
  int user;
  int handle;
  uint32_t buttons;   /* raw DS4 buttons this frame */
  uint32_t previous;  /* raw DS4 buttons last frame */
  int lx, ly;
} Pad;

static Pad g_pads[kPlayers] = {{-1, -1}, {-1, -1}};
static unsigned g_pad_poll;

static void RefreshPads(void) {
  if (g_pad_poll++ % 60 != 0) return;
  OrbisUserServiceLoginUserIdList list;
  int initial = -1;
  sceUserServiceGetInitialUser(&initial);
  if (sceUserServiceGetLoginUserIdList(&list) != 0) return;
  /* Player 1 is the user who started the game, player 2 the next one. */
  int users[kPlayers] = {initial, -1};
  for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS; i++) {
    int id = list.userId[i];
    if (id <= 0 || id == initial) continue;
    if (users[0] <= 0) users[0] = id;
    else if (users[1] <= 0) { users[1] = id; break; }
  }
  for (int p = 0; p < kPlayers; p++) {
    if (g_pads[p].handle >= 0 && g_pads[p].user == users[p]) continue;
    if (g_pads[p].handle >= 0) scePadClose(g_pads[p].handle);
    g_pads[p].handle = -1;
    g_pads[p].user = users[p];
    if (users[p] > 0) {
      int handle = scePadOpen(users[p], ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
      if (handle >= 0) {
        g_pads[p].handle = handle;
        Log("player %d: DualShock 4 of user %d", p + 1, users[p]);
      }
    }
  }
}

static void ReadPads(void) {
  RefreshPads();
  for (int p = 0; p < kPlayers; p++) {
    Pad *pad = &g_pads[p];
    pad->previous = pad->buttons;
    pad->buttons = 0;
    pad->lx = pad->ly = 0;
    if (pad->handle < 0) continue;
    OrbisPadData data;
    memset(&data, 0, sizeof data);
    if (scePadReadState(pad->handle, &data) != 0 || !data.connected) continue;
    pad->buttons = data.buttons;
    pad->lx = (int)data.leftStick.x - 128;
    pad->ly = (int)data.leftStick.y - 128;
  }
}

static bool Pressed(const Pad *pad, uint32_t button) {
  return (pad->buttons & button) && !(pad->previous & button);
}

/* SNES bit order: B Y Select Start Up Down Left Right A X L R.
 * Face buttons keep the SNES physical layout (Cross = B, Circle = A). */
static uint32_t SnesButtons(const Pad *pad) {
  const uint32_t b = pad->buttons;
  uint32_t input = 0;
  if (b & ORBIS_PAD_BUTTON_CROSS) input |= 1u << 0;
  if (b & ORBIS_PAD_BUTTON_SQUARE) input |= 1u << 1;
  if (b & ORBIS_PAD_BUTTON_TOUCH_PAD) input |= 1u << 2;
  if (b & ORBIS_PAD_BUTTON_OPTIONS) input |= 1u << 3;
  if ((b & ORBIS_PAD_BUTTON_UP) || pad->ly < -kStickDeadzone) input |= 1u << 4;
  if ((b & ORBIS_PAD_BUTTON_DOWN) || pad->ly > kStickDeadzone) input |= 1u << 5;
  if ((b & ORBIS_PAD_BUTTON_LEFT) || pad->lx < -kStickDeadzone) input |= 1u << 6;
  if ((b & ORBIS_PAD_BUTTON_RIGHT) || pad->lx > kStickDeadzone) input |= 1u << 7;
  if (b & ORBIS_PAD_BUTTON_CIRCLE) input |= 1u << 8;
  if (b & ORBIS_PAD_BUTTON_TRIANGLE) input |= 1u << 9;
  if (b & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_L2)) input |= 1u << 10;
  if (b & (ORBIS_PAD_BUTTON_R1 | ORBIS_PAD_BUTTON_R2)) input |= 1u << 11;
  /* Up+Down or Left+Right together would confuse the game. */
  if ((input & 0x30) == 0x30) input &= ~0x30u;
  if ((input & 0xC0) == 0xC0) input &= ~0xC0u;
  return input;
}

/* --------------------------------------------------------------- system */

static bool SystemUiOpen(void) {
  Ps4SystemStatus status;
  memset(&status, 0, sizeof status);
  if (sceSystemServiceGetStatus(&status) != 0) return false;
  if (status.is_system_ui_overlaid > 1 || status.is_in_background_execution > 1)
    return false; /* layout guess wrong: ignore */
  return status.is_system_ui_overlaid || status.is_in_background_execution;
}

static void ExitToSystem(void) {
  Log("exit");
  if (g_log) fclose(g_log);
  g_log = NULL;
  sceSystemServiceLoadExec("exit", NULL);
  for (;;) sceKernelUsleep(100000);
}

/* SRAM is flushed when it changes, so a closed app loses nothing. */
static uint8_t *g_sram_shadow;

static void FlushSramIfChanged(bool force) {
  if (!g_sram || g_sram_size <= 0) return;
  if (!g_sram_shadow) {
    g_sram_shadow = (uint8_t *)malloc((size_t)g_sram_size);
    if (!g_sram_shadow) return;
    memcpy(g_sram_shadow, g_sram, (size_t)g_sram_size);
    return;
  }
  if (!force && !memcmp(g_sram_shadow, g_sram, (size_t)g_sram_size)) return;
  memcpy(g_sram_shadow, g_sram, (size_t)g_sram_size);
  RtlWriteSram();
  Log("SRAM saved (%d bytes)", g_sram_size);
}

/* -------------------------------------------------------------- screens */

static void MessageScreen(const char *const *lines, int count) {
  /* Shown when the game cannot start; Cross or Options leaves the app. */
  sceSystemServiceHideSplashScreen();
  for (;;) {
    ReadPads();
    for (int p = 0; p < kPlayers; p++)
      if (Pressed(&g_pads[p], ORBIS_PAD_BUTTON_CROSS) ||
          Pressed(&g_pads[p], ORBIS_PAD_BUTTON_OPTIONS))
        ExitToSystem();
    uint32_t *display = BeginFrame();
    memset(display, 0, kDisplayBufferSize);
    g_buffer_out_x[g_current_buffer] = -1;
    int y = 300;
    for (int i = 0; i < count; i++, y += 70)
      DrawTextCentered(display, y, 6, i == 0 ? 0xFFFFD040u : 0xFFFFFFFFu,
                       lines[i]);
    FinishFrame();
  }
}

static char *FindRom(void) {
  static const char *const names[] = {"rom.sfc", "rom.smc", "dkc2.sfc",
                                      "dkc2.smc"};
  static char path[512];
  struct stat st;
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
    snprintf(path, sizeof path, DATA_DIR "/%s", names[i]);
    if (stat(path, &st) == 0) return path;
  }
  DIR *dir = opendir(DATA_DIR);
  if (!dir) return NULL;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    const char *dot = strrchr(entry->d_name, '.');
    if (dot && (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc"))) {
      snprintf(path, sizeof path, DATA_DIR "/%s", entry->d_name);
      closedir(dir);
      return path;
    }
  }
  closedir(dir);
  return NULL;
}

/* ----------------------------------------------------------------- menu */

enum {
  kMenuResume,
  kMenuAspect,
  kMenuFilter,
  kMenuEdges,
  kMenuSlot,
  kMenuSave,
  kMenuLoad,
  kMenuExit,
  kMenuCount,
};

static int g_menu_index;
static char g_menu_status[64];

static void ApplyVideoConfig(void) {
  Dkc2VideoSetAspect(kAspects[g_config.aspect]);
  Dkc2VideoSetEdgePolicy((Dkc2VideoEdgePolicy)g_config.edge);
  Dkc2BeginDrawing((uint8_t *)g_frames[g_frame_draw].pixels,
                   (size_t)Dkc2VideoWidth() * 4);
}

static void DrawMenu(uint32_t *display) {
  const int scale = 5, line = 64, width = 900;
  const int height = line * (kMenuCount + 3);
  const int x0 = (kDisplayWidth - width) / 2, y0 = (kDisplayHeight - height) / 2;
  FillRect(display, x0, y0, width, height, 0xFF101830u);
  FillRect(display, x0, y0, width, 6, 0xFFFFD040u);
  DrawTextCentered(display, y0 + 30, scale, 0xFFFFD040u, "DONKEY KONG COUNTRY 2");
  char text[64];
  for (int i = 0; i < kMenuCount; i++) {
    switch (i) {
      case kMenuResume: snprintf(text, sizeof text, "CONTINUAR"); break;
      case kMenuAspect:
        snprintf(text, sizeof text, "FORMATO: < %s >", kAspectNames[g_config.aspect]);
        break;
      case kMenuFilter:
        snprintf(text, sizeof text, "FILTRO: < %s >",
                 g_gpu ? kFilterNames[g_config.filter] : "SIN GPU");
        break;
      case kMenuEdges:
        snprintf(text, sizeof text, "BORDES: < %s >", kEdgeNames[g_config.edge]);
        break;
      case kMenuSlot:
        snprintf(text, sizeof text, "RANURA: < %d >", g_config.slot + 1);
        break;
      case kMenuSave: snprintf(text, sizeof text, "GUARDAR ESTADO"); break;
      case kMenuLoad: snprintf(text, sizeof text, "CARGAR ESTADO"); break;
      case kMenuExit: snprintf(text, sizeof text, "SALIR DEL JUEGO"); break;
    }
    const int y = y0 + 30 + line * (i + 1) + 20;
    const bool selected = i == g_menu_index;
    if (selected) FillRect(display, x0 + 20, y - 12, width - 40, line - 6, 0xFF2C4A8Cu);
    DrawText(display, x0 + 60, y, scale, selected ? 0xFFFFFFFFu : 0xFFA0B0D0u, text);
  }
  if (g_menu_status[0])
    DrawTextCentered(display, y0 + height - 50, 4, 0xFF80FF80u, g_menu_status);
}

/* Returns true when the menu should close. */
static bool UpdateMenu(const Pad *pad, bool *reset_audio) {
  if (Pressed(pad, ORBIS_PAD_BUTTON_CIRCLE) || Pressed(pad, ORBIS_PAD_BUTTON_R3) ||
      Pressed(pad, ORBIS_PAD_BUTTON_OPTIONS))
    return true;
  if (Pressed(pad, ORBIS_PAD_BUTTON_UP))
    g_menu_index = (g_menu_index + kMenuCount - 1) % kMenuCount;
  if (Pressed(pad, ORBIS_PAD_BUTTON_DOWN))
    g_menu_index = (g_menu_index + 1) % kMenuCount;
  int delta = 0;
  if (Pressed(pad, ORBIS_PAD_BUTTON_LEFT)) delta = -1;
  if (Pressed(pad, ORBIS_PAD_BUTTON_RIGHT)) delta = 1;
  const bool confirm = Pressed(pad, ORBIS_PAD_BUTTON_CROSS);
  if (confirm && (g_menu_index == kMenuAspect || g_menu_index == kMenuEdges ||
                  g_menu_index == kMenuSlot || g_menu_index == kMenuFilter))
    delta = 1;
  if (delta) {
    if (g_menu_index == kMenuFilter) {
      g_config.filter = (g_config.filter + kFilterCount + delta) % kFilterCount;
    } else if (g_menu_index == kMenuAspect) {
      g_config.aspect = (g_config.aspect + kAspectCount + delta) % kAspectCount;
      ApplyVideoConfig();
      Dkc2DrawPpuFrame();
      PublishFrame();
    } else if (g_menu_index == kMenuEdges) {
      g_config.edge = (g_config.edge + kDkc2VideoEdgePolicyCount + delta) %
                      kDkc2VideoEdgePolicyCount;
      ApplyVideoConfig();
      Dkc2DrawPpuFrame();
      PublishFrame();
    } else if (g_menu_index == kMenuSlot) {
      g_config.slot = (g_config.slot + 5 + delta) % 5;
    }
    SaveConfig();
    g_menu_status[0] = 0;
  }
  if (!confirm) return false;
  char path[160];
  switch (g_menu_index) {
    case kMenuResume: return true;
    case kMenuSave:
      RtlSaveSlotPath(g_config.slot, path, sizeof path);
      snprintf(g_menu_status, sizeof g_menu_status,
               RtlSaveSnapshot(path) ? "RANURA %d GUARDADA" : "ERROR AL GUARDAR",
               g_config.slot + 1);
      Log("save state %s", path);
      return false;
    case kMenuLoad:
      RtlSaveSlotPath(g_config.slot, path, sizeof path);
      if (RtlLoadSnapshot(path)) {
        Dkc2DrawPpuFrame();
        PublishFrame();
        *reset_audio = true;
        snprintf(g_menu_status, sizeof g_menu_status, "RANURA %d CARGADA",
                 g_config.slot + 1);
        Log("load state %s", path);
        return true;
      }
      snprintf(g_menu_status, sizeof g_menu_status, "RANURA %d VACIA",
               g_config.slot + 1);
      return false;
    case kMenuExit:
      FlushSramIfChanged(true);
      SaveConfig();
      ExitToSystem();
      return true;
  }
  return false;
}

/* ------------------------------------------------------------ emulation */

/* The emulation thread runs the game on the audio clock: it emulates a frame
 * whenever the audio ring is below its target, so the game keeps the SNES's
 * exact speed and the sound never starves while the presenter is busy.
 * Everything that touches the emulator from the presenter (menu, states,
 * aspect changes, exit) happens while this thread is parked. */
static volatile uint32_t g_inputs;
static volatile bool g_emu_pause_request, g_emu_paused, g_emu_failed;
static volatile uint64_t g_emu_time_us, g_emu_worst_us, g_emu_frames;

static void PauseEmulation(void) {
  g_emu_pause_request = true;
  while (!g_emu_paused && !g_emu_failed) sceKernelUsleep(500);
}

static void ResumeEmulation(void) { g_emu_pause_request = false; }

static void *EmulationThread(void *unused) {
  (void)unused;
  int16_t frame_audio[600 * 2];
  double audio_fraction = 0.0;
  unsigned long long frame = 0;
  bool was_paused = true;
  uint64_t next_frame_time = 0; /* pacing without audio */
  for (;;) {
    if (g_emu_pause_request) {
      g_emu_paused = true;
      was_paused = true;
      sceKernelUsleep(2000);
      continue;
    }
    g_emu_paused = false;
    if (was_paused) {
      /* Resume with a cushion of silence instead of a burst of frames. */
      was_paused = false;
      AudioReset();
      static const int16_t silence[256 * 2];
      while (g_audio >= 0 && RingFill() + 256 < kRingTarget - 800)
        PushAudio(silence, 171); /* 171 SNES frames ~ 256 output frames */
      next_frame_time = sceKernelGetProcessTime();
    }
    if (g_audio >= 0) {
      if (RingFill() >= kRingTarget) {
        sceKernelUsleep(1000);
        continue;
      }
    } else {
      const uint64_t now = sceKernelGetProcessTime();
      if (now < next_frame_time) {
        sceKernelUsleep(1000);
        continue;
      }
      next_frame_time += (uint64_t)(1000000.0 / kVideoRate);
    }

    const uint64_t start = sceKernelGetProcessTime();
    (void)RtlRunFrame(g_inputs);
    if (g_fail || !Dkc2LastLleResult()) {
      Log("runtime stopped at frame %llu (resume PC $%06x)", frame + 1,
          (unsigned)Dkc2ResumePc());
      g_emu_failed = true;
      return NULL;
    }
    Dkc2DrawPpuFrame();
    audio_fraction += (double)kSnesRate / kVideoRate;
    int audio_frames = (int)audio_fraction;
    audio_fraction -= audio_frames;
    RtlRenderAudio(frame_audio, audio_frames, 2);
    Dkc2MusicMix(frame_audio, audio_frames, kSnesRate);
    PushAudio(frame_audio, audio_frames);
    PublishFrame();
    frame++;
    if (frame % 300 == 0) FlushSramIfChanged(false);

    const uint64_t spent = sceKernelGetProcessTime() - start;
    if (spent > 100000)
      Log("slow frame %llu: %.1f ms (resume PC $%06x, audio fill %u)", frame,
          spent / 1000.0, (unsigned)Dkc2ResumePc(), RingFill());
    g_emu_time_us += spent;
    if (spent > g_emu_worst_us) g_emu_worst_us = spent;
    g_emu_frames++;
  }
  return NULL;
}

/* ----------------------------------------------------------------- game */

static int GameMain(void) {
  OpenLog();
  Log("DKC2Recomp PS4 " VERSION_TEXT " starting");
  chdir(DATA_DIR); /* kongs.cfg, msu1.cfg and other relative files */
  mkdir(DATA_DIR "/saves", 0777);
  RtlSetSaveRoot(DATA_DIR "/saves");
  LoadConfig();

  sceUserServiceInitialize(NULL);
  scePadInit();
  if (!VideoInit()) return 1;
  if (!AudioInit()) Log("audio unavailable: continuing silent");
  GpuInit();
  ReadPads();

  const char *rom_path = FindRom();
  if (!rom_path) {
    static const char *const lines[] = {
        "DONKEY KONG COUNTRY 2", "NO SE ENCONTRO LA ROM",
        "COPIA DKC2 USA V1.0 (SIN REV 1) A", "/DATA/DKC2/ROM.SFC",
        "PULSA X PARA SALIR"};
    Log("no ROM in " DATA_DIR);
    MessageScreen(lines, 5);
  }
  size_t rom_size = 0;
  char error[256] = {0};
  uint8_t *rom = Dkc2ReadVerifiedRom(rom_path, &rom_size, error, sizeof error);
  if (!rom) {
    static const char *const lines[] = {
        "DONKEY KONG COUNTRY 2", "ROM NO SOPORTADA",
        "SE NECESITA DKC2 USA V1.0 (SIN REV 1)", "REVISA /DATA/DKC2/DEBUG.LOG",
        "PULSA X PARA SALIR"};
    Log("ROM rejected (%s): %s", rom_path, error);
    MessageScreen(lines, 5);
  }
  Log("ROM: %s (%zu bytes)", rom_path, rom_size);

  Dkc2KongsInitialize();
  Dkc2MusicInitialize();
  RtlRegisterGame(Dkc2GameInfo());
  if (!SnesInit(rom, (int)rom_size)) {
    static const char *const lines[] = {"DONKEY KONG COUNTRY 2",
                                        "EL RUNTIME RECHAZO LA ROM",
                                        "PULSA X PARA SALIR"};
    MessageScreen(lines, 3);
  }
  RtlReadSram();
  FlushSramIfChanged(false); /* take the initial shadow copy */
  ApplyVideoConfig();
  Log("video: %s (%dx224), edges %s", kAspectNames[g_config.aspect],
      Dkc2VideoWidth(), kEdgeNames[g_config.edge]);
  sceSystemServiceHideSplashScreen();

  pthread_t emulation;
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 16u * 1024u * 1024u);
  if (pthread_create(&emulation, &attributes, EmulationThread, NULL) != 0) {
    static const char *const lines[] = {"DONKEY KONG COUNTRY 2",
                                        "NO SE PUDO INICIAR EL JUEGO",
                                        "PULSA X PARA SALIR"};
    MessageScreen(lines, 3);
  }

  bool menu_open = false;
  bool ui_open = false;
  unsigned long long tick = 0;
  uint64_t stat_present = 0, stat_start = sceKernelGetProcessTime();
  unsigned stat_ticks = 0;

  for (;; tick++) {
    ReadPads();
    if (g_emu_failed) {
      FlushSramIfChanged(true);
      static const char *const lines[] = {
          "DONKEY KONG COUNTRY 2", "EL JUEGO SE DETUVO",
          "REVISA /DATA/DKC2/DEBUG.LOG", "PULSA X PARA SALIR"};
      MessageScreen(lines, 4);
    }
    if (tick % 6 == 0) {
      bool now_open = SystemUiOpen();
      if (now_open != ui_open) {
        ui_open = now_open;
        Log("system UI %s", ui_open ? "open: paused" : "closed: resumed");
        if (ui_open) {
          PauseEmulation();
          FlushSramIfChanged(false);
        } else if (!menu_open) {
          ResumeEmulation();
        }
      }
    }
    if (menu_open) {
      bool reset_audio = false;
      if (UpdateMenu(&g_pads[0], &reset_audio)) {
        menu_open = false;
        if (!ui_open) ResumeEmulation();
      }
    } else if (Pressed(&g_pads[0], ORBIS_PAD_BUTTON_R3) && !ui_open) {
      PauseEmulation();
      menu_open = true;
      g_menu_index = kMenuResume;
      g_menu_status[0] = 0;
    }
    g_inputs = menu_open ? 0u
                         : SnesButtons(&g_pads[0]) |
                               (SnesButtons(&g_pads[1]) << 12);

    const Frame *frame = TakeFrame();
    uint32_t *display = BeginFrame();
    const uint64_t start = sceKernelGetProcessTime();
    if (!g_gpu || !DrawGameGpu(display, frame, menu_open))
      DrawGame(display, frame, menu_open);
    if (menu_open) DrawMenu(display);
    FinishFrame();
    if (g_gpu) gnm_frame_done();
    stat_present += sceKernelGetProcessTime() - start;

    if (++stat_ticks == 600) {
      const uint64_t now = sceKernelGetProcessTime();
      const uint64_t frames = g_emu_frames;
      Log("perf: %.1f fps shown, %llu frames emulated (%.2f ms avg, worst "
          "%.2f), draw %.2f ms, gpu %.2f ms, filter %s, audio fill %u",
          600.0 * 1000000.0 / (double)(now - stat_start),
          (unsigned long long)frames,
          frames ? g_emu_time_us / (double)frames / 1000.0 : 0.0,
          g_emu_worst_us / 1000.0, stat_present / 600.0 / 1000.0,
          g_gpu_wait_us / 600.0 / 1000.0,
          g_gpu ? kFilterNames[g_config.filter] : "CPU", RingFill());
      g_emu_frames = 0;
      g_emu_time_us = 0;
      g_emu_worst_us = 0;
      g_gpu_wait_us = 0;
      stat_present = 0;
      stat_ticks = 0;
      stat_start = now;
    }
  }
  return 0;
}

/* The recompiled game code can nest deeply: run it on a large stack. */
static void *GameThread(void *unused) {
  (void)unused;
  GameMain();
  ExitToSystem();
  return NULL;
}

int main(void) {
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, 16u * 1024u * 1024u);
  pthread_t thread;
  if (pthread_create(&thread, &attributes, GameThread, NULL) != 0)
    GameThread(NULL);
  pthread_join(thread, NULL);
  ExitToSystem();
  return 0;
}
