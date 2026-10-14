<#
.SYNOPSIS
  Example client for Encore's trigger interface: start a song rescan in a running Clone Hero,
  or show its status.

.DESCRIPTION
  Encore listens on http://127.0.0.1:47321 (ENCORE_PORT overrides the port in the game's
  environment). POST /rescan starts a full scan if the song list is open; GET /status reports
  whether Encore is ready, the song list is open and a scan is running.

.EXAMPLE
  .\encore-rescan.ps1              # start a rescan
  .\encore-rescan.ps1 -Status      # show status
  .\encore-rescan.ps1 -Wait        # start a rescan and wait until it has finished
#>
param([switch]$Status, [switch]$Wait, [int]$Port = 47321)
$ErrorActionPreference = 'Stop'
$base = "http://127.0.0.1:$Port"

# Returns the parsed JSON body for success and error statuses alike.
function Call([string]$Method, [string]$Path) {
  try {
    $r = Invoke-WebRequest -UseBasicParsing -Method $Method -Uri "$base$Path" -TimeoutSec 10
    return [pscustomobject]@{ Code = [int]$r.StatusCode; Body = $r.Content | ConvertFrom-Json }
  } catch [System.Net.WebException] {
    $resp = $_.Exception.Response
    if (-not $resp) { throw "Encore is not reachable at $base (is Clone Hero running with Encore installed?)" }
    # PowerShell 5.1 has already read the error body into ErrorDetails.
    $text = $_.ErrorDetails.Message
    if (-not $text) { $text = (New-Object System.IO.StreamReader($resp.GetResponseStream())).ReadToEnd() }
    return [pscustomobject]@{ Code = [int]$resp.StatusCode; Body = $text | ConvertFrom-Json }
  }
}

if ($Status) {
  $s = Call GET /status
  $s.Body
  exit ([int]($s.Code -ne 200))
}

$r = Call POST /rescan
"$($r.Code) $($r.Body.result): $($r.Body.message)"
if ($r.Code -ne 202) { exit 1 }
if ($Wait) {
  do {
    Start-Sleep -Milliseconds 500
    $s = Call GET /status
  } while ($s.Code -ne 200 -or $s.Body.scanning)
  'scan finished'
}
