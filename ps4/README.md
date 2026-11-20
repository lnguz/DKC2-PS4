# Donkey Kong Country 2 for PS4

A native PS4 port of [DKC2Recomp](https://github.com/elliotttate/DKC2Recomp)
(static recompilation of *Donkey Kong Country 2: Diddy's Kong Quest*, built on
[snesrecomp](https://github.com/elliotttate/snesrecomp)) for jailbroken
consoles with GoldHEN, built with the [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
toolchain.

This repository contains no game data. You need your own **USA v1.0** ROM
(not Rev 1; SHA-256 `35421a9a…cbf2d8633`).

## Features

- Real 16:9 widescreen (342x224, level edges rebuilt from the level map), also
  16:10 and 4:3, scaled to 1080p.
- Screen filters on the GPU (own PM4 packets and LLVM-compiled GCN kernels):
  - **PIXELES**: crisp pixels, all the same size.
  - **HD**: ScaleFX (Sp00kyFox, MIT) at 3x, Lanczos2 to the screen, then
    contrast-adaptive sharpening.
- Exact SNES speed: emulation runs on its own thread paced by the audio clock;
  presentation and GPU work run on another.
- Two-player co-op with two DualShock 4 controllers (second logged-in user).
- 9 save-state slots, battery saves written automatically.
- Fix for a multi-second stall after loading a state (the SPC's guest-clock
  mapping is re-anchored on load; see `RebaseApuClock` in `ps4/src/main.c`).

## Using it

1. Install the pkg.
2. Copy your ROM to `/data/DKC2/rom.sfc` (or `rom.smc`; a copier header is
   fine).
3. Start the game. **R3** opens the menu: format, filter, edges, save-state
   slot, save, load, exit.

| DualShock 4 | SNES |
|---|---|
| Cross / Circle / Square / Triangle | B / A / Y / X |
| L1, L2 / R1, R2 | L / R |
| Options / touchpad click | Start / Select |
| D-pad, left stick | D-pad |

Saves, settings (`ps4.cfg`) and `debug.log` live in `/data/DKC2`. An empty
`/data/DKC2/nogpu` file turns the GPU path off (CPU scaling).

## Building

On Linux/WSL with the OpenOrbis toolchain, Python 3, Rust and LLVM (for the
GPU kernels):

```sh
git submodule update --init --recursive
python3 scripts/generate_snesrecomp.py --rom /private/path/dkc2.sfc
# supply your own 512x512 ps4/sce_sys/icon0.png
bash ps4/build.sh        # -> build-ps4/DKC2-PS4-<version>.pkg
```

`LLVM_BIN` points at an LLVM with the AMDGPU backend to rebuild
`ps4/src/gcn_kernels.h` from `ps4/kernels/*.cl`; without it the checked-in
header is used. The generated C (and therefore the pkg) is built from your
ROM and is never committed.

`ps4/tools/host_state_test.c` builds on the desktop (`cmake -S ps4 -B
build-host`) and reproduces state loads headless.
