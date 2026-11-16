#!/usr/bin/env bash
# Builds and runs the host parser/layout tests (no ESP-IDF required).
# Mirrors tests/run_tests.ps1 so the same suite can run on Linux and in CI.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

CXX="${CXX:-g++}"
mkdir -p tests/build

common=(-std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -I firmware/main)
core=(firmware/main/core/utf8.cpp firmware/main/core/encoding.cpp firmware/main/core/byte_source.cpp)
atlas=(firmware/main/text/atlas/atlas_catalog.cpp
       firmware/main/text/atlas/atlas_14.cpp
       firmware/main/text/atlas/atlas_18.cpp
       firmware/main/text/atlas/atlas_24.cpp
       firmware/main/text/atlas/atlas_28.cpp
       firmware/main/text/atlas/atlas_32.cpp)
miniz=(tests/support/miniz/miniz.c)

failed=0

run_target() {
    local name="$1"; shift
    local -a srcs=("$@")
    local exe="tests/build/${name}"
    local -a args=("${common[@]}" -o "$exe" "${core[@]}")

    for src in "${srcs[@]}"; do
        case "$src" in
            *formats/epub.cpp|*formats/pdf.cpp) args+=(-I tests/support/miniz "${miniz[@]}") ;;
        esac
    done
    args+=("${srcs[@]}")

    echo "== build ${name}"
    if ! "$CXX" "${args[@]}"; then
        echo "BUILD FAILED: ${name}"
        failed=$((failed + 1))
        return
    fi
    echo "== run ${name}"
    if ! "$exe"; then
        echo "TEST FAILED: ${name}"
        failed=$((failed + 1))
    fi
}

run_target test_txt   firmware/main/formats/txt.cpp tests/test_txt.cpp
run_target test_md    firmware/main/formats/markdown.cpp tests/test_md.cpp
run_target test_fb2   firmware/main/formats/fb2.cpp tests/test_fb2.cpp
run_target test_epub  firmware/main/formats/epub.cpp tests/test_epub.cpp
run_target test_pdf   firmware/main/formats/pdf.cpp tests/test_pdf.cpp
run_target test_links firmware/main/formats/fb2.cpp firmware/main/formats/txt.cpp \
                      firmware/main/formats/markdown.cpp firmware/main/formats/epub.cpp \
                      firmware/main/formats/pdf.cpp firmware/main/formats/formats.cpp \
                      firmware/main/text/font.cpp firmware/main/text/layout.cpp \
                      firmware/main/text/hyphenate.cpp "${atlas[@]}" tests/test_links.cpp
run_target test_layout firmware/main/text/font.cpp firmware/main/text/layout.cpp \
                       firmware/main/text/hyphenate.cpp "${atlas[@]}" tests/test_layout.cpp

if [ "$failed" -gt 0 ]; then
    echo "$failed target(s) failed"
    exit 1
fi
echo "All host tests passed"
