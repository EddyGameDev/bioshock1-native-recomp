<#
.SYNOPSIS
Downloads the prebuilt ReXGlue SDK host toolkit (rexglue.exe) from GitHub Releases.

.DESCRIPTION
The vendored third_party/rexglue-sdk source snapshot has 22 uninitialised git
submodules and needs Clang 20+/CMake 3.25+ to build. The upstream project
publishes prebuilt win-amd64 SDK archives, which are a much faster path to a
working host recompiler. This script installs v0.11.0 into tools-bin/.

Run once; re-running skips existing files.
#>

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$project = Split-Path $PSScriptRoot -Parent
$toolsBin = Join-Path $project 'tools-bin'
New-Item -ItemType Directory -Force -Path $toolsBin | Out-Null

$version = '0.11.0'
$asset = "rexglue-sdk-$version-win-amd64.zip"
$url = "https://github.com/rexglue/rexglue-sdk/releases/download/v$version/$asset"
$zip = Join-Path $toolsBin $asset
$dest = Join-Path $toolsBin "rexglue-sdk-$version"
$exe = Join-Path $dest 'win-amd64\bin\rexglue.exe'

if (Test-Path $exe) {
    Write-Host "ReXGlue already installed: $exe"
} else {
    if (-not (Test-Path $zip)) {
        Write-Host "Downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    }
    Write-Host "Expanding $asset"
    Expand-Archive -LiteralPath $zip -DestinationPath $dest -Force
}

if (-not (Test-Path $exe)) { throw "rexglue.exe not found after install" }
Write-Host "ReXGlue CLI: $exe"
& $exe --version
