<#
.SYNOPSIS
Installs the host + Android build toolchain for the BioShock 1 static recomp.

.DESCRIPTION
Idempotent, no administrator rights required. Downloads Android SDK packages
straight from dl.google.com (avoiding the interactive sdkmanager license
prompt) and unpacks them into $env:ANDROID_HOME (F:\tools\android-sdk):

  cmdline-tools/latest, platforms/android-34, build-tools/34.0.0,
  platform-tools, ndk/26.3.11579264, cmake/3.22.1

No game data, BIOS or proprietary Microsoft content is installed here.
#>

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$sdkRoot = $env:ANDROID_HOME
if (-not $sdkRoot) { $sdkRoot = 'F:\tools\android-sdk' }
New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null

$packages = @(
    @{ Name = 'cmdline-tools'; Url = 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip'; Dest = 'cmdline-tools/latest' },
    @{ Name = 'platform-34';   Url = 'https://dl.google.com/android/repository/platform-34-ext12_r01.zip';          Dest = 'platforms/android-34' },
    @{ Name = 'build-tools';   Url = 'https://dl.google.com/android/repository/build-tools_r34-windows.zip';        Dest = 'build-tools/34.0.0' },
    @{ Name = 'platform-tools';Url = 'https://dl.google.com/android/repository/platform-tools_r37.0.3-win.zip';      Dest = 'platform-tools' },
    @{ Name = 'ndk-r26c';      Url = 'https://dl.google.com/android/repository/android-ndk-r26c-windows.zip';        Dest = 'ndk/26.3.11579264' },
    @{ Name = 'cmake-3.22.1';  Url = 'https://dl.google.com/android/repository/cmake-3.22.1-windows.zip';            Dest = 'cmake/3.22.1' }
)

function Get-SingleChild([string]$dir) {
    $items = @(Get-ChildItem -LiteralPath $dir -Force)
    if ($items.Count -eq 1 -and $items[0].PSIsContainer) { return $items[0].FullName }
    return $dir
}

foreach ($pkg in $packages) {
    $dest = Join-Path $sdkRoot $pkg.Dest
    if (Test-Path $dest) {
        Write-Host "skip   $($pkg.Name) (already present)"
        continue
    }

    $file = Join-Path $env:TEMP (Split-Path $pkg.Url -Leaf)
    if (-not (Test-Path $file)) {
        Write-Host "download $($pkg.Name) -> $file"
        Invoke-WebRequest -Uri $pkg.Url -OutFile $file -UseBasicParsing
    }

    $tmp = Join-Path $env:TEMP ("bs1-sdk-" + $pkg.Name)
    if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Write-Host "expand  $($pkg.Name)"
    Expand-Archive -LiteralPath $file -DestinationPath $tmp -Force

    $src = Get-SingleChild $tmp
    # The cmdline-tools zip unpacks to a folder named 'cmdline-tools' which
    # must land at sdk/cmdline-tools/latest.
    New-Item -ItemType Directory -Force -Path (Split-Path $dest -Parent) | Out-Null
    Move-Item -LiteralPath $src -Destination $dest -Force
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "installed $($pkg.Name) -> $dest"
}

Write-Host "`nSDK tree:"
Get-ChildItem $sdkRoot -Directory | Select-Object Name | Format-Table -AutoSize
$cmake = Join-Path $sdkRoot 'cmake\3.22.1\bin\cmake.exe'
if (Test-Path $cmake) { Write-Host "Host CMake: $cmake"; & $cmake --version | Select-Object -First 1 }
Write-Host 'Done.'
