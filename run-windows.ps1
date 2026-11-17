<#
.SYNOPSIS
  Launches the native Win64 BioShock 1 recompilation with configurable resolution.

.DESCRIPTION
  The guest renders at 1280x720. The sharpest image is a native 1280x720 window
  at 1x scale (1:1 pixels, no upscaling) - this matches Xenia's default look.
  Stretching that buffer to 1920x1080 fullscreen makes it visibly soft. Use a
  higher -Scale to supersample for a sharp large window, or -Fullscreen to fill
  the screen.

.PARAMETER Resolution
  Window/video mode as WxH (default 1280x720 = native, sharpest).

.PARAMETER Scale
  Internal draw resolution scale, 1-8 (default 1). 1x at 1280x720 is pixel-perfect.
  For a larger sharp window, e.g. 1920x1080, use -Scale 2 (renders 2560x1440
  internally and downsamples). 3x is even sharper but needs a strong GPU.

.PARAMETER Fullscreen
  Fill the screen instead of windowed. Pair with a higher -Scale for sharpness.

.PARAMETER GameRoot
  Path to the extracted game tree (default C:\bs1game).

.EXAMPLE
  .\run-windows.ps1                       # 1280x720 windowed, native 1:1 (sharpest)
  .\run-windows.ps1 -Fullscreen -Scale 2  # 1920x1080 fullscreen, sharp 2x
  .\run-windows.ps1 -Resolution 1920x1080 -Scale 2
  .\run-windows.ps1 -Resolution 2560x1440 -Scale 3
#>
param(
    [string]$Resolution = '1280x720',
    [ValidateRange(1, 8)][int]$Scale = 1,
    [switch]$Fullscreen,
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
if ($Fullscreen) { $launchArgs += '--ui.fullscreen=true' }
else { $launchArgs += '--ui.fullscreen=false' }

Write-Host "Launching: $exe"
Write-Host ("Args: " + ($launchArgs -join ' '))
Push-Location $exeDir
try {
    & $exe @launchArgs
} finally {
    Pop-Location
}
