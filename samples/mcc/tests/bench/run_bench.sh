#!/usr/bin/env bash
# MCC vs a reference C compiler on small kernels: same output, best-of-N
# wall time and .text size.
#
# Usage: ./run_bench.sh [--cc=clang] [--opt=-O2] [--runs=3] [--mcc-flags=-std=c99] [kernel.c ...]
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MCC_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MCC="$MCC_DIR/mcc"
CC=clang
OPT=-O2
RUNS=3
MCC_FLAGS="-std=c99"
KERNELS=()

for arg in "$@"; do
    case "$arg" in
        --cc=*) CC="${arg#--cc=}" ;;
        --opt=*) OPT="${arg#--opt=}" ;;
        --runs=*) RUNS="${arg#--runs=}" ;;
        --mcc-flags=*) MCC_FLAGS="${arg#--mcc-flags=}" ;;
        -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
        *) KERNELS+=("$arg") ;;
    esac
done
if [ ${#KERNELS[@]} -eq 0 ]; then
    KERNELS=("$SCRIPT_DIR"/*.c)
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

best_ms() {
    local best=""
    for _ in $(seq "$RUNS"); do
        local start end ms
        start=$(date +%s%N)
        "$1" > /dev/null
        end=$(date +%s%N)
        ms=$(( (end - start) / 1000000 ))
        if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then best=$ms; fi
    done
    echo "$best"
}

text_size() {
    size -A "$1" 2>/dev/null | awk '$1 == ".text" { print $2 }'
}

printf "%-10s %10s %10s %8s %10s %10s\n" kernel "$CC(ms)" "mcc(ms)" ratio "$CC .text" "mcc .text"
status=0
for src in "${KERNELS[@]}"; do
    name="$(basename "$src" .c)"
    ref="$WORK/$name.ref"
    out="$WORK/$name.mcc"
    if ! "$CC" $OPT -o "$ref" "$src" -lm; then
        echo "$name: $CC failed"; status=1; continue
    fi
    if ! "$MCC" $OPT -I"$MCC_DIR/includes" $MCC_FLAGS -o "$WORK/$name.s" "$src" > "$WORK/$name.log" 2>&1 \
        || ! "$CC" -o "$out" "$WORK/$name.s" -lm; then
        echo "$name: mcc failed"; sed -n '1,5p' "$WORK/$name.log"; status=1; continue
    fi
    if [ "$("$ref")" != "$("$out")" ]; then
        echo "$name: OUTPUT MISMATCH"; "$ref"; "$out"; status=1; continue
    fi
    ref_ms=$(best_ms "$ref")
    mcc_ms=$(best_ms "$out")
    ratio=$(awk -v a="$mcc_ms" -v b="$ref_ms" 'BEGIN { printf "%.2f", (b > 0) ? a / b : 0 }')
    printf "%-10s %10s %10s %8s %10s %10s\n" "$name" "$ref_ms" "$mcc_ms" "${ratio}x" "$(text_size "$ref")" "$(text_size "$out")"
done
exit $status
