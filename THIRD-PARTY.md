# Third-party software: what this repository does NOT contain

This repository contains only our own code (MIT, see `LICENSE`). It uses, but does not include:

| what | where to get it | licence | notes |
|---|---|---|---|
| MorphOS 3.20 (ISO) | <https://www.morphos-team.net/> | proprietary, (C) MorphOS Team | You download it yourself. `tools/remaster.sh` edits only your local copy. Never redistribute the ISO, the remastered ISO or `boot.img`. |
| MorphOS SDK (`sdk-20260529.lha` or newer) | <https://morphos-team.net/> | (C) MorphOS Team; headers, docs and examples may not be redistributed | Needed only to build the guest side. `bridge/gen/gen.py` reads `ppcinline/tinygl.h` from your installed SDK at build time; the generated `gen_*` files are not committed (`.gitignore`). |
| morphos-cross toolchain (GCC 15.2, binutils 2.45.1) | <https://github.com/metaneutrons/morphos-cross> | GPL-3.0-or-later (GMP/MPFR/MPC: LGPL) | Build tool only. `tinygl.library` links only `libgcc` (GCC Runtime Library Exception 3.1). |
| QEMU for Windows | <https://www.qemu.org/>, Windows installers at <https://qemu.weilnetz.de/> | GPL-2.0-or-later (parts under compatible licences) | Not modified, not linked: our programs talk to it over TCP only. The GitHub release zip contains an unmodified subset of QEMU 11.1.0 with all licence texts (`LICENSES/`, `THIRD-PARTY.md` inside the zip) and a separate sources asset. |
| MSVC 2022 / Windows SDK | Microsoft | Microsoft licence | Build tool for `tglhost.exe`, which links only Windows system libraries. |

"MorphOS" and "TinyGL" are names of their respective owners. Our `tinygl.library` is an independent
implementation of the TinyGL interface and contains no MorphOS Team code. QEMU is a trademark of Fabrice
Bellard; this project is not affiliated with or endorsed by the QEMU project.
