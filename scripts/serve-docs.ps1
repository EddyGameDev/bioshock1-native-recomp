<#
.SYNOPSIS
Rebuilds the docs database and starts the local docs-intelligence website.

Usage:
  .\scripts\serve-docs.ps1
  .\scripts\serve-docs.ps1 -Port 9000
#>
param([int]$Port = 8765)

$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
Set-Location $project

Write-Host 'Refreshing verification report (web/verify_report.json)...'
python scripts/verify-all.py | Select-Object -Last 3

Write-Host 'Indexing docs into SQLite (web/docs.db)...'
python tools/docs_indexer.py

Write-Host "Starting website at http://127.0.0.1:$Port"
python web/server.py $Port
