#!/usr/bin/env bash
# Build DKC2Recomp for PS4 and package it.
# Run on Linux/WSL with the OpenOrbis toolchain (default ~/ps4dev/OpenOrbis/PS4Toolchain).
# The private generated C must exist first:
#   python3 scripts/generate_snesrecomp.py --rom /private/path/dkc2.sfc
# Usage: ps4/build.sh [build dir]   (default: build-ps4)
set -euo pipefail

SOURCE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$SOURCE/build-ps4}"
export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$HOME/ps4dev/OpenOrbis/PS4Toolchain}"

cmake -S "$SOURCE/ps4" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$SOURCE/ps4/openorbis.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)" --target dkc2_ps4

"$OO_PS4_TOOLCHAIN/bin/linux/create-fself" -in="$BUILD/dkc2_ps4" \
    -out="$BUILD/dkc2_ps4.oelf" --eboot "$BUILD/eboot.bin" \
    --paid 0x3800000000000011

bash "$SOURCE/ps4/make_pkg.sh" "$BUILD/eboot.bin" "$BUILD"
