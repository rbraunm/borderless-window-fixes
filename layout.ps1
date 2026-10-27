<#
.SYNOPSIS
Lays out a game's build\<game>\bin as it goes into that game's install folder, so an install and a release hold the
same files.

.DESCRIPTION
games\<game>\game.cfg names the game's executable relative to its install folder: version.dll goes beside it, and
the windowFixes folder beside it gets every tool the game's windowFixes.txt names, all of which a newer build
replaces. No settings files: each tool carries its own and writes or updates them in the windowFixes folder when the
game starts, so a player's settings are never replaced. The diagnostic tools never ship; Copy-ToolLayout -diagnostics
adds them to an install, and the loader keeps a listed tool whose DLL the folder holds.
#>

$repositoryFolder = $PSScriptRoot
$diagnosticTools = @("stabilityGuard.dll", "displayGuard.dll")

function Get-GameFolder {
  param([string]$game)
  $folder = Join-Path (Join-Path $repositoryFolder "games") $game
  if (-not (Test-Path -LiteralPath (Join-Path $folder "game.cfg"))) {
    throw "games\$game\game.cfg does not exist"
  }
  return $folder
}

function Get-GameExecutable {
  param([string]$game)
  $configPath = Join-Path (Get-GameFolder $game) "game.cfg"
  $executables = @()
  foreach ($line in Get-Content -LiteralPath $configPath) {
    if ($line -match '^\s*executable\s*=\s*(.+?)\s*$') {
      $executables += $Matches[1]
    }
  }
  if ($executables.Count -ne 1) {
    throw "$configPath must set executable exactly once"
  }
  return $executables[0]
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

function Add-DiagnosticTools {
  param([string]$game, [string]$clientFolder)
  $binaryFolder = Join-Path (Join-Path (Join-Path $repositoryFolder "build") $game) "bin"
  $toolsFolder = Join-Path $clientFolder "windowFixes"
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
