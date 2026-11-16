# Build, flash and monitor the e-reader firmware.
# Usage:
#   .\tools\build.ps1                # build
#   .\tools\build.ps1 -Clean         # fullclean + build
#   .\tools\build.ps1 -Flash         # build + flash (auto-detect COM port)
#   .\tools\build.ps1 -Flash -Port COM5 -Monitor
param(
    [switch]$Clean,
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port = "",
    [string]$IdfVersion = "5.5.1"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root "tools\idf_env.ps1") -Version $IdfVersion
Push-Location (Join-Path $root "firmware")
try {
    if ($Clean) { idf.py fullclean }
    idf.py build
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
    if ($Flash -or $Monitor) {
        if ([string]::IsNullOrWhiteSpace($Port)) {
            $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
            if ($ports.Count -eq 1) {
                $Port = $ports[0]
            } elseif ($ports.Count -eq 0) {
                throw "No serial port found. Connect the device over USB."
            } else {
                throw "Several serial ports found ($($ports -join ', ')). Pass -Port COMx."
            }
        }
        Write-Host "Using port $Port" -ForegroundColor Cyan
        if ($Monitor) {
            idf.py -p $Port flash monitor
        } else {
            idf.py -p $Port flash
        }
    }
}
finally {
    Pop-Location
}
