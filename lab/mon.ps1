# SPDX-License-Identifier: MIT
# Send one or more QEMU monitor commands (';'-separated) to the lab's monitor port and print the replies.
# Input goes to the guest only (QEMU mouse_move/mouse_button/sendkey), never to the host desktop.
#   lab\mon.ps1 "screendump run/shot.png -f png"
#   lab\mon.ps1 quit
param([Parameter(Mandatory = $true)][string]$Cmds, [int]$Pause = 80)
. "$PSScriptRoot\common.ps1"
$c = New-Object System.Net.Sockets.TcpClient('127.0.0.1', $C.MonitorPort)
$s = $c.GetStream(); $w = New-Object System.IO.StreamWriter($s); $w.AutoFlush = $true
Start-Sleep -Milliseconds 300
$buf = New-Object byte[] 65536; $out = ''
foreach ($x in ($Cmds -split ';')) {
    $w.WriteLine($x.Trim())
    Start-Sleep -Milliseconds $Pause
    while ($s.DataAvailable) { $n = $s.Read($buf, 0, $buf.Length); $out += [Text.Encoding]::ASCII.GetString($buf, 0, $n) }
}
Start-Sleep -Milliseconds 300
while ($s.DataAvailable) { $n = $s.Read($buf, 0, $buf.Length); $out += [Text.Encoding]::ASCII.GetString($buf, 0, $n) }
$c.Close()
($out -split "`n" | Where-Object { $_ -notmatch '^\s*$' -and $_ -notmatch 'QEMU .* monitor' -and $_ -notmatch '^\(qemu\)' }) -join "`n"
