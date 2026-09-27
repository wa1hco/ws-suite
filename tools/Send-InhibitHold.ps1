<#
.SYNOPSIS
  Minimal TX Inhibit KEY-agent stand-in for Windows. Needs no keyboard focus.

.DESCRIPTION
  Sends the UDP hold protocol from docs/TX_INHIBIT.md so TX Inhibit can be
  exercised while you are driving the WSJT-X window with mouse and keyboard.

  The interactive helpers (inhibit-test, inhibit-test-gui) read a held key and
  therefore need focus themselves, which makes it impossible to hold the KEY
  and press Tune in WSJT-X at the same time. This script holds from a separate
  window instead, leaving WSJT-X free.

  Mirrors the Hold-sender half of a real KEY agent (docs/TX_INHIBIT.md 3):
  an immediate hold, keepalives about every 200 ms, and an explicit release
  (ttl_ms 0) on exit.

  Fail-safe: if this script is killed without releasing, the station's own hold
  timeout clears the hold after -TtlMs (default 600 ms). That is the deadman
  path, and it is the reason keepalives exist.

.EXAMPLE
  # Hold until Ctrl+C. Start this, then go press Tune in WSJT-X.
  .\Send-InhibitHold.ps1

.EXAMPLE
  # Hold for 20 seconds, then release automatically.
  .\Send-InhibitHold.ps1 -Seconds 20

.EXAMPLE
  # Clear a stuck hold.
  .\Send-InhibitHold.ps1 -Release

.NOTES
  -TargetHost, not -Host: $Host is a reserved PowerShell automatic variable.
#>
[CmdletBinding()]
param(
  [string]$TargetHost = '127.0.0.1',
  [int]$Port = 22372,
  [string]$Station = 'PS-TEST',
  # 0 = hold until Ctrl+C
  [int]$Seconds = 0,
  # Wire ttl_ms: how long the station keeps the hold without a new packet.
  # Protocol range is 100..30000.
  [int]$TtlMs = 600,
  [int]$KeepaliveMs = 200,
  [switch]$Release
)

$ErrorActionPreference = 'Stop'

if ($TtlMs -lt 100 -or $TtlMs -gt 30000) {
  throw "TtlMs must be 100..30000 (protocol range); got $TtlMs"
}
if ($Station -match '["\\]') {
  throw 'Station must not contain quotes or backslashes (the payload is hand-built JSON)'
}

$udp = [System.Net.Sockets.UdpClient]::new()
$seq = 0

function Send-Hold {
  param([int]$Ttl)
  $script:seq++
  $json = '{"tx_inhibit":1,"ttl_ms":' + $Ttl +
          ',"station":"' + $Station +
          '","seq":' + $script:seq + '}'
  $bytes = [Text.Encoding]::UTF8.GetBytes($json)
  [void]$udp.Send($bytes, $bytes.Length, $TargetHost, $Port)
  return $json
}

try {
  if ($Release) {
    $sent = Send-Hold -Ttl 0
    Write-Host "release -> ${TargetHost}:${Port}  $sent"
    return
  }

  Write-Host "Holding TX Inhibit on ${TargetHost}:${Port} (ttl ${TtlMs} ms, keepalive ${KeepaliveMs} ms)"
  if ($Seconds -gt 0) {
    Write-Host "Auto-release after $Seconds s."
  } else {
    Write-Host 'Ctrl+C to release. WSJT-X keeps focus - go press Tune.'
  }

  $deadline = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { $null }
  $ticks = 0
  while ($true) {
    [void](Send-Hold -Ttl $TtlMs)
    $ticks++
    # One line per second, so the console shows it is alive without scrolling.
    if (($ticks * $KeepaliveMs) % 1000 -lt $KeepaliveMs) {
      Write-Host ('  holding... {0:n0}s' -f ($ticks * $KeepaliveMs / 1000))
    }
    if ($deadline -and (Get-Date) -ge $deadline) { break }
    Start-Sleep -Milliseconds $KeepaliveMs
  }
}
finally {
  # Normal end of a hold is an explicit release, not a timeout.
  try {
    $sent = Send-Hold -Ttl 0
    Write-Host "released  $sent"
  } catch {
    Write-Warning "release failed: $_  (the hold will expire after ${TtlMs} ms anyway)"
  }
  $udp.Close()
}
