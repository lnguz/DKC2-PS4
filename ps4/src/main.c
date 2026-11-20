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
#define VERSION_TEXT "1.00"

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
  /* The runtime reports through stdout/stderr: point both at the log. */
  if (g_log) {
    int fd = open(LOG_PATH, O_WRONLY | O_APPEND);
    if (fd >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
  }
}

/* --------------------------------------------------------------- config */

typedef struct {
  int aspect;  /* index into kAspects */
  int edge;    /* Dkc2VideoEdgePolicy */
  int slot;    /* 0..4 */
} Ps4Config;

static const Dkc2VideoAspect kAspects[] = {
    kDkc2VideoAspect16x9, kDkc2VideoAspect16x10, kDkc2VideoAspectNative};
static const char *const kAspectNames[] = {"16:9", "16:10", "4:3"};
enum { kAspectCount = 3 };
static const char *const kEdgeNames[] = {"REFLEJAR", "BARRAS", "DESPLAZAR",
                                         "DESLIZAR"};

static Ps4Config g_config = {0, kDkc2VideoEdgeGlide, 0};

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
  }
  fclose(f);
}

static void SaveConfig(void) {
  FILE *f = fopen(CONFIG_PATH, "w");
  if (!f) return;
  fprintf(f, "aspect=%d\nedge=%d\nslot=%d\n", g_config.aspect, g_config.edge,
          g_config.slot);
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
static uint32_t g_pixels[kSourceMaxWidth * kSourceHeight];
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

static void DrawGame(uint32_t *display, bool dim) {
  const int source_width = Dkc2VideoWidth();
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
      const uint32_t *source = g_pixels + (size_t)converted * source_width;
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
  kRingTarget = 2048, /* ~43 ms of buffered audio */
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
  /* Steer the fill toward the target with at most +/-0.5 % pitch change. */
  double error = ((double)RingFill() - kRingTarget) / kRingTarget;
  if (error > 1.0) error = 1.0;
  if (error < -1.0) error = -1.0;
  const double step =
      (double)kSnesRate / kOutputRate * (1.0 + 0.005 * error);
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
  Dkc2BeginDrawing((uint8_t *)g_pixels, (size_t)Dkc2VideoWidth() * 4);
}

static void DrawMenu(uint32_t *display) {
  const int scale = 5, line = 64, width = 900;
  const int height = line * (kMenuCount + 3);
  const int x0 = (kDisplayWidth - width) / 2, y0 = (kDisplayHeight - height) / 2;
  FillRect(display, x0, y0, width, height, 0xFF101830u);
  FillRect(display, x0, y0, width, 6, 0xFFFFD040u);
  DrawTextCentered(display, y0 + 30, scale, 0xFFFFD040u, "DKC2 RECOMP PS4");
  char text[64];
  for (int i = 0; i < kMenuCount; i++) {
    switch (i) {
      case kMenuResume: snprintf(text, sizeof text, "CONTINUAR"); break;
      case kMenuAspect:
        snprintf(text, sizeof text, "FORMATO: < %s >", kAspectNames[g_config.aspect]);
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
                  g_menu_index == kMenuSlot))
    delta = 1;
  if (delta) {
    if (g_menu_index == kMenuAspect) {
      g_config.aspect = (g_config.aspect + kAspectCount + delta) % kAspectCount;
      ApplyVideoConfig();
      Dkc2DrawPpuFrame();
    } else if (g_menu_index == kMenuEdges) {
      g_config.edge = (g_config.edge + kDkc2VideoEdgePolicyCount + delta) %
                      kDkc2VideoEdgePolicyCount;
      ApplyVideoConfig();
      Dkc2DrawPpuFrame();
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
  ReadPads();

  const char *rom_path = FindRom();
  if (!rom_path) {
    static const char *const lines[] = {
        "DKC2 RECOMP PS4", "NO SE ENCONTRO LA ROM",
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
        "DKC2 RECOMP PS4", "ROM NO SOPORTADA",
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
    static const char *const lines[] = {"DKC2 RECOMP PS4",
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

  int16_t frame_audio[600 * 2];
  double audio_fraction = 0.0;
  bool menu_open = false;
  bool ui_open = false;
  unsigned long long frame = 0;
  uint64_t stat_emulate = 0, stat_present = 0, stat_worst = 0;
  unsigned stat_frames = 0;

  for (;;) {
    ReadPads();
    if (frame % 6 == 0) {
      bool now_open = SystemUiOpen();
      if (now_open != ui_open) {
        ui_open = now_open;
        Log("system UI %s", ui_open ? "open: paused" : "closed: resumed");
        if (ui_open) FlushSramIfChanged(false);
      }
    }
    bool reset_audio = false;
    if (menu_open) {
      if (UpdateMenu(&g_pads[0], &reset_audio)) {
        menu_open = false;
        reset_audio = true;
      }
    } else if (Pressed(&g_pads[0], ORBIS_PAD_BUTTON_R3) && !ui_open) {
      menu_open = true;
      g_menu_index = kMenuResume;
      g_menu_status[0] = 0;
    }
    if (reset_audio) AudioReset();

    const uint64_t start = sceKernelGetProcessTime();
    if (!menu_open && !ui_open) {
      uint32_t inputs = SnesButtons(&g_pads[0]) |
                        (SnesButtons(&g_pads[1]) << 12);
      (void)RtlRunFrame(inputs);
      if (g_fail || !Dkc2LastLleResult()) {
        Log("runtime stopped at frame %llu (resume PC $%06x)", frame + 1,
            (unsigned)Dkc2ResumePc());
        FlushSramIfChanged(true);
        static const char *const lines[] = {
            "DKC2 RECOMP PS4", "EL JUEGO SE DETUVO",
            "REVISA /DATA/DKC2/DEBUG.LOG", "PULSA X PARA SALIR"};
        MessageScreen(lines, 4);
      }
      Dkc2DrawPpuFrame();
      audio_fraction += (double)kSnesRate / kVideoRate;
      int audio_frames = (int)audio_fraction;
      audio_fraction -= audio_frames;
      RtlRenderAudio(frame_audio, audio_frames, 2);
      Dkc2MusicMix(frame_audio, audio_frames, kSnesRate);
      PushAudio(frame_audio, audio_frames);
      frame++;
      if (frame % 300 == 0) FlushSramIfChanged(false);
    } else if (ui_open) {
      sceKernelUsleep(16000);
    }
    const uint64_t emulated = sceKernelGetProcessTime();

    uint32_t *display = BeginFrame();
    DrawGame(display, menu_open);
    if (menu_open) DrawMenu(display);
    FinishFrame();
    const uint64_t presented = sceKernelGetProcessTime();

    if (!menu_open && !ui_open) {
      stat_emulate += emulated - start;
      stat_present += presented - emulated;
      if (emulated - start > stat_worst) stat_worst = emulated - start;
      if (++stat_frames == 600) {
        Log("perf: emulate %.2f ms avg (worst %.2f), present+wait %.2f ms, "
            "audio fill %u",
            stat_emulate / 600.0 / 1000.0, stat_worst / 1000.0,
            stat_present / 600.0 / 1000.0, RingFill());
        stat_emulate = stat_present = stat_worst = 0;
        stat_frames = 0;
      }
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
