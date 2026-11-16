# Builds and runs the host parser tests with the bundled MinGW g++.
param(
    [string]$Filter = "*"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    $gxx = "C:\Users\Blaq\scoop\apps\mingw\current\bin\g++.exe"
    if (-not (Test-Path $gxx)) { throw "MinGW g++ not found: $gxx" }
    New-Item -ItemType Directory -Force -Path "tests/build" | Out-Null

    $common = @(
        "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-static", "-static-libgcc", "-static-libstdc++", "-I", "firmware/main"
    )
    $core = @(
        "firmware/main/core/utf8.cpp",
        "firmware/main/core/encoding.cpp",
        "firmware/main/core/byte_source.cpp"
    )
    $miniz = "tests/support/miniz/miniz.c"

    $targets = @(
        @{ Name = "test_txt"; Src = @("firmware/main/formats/txt.cpp", "tests/test_txt.cpp"); Extra = @() },
        @{ Name = "test_md"; Src = @("firmware/main/formats/markdown.cpp", "tests/test_md.cpp"); Extra = @() },
        @{ Name = "test_fb2"; Src = @("firmware/main/formats/fb2.cpp", "tests/test_fb2.cpp"); Extra = @() },
        @{ Name = "test_epub"; Src = @("firmware/main/formats/epub.cpp", "tests/test_epub.cpp"); Extra = @("-I", "tests/support/miniz") },
        @{ Name = "test_pdf"; Src = @("firmware/main/formats/pdf.cpp", "tests/test_pdf.cpp"); Extra = @("-I", "tests/support/miniz") },
        @{ Name = "test_links"; Src = @("firmware/main/formats/fb2.cpp", "firmware/main/formats/txt.cpp", "firmware/main/formats/markdown.cpp", "firmware/main/formats/epub.cpp", "firmware/main/formats/pdf.cpp", "firmware/main/formats/formats.cpp", "firmware/main/text/font.cpp", "firmware/main/text/layout.cpp", "firmware/main/text/hyphenate.cpp", "firmware/main/text/atlas/atlas_catalog.cpp", "firmware/main/text/atlas/atlas_14.cpp", "firmware/main/text/atlas/atlas_18.cpp", "firmware/main/text/atlas/atlas_24.cpp", "firmware/main/text/atlas/atlas_28.cpp", "firmware/main/text/atlas/atlas_32.cpp", "tests/test_links.cpp"); Extra = @("-I", "tests/support/miniz") },
        @{ Name = "test_layout"; Src = @("firmware/main/text/font.cpp", "firmware/main/text/layout.cpp", "firmware/main/text/hyphenate.cpp", "firmware/main/text/atlas/atlas_catalog.cpp", "firmware/main/text/atlas/atlas_14.cpp", "firmware/main/text/atlas/atlas_18.cpp", "firmware/main/text/atlas/atlas_24.cpp", "firmware/main/text/atlas/atlas_28.cpp", "firmware/main/text/atlas/atlas_32.cpp", "tests/test_layout.cpp"); Extra = @() }
    )

    $failed = 0
    foreach ($target in $targets) {
        if ($target.Name -notlike $Filter) { continue }
        $exe = "tests/build/$($target.Name).exe"
        $args = @() + $common + @("-o", $exe) + $core
        if ($target.Src -contains "firmware/main/formats/epub.cpp" -or $target.Src -contains "firmware/main/formats/pdf.cpp") {
            $args += $miniz
        }
        $args += $target.Extra + $target.Src
        Write-Host "== build $($target.Name)" -ForegroundColor Cyan
        & $gxx @args
        if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED: $($target.Name)" -ForegroundColor Red; $failed++; continue }
        Write-Host "== run $($target.Name)" -ForegroundColor Cyan
        & $exe
        if ($LASTEXITCODE -ne 0) { Write-Host "TEST FAILED: $($target.Name)" -ForegroundColor Red; $failed++ }
    }
    if ($failed -gt 0) { Write-Host "$failed target(s) failed" -ForegroundColor Red; exit 1 }
    Write-Host "All host tests passed" -ForegroundColor Green
}
finally {
    Pop-Location
}
