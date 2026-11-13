# SPDX-License-Identifier: MIT
# Dot-sourced by the lab scripts: loads ..\config.ps1 and resolves its paths to absolute ones.
$Root = Split-Path -Parent $PSScriptRoot
. (Join-Path $Root 'config.ps1')

function Resolve-LabPath([string]$p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return [System.IO.Path]::GetFullPath((Join-Path $Root $p))
}

$C = @{}
foreach ($k in $Config.Keys) { $C[$k] = $Config[$k] }
foreach ($k in 'QemuDir', 'MorphOSIso', 'LabIso', 'BootImg', 'TransferDir', 'InboxDir', 'TglHost', 'RunDir') {
    $C[$k] = Resolve-LabPath $Config[$k]
}
$C.ServeDirs = @($Config.ServeDirs | ForEach-Object { Resolve-LabPath $_ })
$C.QemuExe = Join-Path $C.QemuDir 'qemu-system-ppc.exe'
New-Item -ItemType Directory -Force $C.RunDir, $C.InboxDir | Out-Null

# Quote one argument for a Windows command line (Start-Process joins arguments without quoting).
function Quote-Arg([string]$a) {
    if ($a -notmatch '[\s"]') { return $a }
    return '"' + ($a -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Save-Pid([string]$name, [int]$id) { Set-Content -Path (Join-Path $C.RunDir "$name.pid") -Value $id }
function Get-SavedPid([string]$name) {
    $f = Join-Path $C.RunDir "$name.pid"
    if (Test-Path $f) { return [int](Get-Content $f | Select-Object -First 1) }
    return $null
}
