# Activates an ESP-IDF installation for the current PowerShell session.
# Usage: . .\tools\idf_env.ps1 [-Version 5.5.1]
param(
    [string]$Version = "5.5.1"
)

$ErrorActionPreference = "Stop"
$toolsRoot = "C:\Espressif\tools"

switch ($Version) {
    "5.5.1" {
        $idfPath = "C:\esp\5.5.1\esp-idf"
        $pythonEnv = Join-Path $toolsRoot "python\v5.5\venv"
        $toolchain = Join-Path $toolsRoot "xtensa-esp-elf\esp-14.2.0_20241119"
    }
    "6.1" {
        $idfPath = "C:\esp\v6.1\esp-idf"
        $pythonEnv = Join-Path $toolsRoot "python\v6.1\venv"
        $toolchain = Join-Path $toolsRoot "xtensa-esp-elf\esp-15.2.0_20251204"
    }
    default { throw "Unknown ESP-IDF version: $Version" }
}

if (-not (Test-Path $idfPath)) { throw "ESP-IDF not found: $idfPath" }
if (-not (Test-Path $pythonEnv)) { throw "Python env not found: $pythonEnv" }

$cmake = (Get-ChildItem (Join-Path $toolsRoot "cmake") -Directory | Select-Object -Last 1).FullName
$ninja = (Get-ChildItem (Join-Path $toolsRoot "ninja") -Directory | Select-Object -Last 1).FullName
$idfExe = (Get-ChildItem (Join-Path $toolsRoot "idf-exe") -Directory | Select-Object -Last 1).FullName
$ccache = (Get-ChildItem (Join-Path $toolsRoot "ccache") -Directory | Select-Object -Last 1).FullName
$romElfs = (Get-ChildItem (Join-Path $toolsRoot "esp-rom-elfs") -Directory | Select-Object -Last 1).FullName

$env:IDF_PATH = $idfPath
$env:IDF_TOOLS_PATH = $toolsRoot
$env:IDF_PYTHON_ENV_PATH = $pythonEnv
$env:ESP_ROM_ELF_DIR = "$romElfs/"
$env:IDF_CCACHE_ENABLE = "1"
$env:PYTHONUTF8 = "1"

$candidates = @(
    (Join-Path $toolchain "bin"),
    (Join-Path $toolchain "xtensa-esp-elf\bin"),
    (Join-Path $cmake "bin"),
    $ninja,
    (Join-Path $pythonEnv "Scripts"),
    $idfExe
)
if (Test-Path $ccache) {
    $ccacheDir = Get-ChildItem $ccache -Directory | Select-Object -Last 1
    if ($ccacheDir) { $candidates += $ccacheDir.FullName }
}

$paths = $candidates | Where-Object { Test-Path $_ }
$env:PATH = ($paths -join ";") + ";" + $env:PATH
Write-Host "ESP-IDF $Version activated: $idfPath"
