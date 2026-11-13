# Building morphos-3d-emu

Two halves, built on two toolchains:

| part | runs on | built with | output |
|---|---|---|---|
| guest: `tinygl.library`, `tglbload`, `tglbget`, `tglbtest` | MorphOS (PPC) | morphos-cross GCC + MorphOS SDK, on Linux or WSL | `bin/guest/` |
| host: `tglhost.exe` | Windows x64 | MSVC 2022 (Build Tools are enough) | `bin/host/` |

Both halves use generated slot tables (`gen_*` files). They are produced by `bridge/gen/gen.py` from the SDK
header `ppcinline/tinygl.h` **on your machine** and are deliberately not part of the repository: the MorphOS
SDK may not be redistributed, not even in modified form (SDK.readme, section 8), so this project ships neither
SDK files nor files derived from them. Build the guest first; that step also writes the host's `gen_*` files.

## 1. Toolchain and SDK (once)

1. Install a Linux environment. On Windows: WSL with Ubuntu 22.04 or 24.04.
2. Install the cross toolchain from <https://github.com/metaneutrons/morphos-cross> (GCC 15.2, binutils 2.45.1).
   It installs into `~/.morphos-cross/<version>` with a `current` link. The toolchain is GPL-3.0; it is not
   bundled here.
3. Install the **MorphOS SDK** (`sdk-20260529.lha` or newer, from <https://morphos-team.net/>; morphos-cross can
   fetch and unpack it for you, see its README). The build expects the SDK headers under
   `~/.morphos-cross/current/os-include/` (that is where morphos-cross puts them). Set `MORPHOS_CROSS` if your
   toolchain lives elsewhere.
4. `python3` must be available (only for `gen.py`).

## 2. Guest side

```sh
bash bridge/guest/build.sh
```

This runs `gen.py` against `$MORPHOS_CROSS/os-include/ppcinline/tinygl.h`, writes
`bridge/guest/gen_{slots.c,manual.h,slotnums.h}` and `bridge/host/gen_{dispatch.c,names.c,slotnums.h}`, then
builds the four guest binaries into `bin/guest/`.

Notes:

- The library is linked with `-nostdlib` and only `-lgcc` (GCC runtime library exception applies).
- It must be compiled with `-ffreestanding -fno-tree-loop-distribute-patterns`: otherwise GCC turns the
  library's own `memset` loop into a call to `memset`, which recurses forever and hangs at the first `GLInit`.
- `gen.py` can also be run on its own: `python3 bridge/gen/gen.py <path to ppcinline/tinygl.h>`, or with
  `MORPHOS_SDK_INCLUDE` pointing at the SDK include folder. Re-run it after an SDK update.
- With the same toolchain and SDK the build is reproducible byte for byte.

## 3. Host side

From any Windows shell, after step 2 (the `gen_*` files must exist in `bridge/host/`; on WSL the repository on a
Windows drive is shared, otherwise copy the three host `gen_*` files over):

```bat
bridge\host\build.bat
```

It finds MSVC through `vswhere` (or uses `cl.exe` from the current prompt) and writes `bin\host\tglhost.exe`.
`tglhost.exe` links only Windows system libraries (`ws2_32`, `opengl32`, `gdi32`, `user32`).

## 4. Tests without QEMU

```bat
bin\host\tglhost.exe --hidden --port 7655 --exit-after 1
python tests\fakeguest.py 7655
```

`fakeguest.py` speaks the guest protocol: it draws a Gouraud triangle and an RGB565 textured quad, reads pixels
back, checks them and swaps three frames with mirroring. It prints `fakeguest OK` on success.

## 5. Tests in the lab

`lab\run.ps1` (see the README) boots MorphOS and runs `lab\serve\step`, which loads the freshly built bridge and
runs `tglbtest` (basic scene with `glReadPixels` checks) and `tglbtest stress 20` (about 10,000 triangles per
frame) with and without mirroring. The guest's logs arrive in `inbox\` (`test2.log`, `stress.log`,
`stress-nomirror.log`, `load2.log`), the host log in `run\tglhost.log`.
`tglhost --shot 1,60,149 --shotdir run\shots` saves frames as BMP (`lab\start-host.ps1 -TglArgs '...'`).

The guest fetches `bin/guest/*` over TCP at every run, so a rebuilt bridge needs no new ISO and no reboot:
open a Shell in MorphOS and run `RAM:tglbget get 10.0.2.2 7660 step RAM:step` and `execute RAM:step`
(`tglbload` replaces an older bridge in memory).

## tglhost options

```
tglhost [--port 7654] [--hidden] [--shot 60,120] [--shotdir dir] [--log file]
        [--exit-after N]   exit after N guest connections have closed
        [--glcheck]        debug: glGetError after every command, first error per opcode logged
```
