# SPDX-License-Identifier: MIT
# One command for a lab run: start tglhost + file server, then MorphOS in QEMU.
# The remastered ISO runs ide0:tglb-autorun after boot: network up, fetch lab\serve\step from the host and
# execute it (by default: load the bridge and run the tglbtest demo; edit lab\serve\step for your program).
#   lab\run.ps1          QEMU window; click OK in the MorphOS boot language window yourself
#   lab\run.ps1 -Hidden  no windows at all; the language window is confirmed through the QEMU monitor
param([switch]$Hidden, [int]$BootWait = 100)
. "$PSScriptRoot\common.ps1"
& "$PSScriptRoot\start-host.ps1" -Hidden:$Hidden
& "$PSScriptRoot\start-qemu.ps1" -Hidden:$Hidden
if ($Hidden) {
    Start-Sleep $BootWait
    # OK button of the boot language window (guest mouse only, through the QEMU monitor)
    & "$PSScriptRoot\mon.ps1" -Cmds 'mouse_move 0 92;mouse_move 0 93;mouse_button 1;mouse_button 0' -Pause 70 | Out-Null
    'language window confirmed'
} else {
    'Click OK in the MorphOS language window. The demo starts about 30 s after the desktop appears.'
}
"Guest logs arrive in $($C.InboxDir); host log: $(Join-Path $C.RunDir 'tglhost.log'). Stop with lab\stop.ps1."
