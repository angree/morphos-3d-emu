# SPDX-License-Identifier: MIT
# Start MorphOS on QEMU pegasos2 (unmodified QEMU, no -bios: QEMU's built-in VOF firmware boots boot.img).
#   -Hidden : no window (-display none); drive the guest with lab\mon.ps1 / lab\type.ps1
#   -DryRun : only print the QEMU command line
# The QEMU monitor listens on tcp 127.0.0.1:<MonitorPort>; the guest network is QEMU user networking
# (rtl8139), the guest reaches the host as 10.0.2.2. ide0: is the transfer folder (FAT, read at boot).
# Stop it with lab\stop.ps1 (monitor 'quit'), never by process name.
param([switch]$Hidden, [switch]$DryRun)
. "$PSScriptRoot\common.ps1"

foreach ($f in @($C.QemuExe, $C.BootImg, $C.LabIso) | Where-Object { -not $DryRun }) {
    if (-not (Test-Path $f)) { throw "missing: $f (see README.md, Quick start; paths are set in config.ps1)" }
}
# the guest copies tglbget from ide0: at boot, so the transfer folder must hold the current build
$get = $C.ServeDirs | ForEach-Object { Join-Path $_ 'tglbget' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($get -and -not $DryRun) { Copy-Item $get (Join-Path $C.TransferDir 'tglbget') -Force }

$display = if ($Hidden) { 'none' } else { $C.Display }
$xfer = ($C.TransferDir -replace '\\', '/')
$qargs = @(
    '-L', (Join-Path $C.QemuDir 'share'),
    '-machine', 'pegasos2', '-m', "$($C.RamMB)", '-rtc', 'base=localtime',
    '-device', 'ati-vga,guest_hwcursor=true,romfile=',
    '-kernel', $C.BootImg,
    '-drive', "file=$($C.LabIso),if=ide,index=2,media=cdrom,readonly=on",
    '-drive', "file=fat:16:rw:$xfer,format=raw,if=ide,index=0",
    '-netdev', 'user,id=n0', '-device', 'rtl8139,netdev=n0',
    '-display', $display, '-audio', 'none',
    '-monitor', "tcp:127.0.0.1:$($C.MonitorPort),server,nowait",
    '-serial', "file:$(Join-Path $C.RunDir 'serial.log')"
)
$argline = ($qargs | ForEach-Object { Quote-Arg $_ }) -join ' '
if ($DryRun) { return "$(Quote-Arg $C.QemuExe) $argline" }
$ws = if ($Hidden) { 'Hidden' } else { 'Normal' }
$p = Start-Process -FilePath $C.QemuExe -ArgumentList $argline -WindowStyle $ws -PassThru
Save-Pid 'qemu' $p.Id
"qemu pid $($p.Id) (monitor 127.0.0.1:$($C.MonitorPort))"
