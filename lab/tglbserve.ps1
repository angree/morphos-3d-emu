# SPDX-License-Identifier: MIT
# File server for the guest tool tglbget (same protocol as bridge\host\tglbserve.py, no Python needed).
#   GET <name>\n          -> u32 big-endian length + data, or 0xFFFFFFFF if missing
#   PUT <name> <size>\n + data -> file written to the inbox, reply "OK\n"
# -Serve takes one or more folders separated by ';' (searched in order).
# Listens on 127.0.0.1 only (the QEMU guest reaches it as 10.0.2.2). Names: [A-Za-z0-9._-]{1,64} only.
#   powershell -File lab\tglbserve.ps1 -Port 7660 -Inbox inbox -Serve 'lab\serve;bin\guest'
param([int]$Port = 7660, [Parameter(Mandatory = $true)][string]$Inbox, [Parameter(Mandatory = $true)][string]$Serve)
$ErrorActionPreference = 'Stop'
$Inbox = [System.IO.Path]::GetFullPath($Inbox)
$Dirs = @(($Serve -split ';') | Where-Object { $_ } | ForEach-Object { [System.IO.Path]::GetFullPath($_) })
New-Item -ItemType Directory -Force $Inbox | Out-Null
$ok = '^[A-Za-z0-9._-]{1,64}$'
function Log($m) { [Console]::Out.WriteLine(("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m)); [Console]::Out.Flush() }
function Be32([uint32]$v) { $b = [BitConverter]::GetBytes($v); [Array]::Reverse($b); return ,$b }

$l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
$l.Start()
Log "tglbserve on 127.0.0.1:$Port serve=$($Dirs -join ';') inbox=$Inbox"
while ($true) {
    $c = $l.AcceptTcpClient()
    try {
        $c.ReceiveTimeout = 30000
        $s = $c.GetStream()
        $line = New-Object System.Text.StringBuilder
        while ($line.Length -lt 300) {
            $b = $s.ReadByte()
            if ($b -lt 0 -or $b -eq 10) { break }
            [void]$line.Append([char]$b)
        }
        $parts = $line.ToString().Trim() -split '\s+'
        if ($parts.Count -ge 2 -and $parts[0] -eq 'GET' -and $parts[1] -match $ok) {
            $f = $Dirs | ForEach-Object { Join-Path $_ $parts[1] } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
            if ($f) {
                $data = [System.IO.File]::ReadAllBytes($f)
                $h = Be32 ([uint32]$data.Length); $s.Write($h, 0, 4); $s.Write($data, 0, $data.Length)
                Log "GET $($parts[1]) $($data.Length)"
            } else {
                $h = Be32 ([uint32]::MaxValue); $s.Write($h, 0, 4)
                Log "GET $($parts[1]) MISSING"
            }
        } elseif ($parts.Count -eq 3 -and $parts[0] -eq 'PUT' -and $parts[1] -match $ok) {
            $n = [int]$parts[2]; $data = New-Object byte[] $n; $got = 0
            while ($got -lt $n) {
                $r = $s.Read($data, $got, $n - $got)
                if ($r -le 0) { throw 'short read' }
                $got += $r
            }
            [System.IO.File]::WriteAllBytes((Join-Path $Inbox $parts[1]), $data)
            $okb = [Text.Encoding]::ASCII.GetBytes("OK`n"); $s.Write($okb, 0, $okb.Length)
            Log "PUT $($parts[1]) $n"
        } else {
            Log "bad request: $($line.ToString())"
        }
    } catch {
        Log "error: $_"
    } finally {
        $c.Close()
    }
}
