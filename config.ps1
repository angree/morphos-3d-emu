# SPDX-License-Identifier: MIT
#
# morphos-3d-emu: the ONE file you may need to edit.
# Relative paths are relative to the folder this file is in.
# All the lab scripts (lab\*.ps1, tools\remaster.ps1) read it.

$Config = @{
    # Folder with qemu-system-ppc.exe (the release ships it in qemu\; or point it at an
    # installed QEMU for Windows, e.g. 'C:\Program Files\qemu').
    QemuDir     = 'qemu'

    # The official MorphOS ISO you downloaded yourself from https://www.morphos-team.net/
    # (never shipped with this project). Input of tools\remaster.ps1.
    MorphOSIso  = 'morphos\morphos-3.20.iso'

    # Where tools\remaster.ps1 writes the lab ISO and boot.img, and where the lab boots them from.
    LabIso      = 'morphos\morphos-3.20-tglb.iso'
    BootImg     = 'morphos\boot.img'

    # Transfer folder: QEMU shows it to the guest as the FAT disk ide0: (files must exist before boot).
    TransferDir = 'lab\xfer'

    # Files the guest fetches over TCP with tglbget (searched in this order), and where it puts files back.
    ServeDirs   = @('lab\serve', 'bin\guest')
    InboxDir    = 'inbox'

    # The host renderer.
    TglHost     = 'bin\host\tglhost.exe'

    # Logs and PID files of a lab run.
    RunDir      = 'run'

    # QEMU display: 'sdl' (a window with the MorphOS desktop) or 'none' (headless, drive it via lab\mon.ps1).
    Display     = 'sdl'
    RamMB       = 1024

    # TCP ports on 127.0.0.1. The guest reaches the host as 10.0.2.2 (QEMU user networking).
    MonitorPort = 4559   # QEMU monitor (lab\mon.ps1, lab\type.ps1)
    BridgePort  = 7654   # tglhost (TGLBRIDGE_PORT in the guest)
    ServePort   = 7660   # file server for tglbget
    # (the guest scripts lab\xfer\tglb and lab\serve\step use 10.0.2.2:7660 too; change them together)
}
