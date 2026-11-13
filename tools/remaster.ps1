# SPDX-License-Identifier: MIT
# Windows wrapper for tools/remaster.sh: runs it in WSL (needs a WSL distribution with xorriso:
# wsl sudo apt install xorriso). Reads MorphOSIso, LabIso and BootImg from config.ps1.
#   powershell -ExecutionPolicy Bypass -File tools\remaster.ps1 [-Distro Ubuntu-24.04]
param([string]$Distro = '')
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'lab\common.ps1')
if (-not (Test-Path $C.MorphOSIso)) { throw "MorphOS ISO not found: $($C.MorphOSIso) - download it from https://www.morphos-team.net/ and set MorphOSIso in config.ps1" }
$outDir = Split-Path -Parent $C.LabIso
New-Item -ItemType Directory -Force $outDir | Out-Null
$d = @(); if ($Distro) { $d = @('-d', $Distro) }
function ToWsl([string]$p) { (& wsl.exe @d -- wslpath -a ($p -replace '\', '/')).Trim() }
$sh = ToWsl (Join-Path $PSScriptRoot 'remaster.sh')
& wsl.exe @d -- bash $sh (ToWsl $C.MorphOSIso) (ToWsl $outDir)
if ($LASTEXITCODE -ne 0) { throw "remaster.sh failed ($LASTEXITCODE)" }
$made = Join-Path $outDir 'morphos-3.20-tglb.iso'
if ($made -ne $C.LabIso) { Move-Item $made $C.LabIso -Force }
if ((Join-Path $outDir 'boot.img') -ne $C.BootImg) { Copy-Item (Join-Path $outDir 'boot.img') $C.BootImg -Force }
"lab ISO: $($C.LabIso)"; "boot.img: $($C.BootImg)"
