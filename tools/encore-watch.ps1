<#
.SYNOPSIS
  Watch Clone Hero song folders and start an Encore rescan when charts are added or changed.

.DESCRIPTION
  Watches the given folders (recursively) for changes to song.ini, notes.chart, notes.mid and
  .sng files, and for new or renamed folders that contain them. After a burst of changes it waits
  until nothing has changed for -QuietSeconds (so a downloader can finish extracting), then calls
  POST /rescan on Encore.

  If the song list is not open (409 not_in_song_list), the rescan stays pending and the watcher
  polls GET /status until the song list is open, then retries. If a scan is already running
  (409 scanning), it retries after that scan. If Encore is not reachable or not ready, it retries
  every few seconds. Stop the watcher with Ctrl+C.

.EXAMPLE
  .\encore-watch.ps1
  .\encore-watch.ps1 -Path 'D:\Songs', 'E:\More Songs' -QuietSeconds 10
#>
param(
  [string[]]$Path = @(Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Clone Hero\Songs'),
  [int]$QuietSeconds = 5,
  [int]$PollSeconds = 2,
  [int]$Port = 47321
)
$ErrorActionPreference = 'Stop'
$base = "http://127.0.0.1:$Port"
$chartFiles = @('song.ini', 'notes.chart', 'notes.mid')

function Log([string]$Text) { Write-Host ('{0:HH:mm:ss} {1}' -f (Get-Date), $Text) }

# Returns the parsed JSON body for success and error statuses alike, or $null if Encore is not
# reachable (game not running, or Encore not installed).
function Call([string]$Method, [string]$Path) {
  try {
    $r = Invoke-WebRequest -UseBasicParsing -Method $Method -Uri "$base$Path" -TimeoutSec 10
    return [pscustomobject]@{ Code = [int]$r.StatusCode; Body = $r.Content | ConvertFrom-Json }
  } catch [System.Net.WebException] {
    $resp = $_.Exception.Response
    if (-not $resp) { return $null }
    # PowerShell 5.1 has already read the error body into ErrorDetails.
    $text = $_.ErrorDetails.Message
    if (-not $text) { $text = (New-Object System.IO.StreamReader($resp.GetResponseStream())).ReadToEnd() }
    return [pscustomobject]@{ Code = [int]$resp.StatusCode; Body = $text | ConvertFrom-Json }
  }
}

function IsChartFile([string]$FullPath) {
  $name = [IO.Path]::GetFileName($FullPath)
  return ($chartFiles -contains $name.ToLowerInvariant()) -or $name.EndsWith('.sng', 'OrdinalIgnoreCase')
}

# A change is relevant if it touches a chart file, or creates/renames a folder that already
# contains one (e.g. a downloader extracting to a temp folder and renaming it at the end).
function IsRelevant($EventArgs) {
  $full = $EventArgs.FullPath
  if (IsChartFile $full) { return $true }
  if ($EventArgs.ChangeType -in 'Created', 'Renamed' -and (Test-Path -LiteralPath $full -PathType Container)) {
    $hit = Get-ChildItem -LiteralPath $full -Recurse -File -ErrorAction SilentlyContinue |
      Where-Object { IsChartFile $_.Name } | Select-Object -First 1
    return [bool]$hit
  }
  return $false
}

$watchers = @()
$sourceIds = @()
try {
  foreach ($p in $Path) {
    if (-not (Test-Path -LiteralPath $p -PathType Container)) { throw "Song folder not found: $p" }
    $w = New-Object IO.FileSystemWatcher
    $w.Path = (Resolve-Path -LiteralPath $p).ProviderPath
    $w.IncludeSubdirectories = $true
    $w.InternalBufferSize = 65536
    $w.NotifyFilter = [IO.NotifyFilters]'FileName, DirectoryName, LastWrite, Size'
    foreach ($evt in 'Created', 'Changed', 'Deleted', 'Renamed', 'Error') {
      $id = "encore-watch-$($watchers.Count)-$evt"
      Register-ObjectEvent -InputObject $w -EventName $evt -SourceIdentifier $id | Out-Null
      $sourceIds += $id
    }
    $w.EnableRaisingEvents = $true
    $watchers += $w
    Log "watching $($w.Path)"
  }
  Log "rescan after ${QuietSeconds}s without changes; Ctrl+C to stop"

  $pending = $false      # a rescan is owed
  $lastChange = [datetime]::MinValue
  $nextTry = [datetime]::MinValue
  $waitingFor = ''       # why the pending rescan is held back, to log each reason once

  while ($true) {
    $e = Wait-Event -Timeout 1
    while ($e) {
      Remove-Event -EventIdentifier $e.EventIdentifier
      if ($sourceIds -contains $e.SourceIdentifier) {
        if ($e.SourceIdentifier.EndsWith('-Error')) {
          Log 'watcher buffer overflow, scheduling a rescan to be safe'
          $relevant = $true
        } else {
          $relevant = IsRelevant $e.SourceEventArgs
        }
        if ($relevant) {
          if (-not $pending) { Log "change: $($e.SourceEventArgs.ChangeType) $($e.SourceEventArgs.FullPath)" }
          $pending = $true
          $lastChange = Get-Date
        }
      }
      $e = Get-Event | Select-Object -First 1
    }

    $now = Get-Date
    if (-not $pending -or $now -lt $lastChange.AddSeconds($QuietSeconds) -or $now -lt $nextTry) { continue }

    if ($waitingFor -in 'not_in_song_list', 'scanning') {
      # Poll the cheap status endpoint until the song list is open and idle, then retry.
      $s = Call GET /status
      if (-not $s -or $s.Code -ne 200 -or -not $s.Body.songList -or $s.Body.scanning) {
        $nextTry = $now.AddSeconds($PollSeconds)
        continue
      }
    }

    $r = Call POST /rescan
    $result = if ($r) { $r.Body.result } else { 'unreachable' }
    if ($result -eq 'started') {
      Log 'rescan started'
      $pending = $false
      $waitingFor = ''
      continue
    }
    if ($result -ne $waitingFor) {
      switch ($result) {
        'not_in_song_list' { Log 'rescan pending: waiting for the song list to be opened' }
        'scanning'         { Log 'rescan pending: waiting for the running scan to finish' }
        'unreachable'      { Log "rescan pending: Encore not reachable at $base (is Clone Hero running?)" }
        default            { Log "rescan pending: $($r.Code) $result, retrying" }
      }
      $waitingFor = $result
    }
    $nextTry = $now.AddSeconds($(if ($result -in 'not_in_song_list', 'scanning') { $PollSeconds } else { 5 }))
  }
} finally {
  foreach ($id in $sourceIds) { Unregister-Event -SourceIdentifier $id -ErrorAction SilentlyContinue }
  foreach ($w in $watchers) { $w.Dispose() }
  Get-Event | Remove-Event -ErrorAction SilentlyContinue
}
