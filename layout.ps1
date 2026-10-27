<#
.SYNOPSIS
Lays out a game's build\<game>\bin as it goes into that game's install folder, so an install and a release hold the
same files.

.DESCRIPTION
games\<game>\game.cfg names the game's executable relative to its install folder: version.dll goes beside it, and
the windowFixes folder beside it gets every tool the game's windowFixes.txt names, all of which a newer build
replaces. No settings files: each tool carries its own and writes or updates them in the windowFixes folder when the
game starts, so a player's settings are never replaced. The diagnostic tools never ship; Copy-ToolLayout -diagnostics
adds them to an install, and the loader keeps a listed tool whose DLL the folder holds. game.cfg's architecture (x64
or x86) picks the build.
#>

$repositoryFolder = $PSScriptRoot

function Get-GameFolder {
  param([string]$game)
  $folder = Join-Path (Join-Path $repositoryFolder "games") $game
  if (-not (Test-Path -LiteralPath (Join-Path $folder "game.cfg"))) {
    throw "games\$game\game.cfg does not exist"
  }
  return $folder
}

function Get-GameSetting {
  param([string]$game, [string]$key)
  $configPath = Join-Path (Get-GameFolder $game) "game.cfg"
  $values = @()
  foreach ($line in Get-Content -LiteralPath $configPath) {
    if ($line -match ('^\s*' + $key + '\s*=\s*(.+?)\s*$')) {
      $values += $Matches[1]
    }
  }
  if ($values.Count -ne 1) {
    throw "$configPath must set $key exactly once"
  }
  return $values[0]
}

function Get-GameExecutable {
  param([string]$game)
  return Get-GameSetting $game "executable"
}

function Get-GameCompanions {
  # Programs beside the game's executable that also get the tools, such as a launcher step the game cannot start
  # without: file names, any number of companion= lines.
  param([string]$game)
  $configPath = Join-Path (Get-GameFolder $game) "game.cfg"
  $companions = @()
  foreach ($line in Get-Content -LiteralPath $configPath) {
    if ($line -match '^\s*companion\s*=\s*(.+?)\s*$') {
      $companion = $Matches[1]
      if ($companion -notmatch '^[^\\/:|]+\.exe$') {
        throw "$configPath`: companion must be an .exe file name beside the executable, not $companion"
      }
      $companions += $companion
    }
  }
  return $companions
}

function Get-GameArchitecture {
  param([string]$game)
  $architecture = Get-GameSetting $game "architecture"
  if (@("x64", "x86") -notcontains $architecture) {
    throw "games\$game\game.cfg: architecture must be x64 or x86, not $architecture"
  }
  return $architecture
}

function Get-DiagnosticTools {
  return @("stabilityGuard.dll", "displayGuard.dll")
}

function Get-ShippedTools {
  # Every tool the game's windowFixes.txt names, on or switched off with #: a player can turn any of them on.
  param([string]$game)
  $listPath = Join-Path (Get-GameFolder $game) "windowFixes.txt"
  $tools = @()
  foreach ($line in Get-Content -LiteralPath $listPath) {
    $name = $line.Trim().TrimStart("#").Trim()
    if ($name -match '^[^\\/: ]+\.dll$') {
      $tools += $name
    }
  }
  if ($tools.Count -eq 0) {
    throw "$listPath names no tools"
  }
  return $tools
}

function Copy-LayoutFile {
  param([string]$sourcePath, [string]$destinationFolder)
  if (-not (Test-Path -LiteralPath $sourcePath)) {
    throw "$sourcePath is missing; run build.ps1 first"
  }
  if (-not (Test-Path -LiteralPath $destinationFolder)) {
    New-Item -ItemType Directory -Path $destinationFolder -Force | Out-Null
  }
  Copy-Item -LiteralPath $sourcePath -Destination $destinationFolder -Force
}

function Copy-ReleaseFiles {
  param([string]$game, [string]$clientFolder)
  $binaryFolder = Join-Path (Join-Path (Join-Path $repositoryFolder "build") $game) "bin"
  $tools = Get-ShippedTools $game
  Copy-LayoutFile (Join-Path $binaryFolder "version.dll") $clientFolder
  foreach ($tool in $tools) {
    Copy-LayoutFile (Join-Path $binaryFolder $tool) (Join-Path $clientFolder "windowFixes")
  }
  return "version.dll, tools $($tools -join ', ')"
}

function Get-ReleaseFingerprint {
  # The SHA-256 of the release's files by name: the same files give the same fingerprint, whatever the zip's timestamps.
  param([string]$game)
  $binaryFolder = Join-Path (Join-Path (Join-Path $repositoryFolder "build") $game) "bin"
  $lines = @()
  foreach ($fileName in @("version.dll") + @(Get-ShippedTools $game)) {
    $path = Join-Path $binaryFolder $fileName
    if (-not (Test-Path -LiteralPath $path)) {
      throw "$path is missing; run build.ps1 first"
    }
    $lines += $fileName + " " + (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
  }
  $bytes = [System.Text.Encoding]::ASCII.GetBytes(($lines -join "`n"))
  $hasher = New-Object System.Security.Cryptography.SHA256Managed
  return ([System.BitConverter]::ToString($hasher.ComputeHash($bytes)) -replace "-", "").ToLowerInvariant()
}

function Add-DiagnosticTools {
  param([string]$game, [string]$clientFolder)
  $binaryFolder = Join-Path (Join-Path (Join-Path $repositoryFolder "build") $game) "bin"
  $toolsFolder = Join-Path $clientFolder "windowFixes"
  $diagnosticTools = @(Get-DiagnosticTools)
  foreach ($tool in $diagnosticTools) {
    Copy-LayoutFile (Join-Path $binaryFolder $tool) $toolsFolder
  }
  $listPath = Join-Path $toolsFolder "windowFixes.txt"
  $lines = @()
  if (Test-Path -LiteralPath $listPath) {
    $lines = @(Get-Content -LiteralPath $listPath)
  }
  $listed = @($lines | ForEach-Object { $_.Trim().TrimStart("#").Trim() })
  $missing = @($diagnosticTools | Where-Object { $listed -notcontains $_ })
  if ($missing.Count -gt 0) {
    [System.IO.File]::WriteAllText($listPath, ((@($missing) + $lines) -join "`r`n"), [System.Text.Encoding]::ASCII)
  }
  return "diagnostic tools $($diagnosticTools -join ', ')"
}

function Copy-ToolLayout {
  param([string]$game, [string]$destination, [switch]$diagnostics)
  $executablePath = Join-Path $destination (Get-GameExecutable $game)
  if (-not (Test-Path -LiteralPath $executablePath)) {
    throw "$destination has no $(Get-GameExecutable $game)"
  }
  $clientFolder = Split-Path $executablePath
  $installed = Copy-ReleaseFiles $game $clientFolder
  if ($diagnostics) {
    $installed += ", " + (Add-DiagnosticTools $game $clientFolder)
  }
  return $installed
}
