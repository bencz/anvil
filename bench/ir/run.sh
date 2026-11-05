#!/usr/bin/env bash
# Benchmark kernels built with the ANVIL API against clang compiling the
# same algorithms in C.  Each kernel and its reference are linked with the
# same driver (compiled by the reference compiler), so only the kernel code
# differs; neither is inlined into the driver.  The references build with
# -fno-builtin: a loop the compiler would replace by a C library call
# (strlen, memset) measures that library, not code generation.
#
# x86_64 and x86 run on the host and report milliseconds.  arm64, ppc64,
# ppc64le and ppc32 run under qemu-user (freestanding, qemu/) on reduced
# workloads (BENCH_SMALL) and report dynamic instruction counts.  Without
# --cpu, ANVIL targets the CPU the reference compiler assumes by default.
#
# --asm=DIR keeps both compilers' assembly of each kernel, side by side
# (DIR/<arch>/<kernel>.clang.s and .anvil.s), for reading the differences.
#
# Usage: bench/ir/run.sh [--arch=x86_64|x86|arm64|ppc64|ppc64le|ppc32] [--cpu=NAME] [--opt=-O2] [--runs=3] [--cc=clang] [--asm=DIR] [kernel ...]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="$ROOT/bench/ir"
ARCH=x86_64; CPU=""; OPT=-O2; RUNS=3; CC=clang; ASM_DIR=""; KERNELS=()
for arg in "$@"; do
    case "$arg" in
        --arch=*) ARCH="${arg#--arch=}" ;;
        --cpu=*) CPU="${arg#--cpu=}" ;;
        --opt=*) OPT="${arg#--opt=}" ;;
        --runs=*) RUNS="${arg#--runs=}" ;;
        --cc=*) CC="${arg#--cc=}" ;;
        --asm=*) ASM_DIR="${arg#--asm=}" ;;
        -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
        *) KERNELS+=("$arg") ;;
    esac
done
MODE=time; QEMU=""; LD=""; DEFAULT_CPU=""
case "$ARCH" in
    x86_64) TARGET_CFLAGS="" ;;
    x86) TARGET_CFLAGS="-m32"; DEFAULT_CPU=pentium4 ;;  # clang -m32 targets pentium4 (SSE2)
    arm64) MODE=count; TARGET_CFLAGS="--target=aarch64-linux-gnu"; QEMU=qemu-aarch64-static; LD=ld.lld ;;
    ppc64) MODE=count; TARGET_CFLAGS="--target=powerpc64-linux-gnu -mabi=elfv1"; QEMU=qemu-ppc64-static; LD=powerpc64-linux-gnu-ld ;;
    ppc64le) MODE=count; TARGET_CFLAGS="--target=powerpc64le-linux-gnu"; QEMU=qemu-ppc64le-static; LD=ld.lld ;;
    ppc32) MODE=count; TARGET_CFLAGS="--target=powerpc-linux-gnu"; QEMU=qemu-ppc-static; LD=ld.lld ;;
    *) echo "unknown --arch=$ARCH" >&2; exit 2 ;;
esac
[ ${#KERNELS[@]} -eq 0 ] && KERNELS=(fib crc32 sieve matmul strings qsort lexer)

make -s -C "$ROOT" bench-ir >/dev/null
GEN="$ROOT/build/bench_ir/anvil-bench-gen"
WORK="$(mktemp -d /tmp/anvil-bench-ir.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

best_ms() {
    local best=""
    for _ in $(seq "$RUNS"); do
        local start end ms
        start=$(date +%s%N); "$1" > /dev/null; end=$(date +%s%N)
        ms=$(( (end - start) / 1000000 ))
        if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then best=$ms; fi
    done
    echo "$best"
}

# Instructions the program executes under qemu (one instruction per
# translation block, each block logged once per execution).
insns() {
    "$QEMU" -one-insn-per-tb -d exec,nochain -D /dev/stdout "$1" 2>/dev/null | grep -c '^Trace' || true
}

# Links driver + kernel objects into $1 (hosted, or freestanding under qemu).
link() {
    local out=$1; shift
    if [ "$MODE" = time ]; then
        "$CC" $TARGET_CFLAGS "$@" -lm -o "$out"
    else
        "$LD" -static -e _start -o "$out" "$WORK/start.o" "$@" "$WORK/printf.o" "$WORK/rt.o" "$WORK/mem.o"
    fi
}

DRIVER_FLAGS=(-O2 -w)
REF_FLAGS=("$OPT" -fno-builtin -w)
if [ "$MODE" = count ]; then
    FREESTANDING=(-ffreestanding -fno-builtin -fno-stack-protector)
    for f in printf rt mem; do "$CC" $TARGET_CFLAGS -O1 "${FREESTANDING[@]}" -c "$DIR/qemu/$f.c" -o "$WORK/$f.o"; done
    "$CC" $TARGET_CFLAGS -c "$DIR/qemu/start_$ARCH.s" -o "$WORK/start.o"
    DRIVER_FLAGS+=("${FREESTANDING[@]}" -DBENCH_SMALL)
    REF_FLAGS+=(-ffreestanding)
fi

GEN_FLAGS=(--arch="$ARCH" "$OPT")
CPU="${CPU:-$DEFAULT_CPU}"
[ -n "$CPU" ] && GEN_FLAGS+=(--cpu="$CPU")
if [ "$MODE" = time ]; then
    printf "%-8s %10s %10s %7s\n" kernel "$CC(ms)" "anvil(ms)" ratio
else
    printf "%-8s %12s %12s %7s\n" kernel "$CC(insns)" "anvil(insns)" ratio
fi
status=0
for k in "${KERNELS[@]}"; do
    "$CC" $TARGET_CFLAGS "${DRIVER_FLAGS[@]}" -c "$DIR/driver/$k.c" -o "$WORK/$k.driver.o"
    "$CC" $TARGET_CFLAGS "${REF_FLAGS[@]}" -c "$DIR/ref/$k.c" -o "$WORK/$k.ref.o"
    link "$WORK/$k.ref" "$WORK/$k.driver.o" "$WORK/$k.ref.o"
    "$GEN" "$k" "$WORK/$k.s" "${GEN_FLAGS[@]}"
    "$CC" $TARGET_CFLAGS -c "$WORK/$k.s" -o "$WORK/$k.anvil.o"
    link "$WORK/$k.anvil" "$WORK/$k.driver.o" "$WORK/$k.anvil.o"
    if [ -n "$ASM_DIR" ]; then
        mkdir -p "$ASM_DIR/$ARCH"
        "$CC" $TARGET_CFLAGS "${REF_FLAGS[@]}" -S -o "$ASM_DIR/$ARCH/$k.clang.s" "$DIR/ref/$k.c"
        cp "$WORK/$k.s" "$ASM_DIR/$ARCH/$k.anvil.s"
    fi
    if [ "$MODE" = time ]; then
        expected=$("$WORK/$k.ref"); got=$("$WORK/$k.anvil")
    else
        expected=$("$QEMU" "$WORK/$k.ref"); got=$("$QEMU" "$WORK/$k.anvil")
    fi
    if [ "$expected" != "$got" ]; then
        echo "$k: OUTPUT DIFFERS: '$got' (expected '$expected')"
        status=1
        continue
    fi
    if [ "$MODE" = time ]; then
        ref=$(best_ms "$WORK/$k.ref"); anvil=$(best_ms "$WORK/$k.anvil")
        printf "%-8s %10s %10s %7s\n" "$k" "$ref" "$anvil" "$(awk -v a="$anvil" -v r="$ref" 'BEGIN { printf "%.2fx", r ? a / r : 0 }')"
    else
        ref=$(insns "$WORK/$k.ref"); anvil=$(insns "$WORK/$k.anvil")
        printf "%-8s %12s %12s %7s\n" "$k" "$ref" "$anvil" "$(awk -v a="$anvil" -v r="$ref" 'BEGIN { printf "%.2fx", r ? a / r : 0 }')"
    fi
done
exit $status
