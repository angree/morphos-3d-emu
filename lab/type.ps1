# SPDX-License-Identifier: MIT
# Type a line into the guest through QEMU sendkey (guest keyboard only, never host input), then Return.
#   lab\type.ps1 "execute RAM:step"
param([Parameter(Mandatory = $true)][string]$Text, [switch]$NoEnter)
$map = @{ ' ' = 'spc'; ':' = 'shift-semicolon'; ';' = 'semicolon'; '/' = 'slash'; '.' = 'dot'; ',' = 'comma';
          '-' = 'minus'; '_' = 'shift-minus'; '=' = 'equal'; '"' = 'shift-apostrophe'; "'" = 'apostrophe';
          '>' = 'shift-dot'; '<' = 'shift-comma'; '*' = 'shift-8'; '#' = 'shift-3'; '?' = 'shift-slash';
          '(' = 'shift-9'; ')' = 'shift-0'; '$' = 'shift-4'; '!' = 'shift-1'; '|' = 'shift-backslash';
          '%' = 'shift-5'; '+' = 'shift-equal'; '[' = 'bracket_left'; ']' = 'bracket_right' }
$keys = @()
foreach ($ch in $Text.ToCharArray()) {
    $s = [string]$ch
    if ($map.ContainsKey($s)) { $keys += $map[$s] }
    elseif ($s -cmatch '[a-z0-9]') { $keys += $s }
    elseif ($s -cmatch '[A-Z]') { $keys += 'shift-' + $s.ToLower() }
    else { throw "no key for '$s'" }
}
if (-not $NoEnter) { $keys += 'ret' }
$cmds = ($keys | ForEach-Object { "sendkey $_" }) -join ';'
& "$PSScriptRoot\mon.ps1" -Cmds $cmds -Pause 40 | Out-Null
