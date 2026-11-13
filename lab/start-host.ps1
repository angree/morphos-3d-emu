# SPDX-License-Identifier: MIT
# Start the Windows side: tglhost.exe (renders the guest's GL with the PC's OpenGL driver) and the file
# server the guest's tglbget talks to. Both listen on 127.0.0.1 only.
#   -Hidden          : tglhost renders off-screen (no window); use --shot via -TglArgs to get BMPs
#   -TglArgs '...'   : extra tglhost options, e.g. '--shot 1,60,149 --shotdir run\shots'
param([switch]$Hidden, [string]$TglArgs = '')
. "$PSScriptRoot\common.ps1"
if (-not (Test-Path $C.TglHost)) { throw "missing: $($C.TglHost) (build it, see docs/BUILD.md, or use the release zip)" }

$targs = "--port $($C.BridgePort) --log $(Quote-Arg (Join-Path $C.RunDir 'tglhost.log'))"
if ($Hidden) { $targs += ' --hidden' }
if ($TglArgs) { $targs += " $TglArgs" }
$h = Start-Process -FilePath $C.TglHost -ArgumentList $targs -WindowStyle Minimized -PassThru
Save-Pid 'tglhost' $h.Id

$sargs = "-NoProfile -ExecutionPolicy Bypass -File $(Quote-Arg (Join-Path $PSScriptRoot 'tglbserve.ps1')) " +
         "-Port $($C.ServePort) -Inbox $(Quote-Arg $C.InboxDir) -Serve " + (Quote-Arg ($C.ServeDirs -join ';'))
$s = Start-Process -FilePath 'powershell.exe' -ArgumentList $sargs -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $C.RunDir 'tglbserve.log')
Save-Pid 'tglbserve' $s.Id
"tglhost pid $($h.Id) (127.0.0.1:$($C.BridgePort)), tglbserve pid $($s.Id) (127.0.0.1:$($C.ServePort))"
