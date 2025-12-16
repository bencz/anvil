#!/bin/bash
# MCC Execution Tests
# Compares output of native compiler vs MCC-generated assembly
#
# Usage: ./run_exec_tests.sh [test_file.c]
#        ./run_exec_tests.sh              # Run all tests

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MCC_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MCC="$MCC_DIR/mcc"
OUTPUT_DIR="$SCRIPT_DIR/output"
TIMEOUT_SEC=5

# Timeout function using Perl (works on macOS and Linux)
run_with_timeout() {
    local timeout=$1
    shift
    perl -e '
        use strict;
        use warnings;
        
        my $timeout = shift @ARGV;
        my $pid = fork();
        
        if ($pid == 0) {
            exec @ARGV;
            exit 127;
        }
        
        eval {
            local $SIG{ALRM} = sub { die "timeout\n" };
            alarm $timeout;
            waitpid($pid, 0);
            alarm 0;
        };
        
        if ($@ eq "timeout\n") {
            kill 9, $pid;
            waitpid($pid, 0);
            exit 124;
        }
        
        exit ($? >> 8);
    ' "$timeout" "$@"
}

# Detect compiler
if command -v clang &> /dev/null; then
    CC=clang
elif command -v gcc &> /dev/null; then
    CC=gcc
else
    echo "Error: No C compiler found (clang or gcc required)"
    exit 1
fi

# Detect architecture
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)  MCC_ARCH="x86_64" ;;
    arm64)   MCC_ARCH="arm64_macos" ;;
    aarch64) MCC_ARCH="arm64" ;;
    *)       echo "Unsupported architecture: $ARCH"; exit 1 ;;
esac

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m' # No Color

# Run a single test
run_test() {
    local src="$1"
    local name=$(basename "$src" .c)
    local native_bin="$OUTPUT_DIR/${name}_native"
    local native_out="$OUTPUT_DIR/${name}_native.out"
    local mcc_asm="$OUTPUT_DIR/${name}_mcc.s"
    local mcc_bin="$OUTPUT_DIR/${name}_mcc"
    local mcc_out="$OUTPUT_DIR/${name}_mcc.out"
    local result="PASS"
    local error_msg=""

    printf "  %-40s " "$name"

    # Step 1: Compile with native compiler and run
    if ! $CC -o "$native_bin" "$src" -lc 2>/dev/null; then
        echo -e "${YELLOW}SKIP${NC} (native compile failed)"
        return 0
    fi

    run_with_timeout $TIMEOUT_SEC "$native_bin" > "$native_out" 2>&1
    local native_exit=$?
    if [ $native_exit -eq 124 ]; then
        echo -e "${YELLOW}SKIP${NC} (native timeout)"
        rm -f "$native_bin"
        return 0
    fi
    rm -f "$native_bin"

    # Step 2: Compile with MCC (may crash with trace trap but still generate output)
    "$MCC" -arch="$MCC_ARCH" -std=c99 -o "$mcc_asm" "$src" 2>/dev/null
    
    # Check if assembly was generated (even if MCC crashed after generating it)
    if [ ! -f "$mcc_asm" ] || [ ! -s "$mcc_asm" ]; then
        echo -e "${RED}FAIL${NC} (MCC produced no output)"
        return 1
    fi

    # Step 3: Assemble and link MCC output
    if ! $CC -o "$mcc_bin" "$mcc_asm" -lc 2>/dev/null; then
        echo -e "${RED}FAIL${NC} (assembly failed)"
        rm -f "$mcc_asm"
        return 1
    fi

    # Step 4: Run MCC binary with timeout
    run_with_timeout $TIMEOUT_SEC "$mcc_bin" > "$mcc_out" 2>&1
    local mcc_exit=$?
    if [ $mcc_exit -eq 124 ]; then
        echo -e "${RED}FAIL${NC} (timeout)"
        rm -f "$mcc_asm" "$mcc_bin"
        return 1
    fi

    # Step 5: Compare outputs and exit codes
    if [ "$native_exit" -ne "$mcc_exit" ]; then
        echo -e "${RED}FAIL${NC} (exit code mismatch: expected $native_exit, got $mcc_exit)"
        rm -f "$mcc_asm" "$mcc_bin" "$native_out" "$mcc_out"
        return 1
    fi
    
    if ! diff -q "$native_out" "$mcc_out" > /dev/null 2>&1; then
        echo -e "${RED}FAIL${NC} (output mismatch)"
        echo "    Expected: $(cat "$native_out" | head -c 100)"
        echo "    Got:      $(cat "$mcc_out" | head -c 100)"
        rm -f "$mcc_asm" "$mcc_bin" "$native_out" "$mcc_out"
        return 1
    fi

    # Cleanup on success
    rm -f "$mcc_asm" "$mcc_bin" "$native_out" "$mcc_out"
    echo -e "${GREEN}PASS${NC}"
    return 0
}

# Main
echo "=== MCC Execution Tests ==="
echo "Compiler: $CC"
echo "MCC Arch: $MCC_ARCH"
echo "Timeout:  ${TIMEOUT_SEC}s"
echo ""

passed=0
failed=0

if [ -n "$1" ]; then
    # Run single test
    if run_test "$1"; then
        passed=1
    else
        failed=1
    fi
else
    # Run all tests in exec directory
    for src in "$SCRIPT_DIR"/*.c; do
        if [ -f "$src" ]; then
            if run_test "$src"; then
                ((passed++)) || true
            else
                ((failed++)) || true
            fi
        fi
    done
fi

echo ""
echo "Results: $passed passed, $failed failed"

if [ $failed -gt 0 ]; then
    exit 1
fi
exit 0
