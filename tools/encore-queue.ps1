<#
.SYNOPSIS
  Example client for Encore's song queue: add a song to the setlist of a running Clone Hero, or
  show the setlist.

.DESCRIPTION
  POST /queue looks the song up by chart checksum (as shown by enchor.us) or by title and optional
  artist, and appends it to the game's setlist:
  - in the song list it is added like the "add to setlist" button (the setlist panel opens);
  - while a setlist is playing it is played after the songs already in the setlist.
  It is refused in practice mode, during a single (quickplay) song and in other menus.
  GET /setlist returns the current setlist.

.EXAMPLE
  .\encore-queue.ps1 -Checksum 0123456789abcdef0123456789abcdef
  .\encore-queue.ps1 -Title 'Through the Fire and Flames' -Artist 'DragonForce'
  .\encore-queue.ps1 -Title 'Soulless 4' -Speed 80
  .\encore-queue.ps1 -List
#>
param(
    [string]$Checksum,
    [string]$Title,
    [string]$Artist,
    [int]$Speed = 0,
    [switch]$List,
    [int]$Port = 47321
)
$ErrorActionPreference = 'Stop'
$base = "http://127.0.0.1:$Port"

# Returns the parsed JSON body for success and error statuses alike.
function Call([string]$Method, [string]$Path, [string]$Body) {
  $params = @{ UseBasicParsing = $true; Method = $Method; Uri = "$base$Path"; TimeoutSec = 10 }
  if ($Body) {
    $params.Body = [System.Text.Encoding]::UTF8.GetBytes($Body)
    $params.ContentType = 'application/json; charset=utf-8'
  }
  try {
    $r = Invoke-WebRequest @params
    $text = [System.Text.Encoding]::UTF8.GetString($r.RawContentStream.ToArray())
    return [pscustomobject]@{ Code = [int]$r.StatusCode; Body = $text | ConvertFrom-Json }
  } catch [System.Net.WebException] {
    $resp = $_.Exception.Response
    if (-not $resp) { throw "Encore is not reachable at $base (is Clone Hero running with Encore installed?)" }
    $text = $_.ErrorDetails.Message
    if (-not $text) { $text = (New-Object System.IO.StreamReader($resp.GetResponseStream())).ReadToEnd() }
    return [pscustomobject]@{ Code = [int]$resp.StatusCode; Body = $text | ConvertFrom-Json }
  }
}

if ($List -or (-not $Checksum -and -not $Title)) {
  $r = Call GET /setlist
  if ($r.Code -ne 200) { "$($r.Code) $($r.Body.result): $($r.Body.message)"; exit 1 }
  "playing setlist: $($r.Body.playing), index: $($r.Body.index)"
  $i = 0
  foreach ($s in $r.Body.songs) {
    $i++
    '{0,3}. {1} - {2} ({3}%) {4}' -f $i, $s.artist, $s.title, $s.speed, $s.checksum
  }
  exit 0
}

$req = @{}
if ($Checksum) { $req.checksum = $Checksum }
if ($Title) { $req.title = $Title }
if ($Artist) { $req.artist = $Artist }
if ($Speed -gt 0) { $req.speed = $Speed }
$r = Call POST /queue ($req | ConvertTo-Json -Compress)
if ($r.Code -eq 200) {
  $s = $r.Body.song
  "queued ($($r.Body.mode)): $($s.artist) - $($s.title) at $($r.Body.speed)%, setlist position $($r.Body.position)"
  if ($r.Body.matches -gt 1) { "note: $($r.Body.matches) songs matched, the first one was queued" }
  exit 0
}
"$($r.Code) $($r.Body.result): $($r.Body.message)"
exit 1
