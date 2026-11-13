#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Build the guest side (MorphOS PPC) with the morphos-cross toolchain and the MorphOS SDK
# (both installed by you, see docs/BUILD.md). Run it on Linux or in WSL:
#
#   bash bridge/guest/build.sh
#
# Steps: 1. bridge/gen/gen.py reads ppcinline/tinygl.h from YOUR installed SDK and writes the
#           gen_* files for the guest and the host (they are never committed: .gitignore);
#        2. builds tinygl.library (the bridge), tglbload, tglbget and tests/tglbtest.
# Outputs go to bin/guest/ at the top of the repository.
#
# Environment:
#   MORPHOS_CROSS   toolchain root (default: ~/.morphos-cross/current)
set -e
G="$(cd "$(dirname "$0")" && pwd)"
TOP="$(cd "$G/../.." && pwd)"
OUT="$TOP/bin/guest"
X="${MORPHOS_CROSS:-$HOME/.morphos-cross/current}"
export PATH="$X/bin:$PATH"
command -v ppc-morphos-gcc >/dev/null || { echo "*** ppc-morphos-gcc not found (MORPHOS_CROSS=$X)"; exit 1; }
HDR="$X/os-include/ppcinline/tinygl.h"
[ -f "$HDR" ] || { echo "*** MorphOS SDK header not found: $HDR (install the SDK, see docs/BUILD.md)"; exit 1; }

echo "== gen.py (from $HDR) =="
python3 "$TOP/bridge/gen/gen.py" "$HDR"

B="$(mktemp -d "${TMPDIR:-/tmp}/tglb-build.XXXXXX")"
trap 'rm -rf "$B"' EXIT
mkdir -p "$OUT" "$B/guest" "$B/common" "$B/tests"
cp "$G"/*.c "$G"/*.h "$B/guest/"
cp "$TOP/bridge/common/"*.h "$B/common/"
cp "$TOP/tests/tglbtest.c" "$B/tests/"
cd "$B/guest"

# -ffreestanding -fno-tree-loop-distribute-patterns: gcc must not turn the library's own memset/memcpy
# loops into calls to memset/memcpy (measured: infinite recursion -> hard hang at the first GLInit)
LIBFLAGS="-O2 -fno-strict-aliasing -fsigned-char -noixemul -ffreestanding -fno-tree-loop-distribute-patterns -Wall -Wno-unused-function -I."
echo "== tinygl.library =="
ppc-morphos-gcc $LIBFLAGS -c tglb_lib.c -o tglb_lib.o
ppc-morphos-gcc $LIBFLAGS -c tglb_manual.c -o tglb_manual.o
ppc-morphos-gcc $LIBFLAGS -w -c gen_slots.c -o gen_slots.o
ppc-morphos-gcc -noixemul -nostartfiles -nostdlib -o tinygl.library tglb_lib.o tglb_manual.o gen_slots.o -lgcc
file tinygl.library | grep -q PowerPC || { echo "*** tinygl.library is not PowerPC"; file tinygl.library; exit 1; }
ppc-morphos-nm tinygl.library | grep -E " (U) " && { echo "*** undefined symbols in the library"; exit 1; } || true

APPFLAGS="-O2 -fno-strict-aliasing -fsigned-char -noixemul -Wall"
echo "== tglbload =="
ppc-morphos-gcc $APPFLAGS tglbload.c -o tglbload
echo "== tglbget =="
ppc-morphos-gcc $APPFLAGS tglbget.c -o tglbget
echo "== tglbtest =="
ppc-morphos-gcc $APPFLAGS ../tests/tglbtest.c -o tglbtest -lm
cp tinygl.library tglbload tglbtest tglbget "$OUT/"
ls -l "$OUT"
