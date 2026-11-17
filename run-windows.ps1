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
  Internal draw resolution scale, 1-8 (default 2). Values > 1 render the guest's
  3D at a higher resolution and downsample, sharpening the image at the cost of
  GPU load. The default of 2 matches the sharper look of other Xenos emulators
  instead of the blurry 1x upscale. Use 1 for the original internal resolution,
  or 3 for even sharper output on a strong GPU.

.PARAMETER Windowed
  Run in a window instead of fullscreen.

.PARAMETER GameRoot
  Path to the extracted game tree (default C:\bs1game).

.EXAMPLE
  .\run-windows.ps1                       # 1920x1080 fullscreen, 2x scale (sharp)
  .\run-windows.ps1 -Scale 1              # original 1x internal resolution
  .\run-windows.ps1 -Scale 3              # even sharper, needs a strong GPU
  .\run-windows.ps1 -Resolution 2560x1440 -Scale 3 -Windowed
#>
param(
    [string]$Resolution = '1920x1080',
    [ValidateRange(1, 8)][int]$Scale = 2,
    [switch]$Windowed,
    [string]$GameRoot = 'C:\bs1game'
)

$ErrorActionPreference = 'Stop'

# Support two layouts:
#   1) Source repo: run-windows.ps1 at the repo root, exe under build\bs1-real
#   2) Release zip: run-windows.ps1 next to bioshock1.exe
$repoExe = Join-Path $PSScriptRoot 'build\bs1-real\bioshock1.exe'
$zipExe = Join-Path $PSScriptRoot 'bioshock1.exe'
if (Test-Path -LiteralPath $repoExe) {
    $exe = $repoExe
    $exeDir = Split-Path $repoExe -Parent
} elseif (Test-Path -LiteralPath $zipExe) {
    $exe = $zipExe
    $exeDir = $PSScriptRoot
} else {
    throw "Host launcher not found at '$repoExe' or '$zipExe'. Extract the whole release zip, or run 'python tools/pipeline.py build-native' from the repo."
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
