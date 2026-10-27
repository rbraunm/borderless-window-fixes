<#
.SYNOPSIS
Builds a game's release: a zip to extract straight into the game's install folder.

.DESCRIPTION
The zip holds what layout.ps1 lays out for the game, at the executable's place in its install folder: version.dll and
a windowFixes folder with every tool the game's windowFixes.txt names. It needs build.ps1 run for the game first. It
carries no settings files: each tool writes its own with the shipped defaults the first time the game starts, and a
later release only adds the settings it introduces, so an update never resets a player's.

.PARAMETER game
A folder under games.

.PARAMETER outputPath
The zip to write. It must not exist yet.
#>
param(
  [Parameter(Mandatory = $true)][string]$game,
  [Parameter(Mandatory = $true)][string]$outputPath
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "layout.ps1")

function Write-PackageLog {
  param([string]$message)
  Write-Host "[package] $message"
}

function Write-Zip {
  param([string]$sourceFolder)
  Add-Type -AssemblyName System.IO.Compression
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  # Entries are added one by one: Windows PowerShell's CreateFromDirectory names them with backslashes, which the zip
  # format does not allow and some extractors keep as part of the file name.
  $archive = [System.IO.Compression.ZipFile]::Open($outputPath, [System.IO.Compression.ZipArchiveMode]::Create)
  try {
    foreach ($entry in Get-ChildItem -LiteralPath $sourceFolder -Recurse -Force -File) {
      $entryName = $entry.FullName.Substring($sourceFolder.Length + 1).Replace("\", "/")
      [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $entry.FullName, $entryName,
        [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
  } finally {
    $archive.Dispose()
  }
}

$outputPath = [System.IO.Path]::GetFullPath($outputPath)
if (Test-Path -LiteralPath $outputPath) {
  throw "$outputPath already exists"
}
$stagingRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("windowFixesPackage" + [System.Guid]::NewGuid().ToString("N"))
$clientFolder = $stagingRoot
$executableFolder = Split-Path (Get-GameExecutable $game)
if ($executableFolder) {
  $clientFolder = Join-Path $stagingRoot $executableFolder
}
New-Item -ItemType Directory -Path $clientFolder -Force | Out-Null
try {
  Write-PackageLog ("added " + (Copy-ReleaseFiles $game $clientFolder))
  Write-Zip $stagingRoot
  Write-PackageLog ("wrote {0}: {1:N0} bytes" -f $outputPath, (Get-Item -LiteralPath $outputPath).Length)
} finally {
  Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
