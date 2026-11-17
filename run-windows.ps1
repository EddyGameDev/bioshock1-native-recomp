<#
.SYNOPSIS
  Launches the native Win64 BioShock 1 recompilation with configurable resolution.

.DESCRIPTION
  The runtime defaults to a 1280x720 window with 1x internal draw resolution,
  which looks very low on a modern monitor. Use this script to pick a larger
  window/video mode and optionally supersample the guest's rendering for a
  sharper image.

.PARAMETER Resolution
  Window/video mode as WxH (default 1920x1080). Equivalent to --ui.resolution.

.PARAMETER Scale
  Internal draw resolution scale, 1-8 (default 1). Values > 1 render the guest's
  3D at a higher resolution and downsample, sharpening the image at the cost of
  GPU load. Try 2 first.

.PARAMETER Windowed
  Run in a window instead of fullscreen.

.PARAMETER GameRoot
  Path to the extracted game tree (default C:\bs1game).

.EXAMPLE
  .\run-windows.ps1                       # 1920x1080 fullscreen, 1x scale
  .\run-windows.ps1 -Scale 2              # 1920x1080, 2x supersampling
  .\run-windows.ps1 -Resolution 2560x1440 -Scale 2 -Windowed
#>
param(
    [string]$Resolution = '1920x1080',
    [ValidateRange(1, 8)][int]$Scale = 1,
    [switch]$Windowed,
    [string]$GameRoot = 'C:\bs1game'
)

$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$exeDir = Join-Path $project 'build\bs1-real'
$exe = Join-Path $exeDir 'bioshock1.exe'

if (-not (Test-Path -LiteralPath $exe)) {
    throw "Host launcher not found: $exe (run 'python tools/pipeline.py build-native' first)"
}
if (-not (Test-Path -LiteralPath $GameRoot)) {
    Write-Warning "Game root '$GameRoot' does not exist; the title will fail to load."
}

$launchArgs = @(
    "--game_data_root=$GameRoot",
    "--ui.resolution=$Resolution",
    "--gpu.resolution_scale_x=$Scale",
    "--gpu.resolution_scale_y=$Scale"
)
if ($Windowed) { $launchArgs += '--ui.fullscreen=false' }

Write-Host "Launching: $exe"
Write-Host ("Args: " + ($launchArgs -join ' '))
Push-Location $exeDir
try {
    & $exe @launchArgs
} finally {
    Pop-Location
}
