#!/bin/bash
# MCC Execution Tests
# Compares output of native compiler vs MCC-generated assembly
#
# Usage: ./run_exec_tests.sh [options] [test_file.c]
#        ./run_exec_tests.sh                    # Run all tests with -O0
#        ./run_exec_tests.sh -O2                # Run all tests with -O2
#        ./run_exec_tests.sh -Og test.c         # Run single test with -Og
#        ./run_exec_tests.sh --all-opts         # Run all tests with all optimization levels
#        ./run_exec_tests.sh --compare-clang    # Compare MCC output size with Clang

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MCC_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MCC="$MCC_DIR/mcc"
OUTPUT_DIR="$SCRIPT_DIR/output"
TIMEOUT_SEC=5
OPT_LEVEL=""
ALL_OPTS=false
COMPARE_CLANG=false

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

# Parse arguments
TEST_FILE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        -O0|-O1|-O2|-O3|-Og)
            OPT_LEVEL="$1"
            shift
            ;;
        --all-opts)
            ALL_OPTS=true
            shift
            ;;
        --compare-clang)
            COMPARE_CLANG=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options] [test_file.c]"
            echo ""
            echo "Options:"
            echo "  -O0, -O1, -O2, -O3, -Og  Set optimization level (default: none)"
            echo "  --all-opts               Run tests with all optimization levels"
            echo "  --compare-clang          Compare assembly size with Clang"
            echo "  -h, --help               Show this help"
            exit 0
            ;;
        *.c)
            TEST_FILE="$1"
            shift
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

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
    local opt="$2"  # Optional optimization level
    local name=$(basename "$src" .c)
    local opt_suffix=""
    [ -n "$opt" ] && opt_suffix="_${opt#-}"
    local native_bin="$OUTPUT_DIR/${name}_native"
    local native_out="$OUTPUT_DIR/${name}_native.out"
    local mcc_asm="$OUTPUT_DIR/${name}_mcc${opt_suffix}.s"
    local mcc_bin="$OUTPUT_DIR/${name}_mcc${opt_suffix}"
    local mcc_out="$OUTPUT_DIR/${name}_mcc${opt_suffix}.out"
    local result="PASS"
    local error_msg=""

    if [ -n "$opt" ]; then
        printf "  %-35s %-5s " "$name" "$opt"
    else
        printf "  %-40s " "$name"
    fi

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

    # Detect C standard from filename or default to c99
    local std="c99"
    if [[ "$name" == *"_c89"* ]]; then
        std="c89"
    elif [[ "$name" == *"_c99"* ]]; then
        std="c99"
    elif [[ "$name" == *"_c11"* ]]; then
        std="c11"
    fi

    # Step 2: Compile with MCC (may crash with trace trap but still generate output)
    local mcc_opts="-arch=$MCC_ARCH -std=$std -I$MCC_DIR/includes"
    [ -n "$opt" ] && mcc_opts="$mcc_opts $opt"
    "$MCC" $mcc_opts -o "$mcc_asm" "$src" 2>/dev/null
    
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

# Run all tests and return counts
run_all_tests() {
    local opt="$1"
    local test_file="$2"
    
    if [ -n "$test_file" ]; then
        # Run single test
        if run_test "$test_file" "$opt"; then
            ((passed++)) || true
        else
            ((failed++)) || true
        fi
    else
        # Run all tests in exec directory
        for src in "$SCRIPT_DIR"/*.c; do
            if [ -f "$src" ]; then
                if run_test "$src" "$opt"; then
                    ((passed++)) || true
                else
                    ((failed++)) || true
                fi
            fi
        done
    fi
}

# Compare assembly sizes with Clang
compare_with_clang() {
    local src="$1"
    local name=$(basename "$src" .c)
    
    # Detect C standard from filename or default to c99
    local std="c99"
    if [[ "$name" == *"_c89"* ]]; then
        std="c89"
    elif [[ "$name" == *"_c99"* ]]; then
        std="c99"
    elif [[ "$name" == *"_c11"* ]]; then
        std="c11"
    fi
    
    echo ""
    echo "=== Assembly Size Comparison: $name ==="
    printf "%-10s %10s %10s\n" "Level" "MCC" "Clang"
    printf "%-10s %10s %10s\n" "-----" "---" "-----"
    
    for opt in "-O0" "-Og" "-O1" "-O2" "-O3"; do
        local mcc_asm="$OUTPUT_DIR/${name}_mcc_${opt}.s"
        local clang_asm="$OUTPUT_DIR/${name}_clang_${opt}.s"
        
        # Compile with MCC (suppress crash messages)
        local mcc_opts="-arch=$MCC_ARCH -std=$std -I$MCC_DIR/includes $opt"
        if { "$MCC" $mcc_opts -o "$mcc_asm" "$src" >/dev/null 2>&1; } 2>/dev/null; then
            # Check if file was generated
            if [ ! -f "$mcc_asm" ] || [ ! -s "$mcc_asm" ]; then
                local mcc_lines="-"
            else
                local mcc_lines=$(grep -v '^\s*[;#@]' "$mcc_asm" 2>/dev/null | grep -v '^\s*$' | grep -v '^\s*\.' | wc -l | tr -d ' ')
            fi
        else
            local mcc_lines="-"
        fi
        
        # Compile with Clang
        if $CC -S $opt -o "$clang_asm" "$src" 2>/dev/null; then
            local clang_lines=$(grep -v '^\s*[;#@]' "$clang_asm" 2>/dev/null | grep -v '^\s*$' | grep -v '^\s*\.' | wc -l | tr -d ' ')
        else
            local clang_lines="-"
        fi
        
        printf "%-10s %10s %10s\n" "$opt" "$mcc_lines" "$clang_lines"
        
        rm -f "$mcc_asm" "$clang_asm" 2>/dev/null
    done
}

# Main
echo "=== MCC Execution Tests ==="
echo "Compiler: $CC"
echo "MCC Arch: $MCC_ARCH"
echo "Timeout:  ${TIMEOUT_SEC}s"
[ -n "$OPT_LEVEL" ] && echo "Opt Level: $OPT_LEVEL"
echo ""

passed=0
failed=0

if $COMPARE_CLANG; then
    # Compare with Clang
    if [ -n "$TEST_FILE" ]; then
        compare_with_clang "$TEST_FILE"
    else
        for src in "$SCRIPT_DIR"/*.c; do
            if [ -f "$src" ]; then
                compare_with_clang "$src"
            fi
        done
    fi
elif $ALL_OPTS; then
    # Run with all optimization levels
    for opt in "" "-Og" "-O1" "-O2" "-O3"; do
        opt_name="${opt:-O0}"
        echo "--- Optimization: $opt_name ---"
        run_all_tests "$opt" "$TEST_FILE"
        echo ""
    done
else
    # Run with specified optimization level (or none)
    run_all_tests "$OPT_LEVEL" "$TEST_FILE"
fi

echo ""
echo "Results: $passed passed, $failed failed"

if [ $failed -gt 0 ]; then
    exit 1
fi
exit 0
