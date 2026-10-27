<#
.SYNOPSIS
Publishes a GitHub release for each game whose release files changed since its last release.

.DESCRIPTION
For every game under games: builds it stamped "unversioned" in place of the commit, so the DLLs depend only on the
code, and takes the release's fingerprint (layout.ps1). A game whose newest release carries the same fingerprint in
its notes is left alone. Otherwise it is built again stamped with the checked-out commit, packed by package.ps1 into
releases\borderless-window-fixes-<game>.zip and, with -publish, released as <game>-<yyyy.MM.dd>-<commit> with its
release.md as title and notes. -test runs the Python client's tests with each build. Without -publish it only reports which games would be released. gh must be logged in.
#>
param(
  [switch]$test,
  [switch]$publish
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "layout.ps1")
$repository = "rbraunm/borderless-window-fixes"
$unversioned = "unversioned"
$fingerprintPattern = '<!-- fingerprint ([0-9a-f]{64}) -->'

function Write-ReleaseLog {
  param([string]$message)
  Write-Host "[release] $message"
}

function Invoke-Script {
  param([string]$scriptName, [string[]]$arguments)
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot $scriptName) @arguments
  if ($LASTEXITCODE -ne 0) {
    throw "$scriptName $($arguments -join ' ') failed (exit code $LASTEXITCODE)"
  }
}

function Get-PublishedFingerprint {
  param([string]$game)
  $releases = & gh release list -R $repository --limit 1000 --json tagName,createdAt | ConvertFrom-Json
  if ($LASTEXITCODE -ne 0) {
    throw "gh release list failed"
  }
  $newest = $releases | Where-Object { $_.tagName.StartsWith("$game-") } | Sort-Object createdAt -Descending | Select-Object -First 1
  if (-not $newest) {
    return $null
  }
  $body = & gh release view $newest.tagName -R $repository --json body -q .body
  if ($LASTEXITCODE -ne 0) {
    throw "gh release view $($newest.tagName) failed"
  }
  if (($body -join "`n") -match $fingerprintPattern) {
    return $Matches[1]
  }
  return "none in $($newest.tagName)"
}

function Publish-Game {
  param([string]$game, [string]$fingerprint)
  $commit = (& git -C $PSScriptRoot rev-parse --short HEAD)
  if ($LASTEXITCODE -ne 0) {
    throw "git rev-parse failed"
  }
  Invoke-Script "build.ps1" @("-game", $game)
  $releasesFolder = Join-Path $PSScriptRoot "releases"
  New-Item -ItemType Directory -Path $releasesFolder -Force | Out-Null
  $zipPath = Join-Path $releasesFolder "borderless-window-fixes-$game.zip"
  Invoke-Script "package.ps1" @("-game", $game, "-outputPath", $zipPath)
  if (-not $publish) {
    return
  }
  $notes = @(Get-Content -Encoding UTF8 -LiteralPath (Join-Path (Get-GameFolder $game) "release.md"))
  if ($notes[0] -notmatch '^# (.+)$') {
    throw "games\$game\release.md must start with a # title"
  }
  $stamp = (Get-Date).ToUniversalTime().ToString("yyyy.MM.dd")
  $title = "$($Matches[1]) $stamp ($commit)"
  $notesPath = Join-Path $releasesFolder "$game-notes.md"
  $body = (($notes | Select-Object -Skip 1) + @("", "<!-- fingerprint $fingerprint -->")) -join "`n"
  [System.IO.File]::WriteAllText($notesPath, $body, (New-Object System.Text.UTF8Encoding $false))
  $fullCommit = (& git -C $PSScriptRoot rev-parse HEAD)
  & gh release create "$game-$stamp-$commit" $zipPath -R $repository --target $fullCommit --title $title --notes-file $notesPath
  if ($LASTEXITCODE -ne 0) {
    throw "gh release create failed for $game"
  }
}

foreach ($game in (Get-ChildItem -Directory -LiteralPath (Join-Path $PSScriptRoot "games")).Name) {
  $arguments = @("-game", $game, "-buildCommit", $unversioned)
  if ($test) {
    $arguments += "-test"
  }
  Invoke-Script "build.ps1" $arguments
  $fingerprint = Get-ReleaseFingerprint $game
  $published = Get-PublishedFingerprint $game
  if ($published -eq $fingerprint) {
    Write-ReleaseLog "$game unchanged (fingerprint $fingerprint)"
    continue
  }
  Write-ReleaseLog "$game changed: fingerprint $fingerprint, last release $published"
  Publish-Game $game $fingerprint
}
