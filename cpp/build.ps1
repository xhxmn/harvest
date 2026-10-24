# Build harvest.exe and the C++ test runner with MinGW g++.
#   .\cpp\build.ps1
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    Write-Host 'building cpp/harvest.exe ...'
    g++ -std=c++17 -O2 -Wall -Wextra -o cpp/harvest.exe cpp/harvest.cpp
    if ($LASTEXITCODE -ne 0) { throw 'build failed' }

    Write-Host 'building cpp/test_harvest.exe ...'
    g++ -std=c++17 -O2 -Wall -Wextra -o cpp/test_harvest.exe cpp/test_harvest.cpp
    if ($LASTEXITCODE -ne 0) { throw 'test build failed' }

    Write-Host 'done: cpp/harvest.exe'
} finally {
    Pop-Location
}
