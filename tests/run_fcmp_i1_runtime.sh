#!/usr/bin/env bash
set -euo pipefail

cc_bin="${CC:-cc}"
if ! command -v "$cc_bin" >/dev/null 2>&1; then
    echo "Native FCMP/i1 execution conformance skipped (C compiler unavailable)"
    exit 0
fi
tmpdir="$(mktemp -d /tmp/anvil-fcmp-i1.XXXXXX)"
trap 'rm -rf "$tmpdir"' EXIT
"${BUILD_DIR:-build}/tests/fcmp_i1_runtime_codegen" "$tmpdir/anvil_fcmp_i1.s"
CCACHE_DISABLE=1 "$cc_bin" -O2 tests/fcmp_i1_runtime_shim.c \
    "$tmpdir/anvil_fcmp_i1.s" -lm -o "$tmpdir/fcmp_i1"
"$tmpdir/fcmp_i1"
