#!/usr/bin/env bash
# Aggregate ABI execution conformance: ANVIL code from
# tests/aggregate_abi_codegen.c calls and is called by
# tests/aggregate_abi_shim.c built with the platform compiler.  x86_64 runs
# natively, i386 through cc -m32, other targets through clang, ld.lld and
# qemu-user as freestanding Linux programs.  A target is skipped when its
# toolchain or emulator is missing; any failure fails the run.
#
# Usage: run_aggregate_abi.sh <build-dir>
set -uo pipefail

gen="${1:-build}/tests/aggregate_abi_codegen"
cc=cc
clang=clang
lld=ld.lld
tmpdir="$(mktemp -d /tmp/anvil-aggregate-abi.XXXXXX)"
trap 'rm -rf "$tmpdir"' EXIT
failed=0

have() { command -v "$1" >/dev/null 2>&1; }

qemu_for() {
    local name
    for name in "qemu-$1" "qemu-$1-static"; do
        if have "$name"; then
            echo "$name"
            return 0
        fi
    done
    return 1
}

generate() {    # target
    "$gen" "$1" "$tmpdir/$1.s" >"$tmpdir/$1.gen.log" 2>&1
    local status=$?
    if [ "$status" -eq 2 ]; then
        echo "aggregate ABI $1: skipped (target does not lower BYVAL/SRET)"
    elif [ "$status" -ne 0 ]; then
        echo "aggregate ABI $1: FAILED (generator)"
        cat "$tmpdir/$1.gen.log"
        failed=1
    fi
    return "$status"
}

report() {      # target status
    if [ "$2" -eq 0 ]; then
        echo "aggregate ABI $1: passed"
    else
        echo "aggregate ABI $1: FAILED (check $2)"
        failed=1
    fi
}

native() {      # target cc-flags...
    local target="$1"
    shift
    if ! echo 'int main(void){return 0;}' | "$cc" "$@" -x c - -o "$tmpdir/probe" >/dev/null 2>&1; then
        echo "aggregate ABI $target: skipped (no $cc $* toolchain)"
        return
    fi
    generate "$target" || return
    if ! "$cc" "$@" "$tmpdir/$target.s" tests/aggregate_abi_shim.c -o "$tmpdir/$target"; then
        echo "aggregate ABI $target: FAILED (link)"
        failed=1
        return
    fi
    "$tmpdir/$target"
    report "$target" $?
}

emulated() {    # target triple qemu-name linker clang-flags...
    local target="$1" triple="$2" qemu linker="$4"
    if ! qemu="$(qemu_for "$3")"; then
        echo "aggregate ABI $target: skipped (qemu-$3 unavailable)"
        return
    fi
    shift 4
    if ! have "$clang" || ! have "$linker"; then
        echo "aggregate ABI $target: skipped ($clang/$linker unavailable)"
        return
    fi
    generate "$target" || return
    local asflags=(--target="$triple" "$@")
    local flags=("${asflags[@]}" -O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-pic)
    if ! "$clang" "${asflags[@]}" -c "$tmpdir/$target.s" -o "$tmpdir/$target.anvil.o" ||
       ! "$clang" "${flags[@]}" -DAGGREGATE_ABI_FREESTANDING -c tests/aggregate_abi_shim.c -o "$tmpdir/$target.shim.o" ||
       ! "$clang" "${flags[@]}" -c tests/platform/freestanding_start.c -o "$tmpdir/$target.start.o" ||
       ! "$linker" -static -e _start "$tmpdir/$target.start.o" "$tmpdir/$target.shim.o" \
            "$tmpdir/$target.anvil.o" -o "$tmpdir/$target"; then
        echo "aggregate ABI $target: FAILED (build)"
        failed=1
        return
    fi
    "$qemu" "$tmpdir/$target"
    report "$target" $?
}

if [ "$(uname -m)" = x86_64 ]; then
    native x86_64
    native i386 -m32
fi
emulated aarch64 aarch64-linux-gnu aarch64 "$lld"
emulated ppc32 powerpc-linux-gnu ppc "$lld"
# ld.lld does not link the ELFv1 ABI.
emulated ppc64 powerpc64-linux-gnu ppc64 powerpc64-linux-gnu-ld -mabi=elfv1
emulated ppc64le powerpc64le-linux-gnu ppc64le "$lld" -mabi=elfv2
exit "$failed"
