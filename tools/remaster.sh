#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Make the lab copy of YOUR official MorphOS ISO (the original file stays untouched) and extract boot.img.
#   bash tools/remaster.sh <morphos-3.20.iso> <output-dir>
# Needs xorriso (Debian/Ubuntu/WSL: sudo apt install xorriso). Writes <output-dir>/morphos-3.20-tglb.iso
# and <output-dir>/boot.img. Nothing from MorphOS is part of this project; this script only edits your copy:
#  - MorphOS/Devs/AudioModes/PEGASOS -> MorphOS/Storage/AudioModes/PEGASOS
#    (QEMU pegasos2: if the audio driver starts before the PCI network driver, networking fails)
#  - S/user-startup (taken from your ISO) gets tools/user-startup.hook appended, which runs
#    ide0:tglb-autorun from the transfer disk if it exists
set -e
ISO="$1"; OUT="$2"
[ -f "$ISO" ] && [ -n "$OUT" ] || { echo "usage: $0 <morphos.iso> <output-dir>"; exit 1; }
command -v xorriso >/dev/null || { echo "*** xorriso not found (sudo apt install xorriso)"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
T="$(mktemp -d "${TMPDIR:-/tmp}/mos-remaster.XXXXXX")"
trap 'rm -rf "$T"' EXIT

xorriso -osirrox on -indev "$ISO" -extract /S/user-startup "$T/user-startup" -extract /boot.img "$T/boot.img" 2>/dev/null
chmod u+w "$T/user-startup" "$T/boot.img"
if grep -q 'tglb-autorun' "$T/user-startup"; then
  echo "S/user-startup already has the hook"
else
  cat "$HERE/user-startup.hook" >> "$T/user-startup"
fi
cp "$T/boot.img" "$OUT/boot.img"

LAB="$OUT/morphos-3.20-tglb.iso"
rm -f "$LAB"
xorriso -indev "$ISO" -outdev "$LAB" \
  -mkdir /MorphOS/Storage/AudioModes -- \
  -mv /MorphOS/Devs/AudioModes/PEGASOS /MorphOS/Storage/AudioModes/PEGASOS -- \
  -map "$T/user-startup" /S/user-startup \
  -boot_image any keep -commit 2>&1 | tail -3
ls -l "$LAB" "$OUT/boot.img"
xorriso -indev "$LAB" -ls /MorphOS/Storage/AudioModes 2>/dev/null | tail -2
