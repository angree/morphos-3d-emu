# SPDX-License-Identifier: MIT
# Stop a lab run: QEMU through its monitor ('quit'), tglhost and the file server by the PIDs this lab saved.
# Never stops anything by process name (other QEMU instances on the machine are left alone).
. "$PSScriptRoot\common.ps1"
try { & "$PSScriptRoot\mon.ps1" -Cmds 'quit' | Out-Null; 'qemu: quit sent' } catch { 'qemu: monitor not reachable (not running?)' }
foreach ($n in 'tglhost', 'tglbserve') {
    $id = Get-SavedPid $n
    if ($id) {
        $p = Get-Process -Id $id -ErrorAction SilentlyContinue
        if ($p -and $p.ProcessName -match '^(tglhost|powershell)$') { Stop-Process -Id $id -Force; "${n}: stopped pid $id" } else { "${n}: pid $id not running" }
        Remove-Item (Join-Path $C.RunDir "$n.pid") -ErrorAction SilentlyContinue
    }
}
