
$ErrorActionPreference = "Stop"
  
$compiler = "C:\msys64\mingw64\bin\g++.exe"
$outputDir = Join-Path $PSScriptRoot "dist"
$outputExe = Join-Path $outputDir "LoopyMacro.exe"

if (-not (Test-Path $compiler)) {
    throw "MinGW g++ was not found at $compiler"
} 
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

& $compiler `
    -std=c++17 `
    -O2 `
    -static `
    -static-libgcc `
    -static-libstdc++ `
    -municode `
    -mwindows `
    "$PSScriptRoot\main.cpp" `
    -o $outputExe `
    -lcomctl32 `
    -lshell32 `
    -luser32 `
    -lgdi32 `
    -lwinmm

Write-Host "Built: $outputExe"
