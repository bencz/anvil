#!/usr/bin/env bash
set -euo pipefail

mingw="${MINGW_CC:-x86_64-w64-mingw32-gcc}"
wine_bin="${WINE:-wine}"
if ! command -v "$mingw" >/dev/null 2>&1 ||
   ! command -v "$wine_bin" >/dev/null 2>&1; then
    echo "Win64 ABI execution conformance skipped (MinGW/Wine unavailable)"
    exit 0
fi
tmpdir="$(mktemp -d /tmp/anvil-win64-abi.XXXXXX)"
trap 'rm -rf "$tmpdir"' EXIT
"${BUILD_DIR:-build}/tests/win64_abi_codegen" "$tmpdir/anvil_win64.s"
CCACHE_DISABLE=1 "$mingw" -O2 tests/win64_abi_shim.c \
    "$tmpdir/anvil_win64.s" -o "$tmpdir/win64_abi.exe"
WINEDEBUG=-all "$wine_bin" "$tmpdir/win64_abi.exe"
