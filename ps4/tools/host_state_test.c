/* Desktop (Linux) reproduction harness for the PS4 port: boots the game
 * headless, runs `before` frames, loads a save state, then runs `after`
 * frames and prints, per frame, the time taken and the APU's guest-clock
 * mapping, to study the state-load stall seen on the console.
 *
 * usage: host_state_test <rom> <state> [before] [after] [saves dir] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "dkc2_game.h"
#include "dkc2_kongs.h"
#include "dkc2_music.h"
#include "dkc2_video.h"
#include "snes/apu.h"
#include "snes/snes.h"
#include "verified_rom.h"

extern int snes_frame_counter;

static double Now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}

static void Report(const char *tag, double ms) {
  Apu *apu = g_snes->apu;
  printf("%-6s frame %5d  %7.2f ms  portClock %llu  guestAnchor %llu  "
         "targetAnchor %llu  lastGuest %llu  lastTarget %llu  valid %d  "
         "queue %u\n",
         tag, snes_frame_counter, ms, (unsigned long long)apu->portClock,
         (unsigned long long)apu->portGuestAnchor,
         (unsigned long long)apu->portTargetAnchor,
         (unsigned long long)apu->portLastGuest,
         (unsigned long long)apu->portLastTarget, (int)apu->portTimeValid,
         apu->portQTail - apu->portQHead);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s rom state [before] [after] [saves]\n", argv[0]);
    return 2;
  }
  const int before = argc > 3 ? atoi(argv[3]) : 300;
  const int after = argc > 4 ? atoi(argv[4]) : 120;
  RtlSetSaveRoot(argc > 5 ? argv[5] : "/tmp/dkc2-host-saves");
  size_t size = 0;
  char error[256];
  uint8_t *rom = Dkc2ReadVerifiedRom(argv[1], &size, error, sizeof error);
  if (!rom) {
    fprintf(stderr, "rom: %s\n", error);
    return 1;
  }
  Dkc2KongsInitialize();
  Dkc2MusicInitialize();
  RtlRegisterGame(Dkc2GameInfo());
  if (!SnesInit(rom, (int)size)) return 1;
  Dkc2VideoSetAspect(kDkc2VideoAspect16x9);
  static uint32_t pixels[448 * 224];
  Dkc2BeginDrawing((uint8_t *)pixels, (size_t)Dkc2VideoWidth() * 4);
  static int16_t audio[600 * 2];
  double fraction = 0.0;

  for (int i = 0; i < before + after; i++) {
    if (i == before) {
      Report("pre", 0.0);
      double t = Now();
      if (!RtlLoadSnapshot(argv[2])) {
        fprintf(stderr, "load failed\n");
        return 1;
      }
      if (argc > 6 && !strcmp(argv[6], "fix")) {
        /* the fix under test: restart the APU guest-clock mapping */
        Apu *apu = g_snes->apu;
        const uint64_t guest = (uint64_t)snes_frame_counter * 17088u;
        apu->portQHead = apu->portQTail;
        apu->portGuestAnchor = guest;
        apu->portTargetAnchor = apu->portClock;
        apu->portLastGuest = guest;
        apu->portLastTarget = apu->portClock;
        apu->portTimeValid = true;
      }
      Dkc2DrawPpuFrame();
      Report("load", (Now() - t) * 1000.0);
    }
    double t = Now();
    /* press Start once in a while so the title screen moves on */
    uint32_t input = (i < before && i % 120 == 60) ? (1u << 3) : 0u;
    RtlRunFrame(input);
    Dkc2DrawPpuFrame();
    fraction += 32040.0 / 60.098811862;
    int frames = (int)fraction;
    fraction -= frames;
    RtlRenderAudio(audio, frames, 2);
    const double ms = (Now() - t) * 1000.0;
    if (i >= before && (i < before + 12 || ms > 20.0)) Report("run", ms);
    else if (i == before - 1) Report("boot", ms);
  }
  return 0;
}
