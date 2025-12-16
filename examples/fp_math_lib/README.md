# ANVIL Floating-Point Math Library Example

This example demonstrates how to use ANVIL to generate a floating-point math library
that can be linked with C code.

## Overview

The example consists of:

1. **`generate_math.c`** - An ANVIL program that generates assembly code for math functions
2. **`test_math.c`** - A C program that links with the generated assembly and tests the functions
3. **`Makefile`** - Build automation for the example

## Generated Functions

The math library provides these functions:

```c
double fp_add(double a, double b);  // Returns a + b
double fp_sub(double a, double b);  // Returns a - b
double fp_mul(double a, double b);  // Returns a * b
double fp_div(double a, double b);  // Returns a / b
double fp_neg(double a);            // Returns -a
double fp_abs(double a);            // Returns |a|
```

## Building

### Prerequisites

- GCC or Clang compiler
- GNU Assembler (as)
- ANVIL library built (`make` in the root directory)

### Quick Build

```bash
# From the anvil root directory, build the library first
make

# Then build this example
cd examples/fp_math_lib
make
```

### Manual Build Steps

```bash
# 1. Build the generator
gcc -Wall -I../../include generate_math.c -o generate_math -L../../lib -lanvil

# 2. Generate assembly for your architecture
./generate_math x86_64 > math_lib.s        # For x86-64 Linux
./generate_math arm64 > math_lib.s         # For ARM64 Linux
./generate_math arm64_macos > math_lib.s   # For ARM64 macOS
./generate_math ppc64le > math_lib.s       # For PowerPC64 LE

# 3. Assemble the generated code
as math_lib.s -o math_lib.o

# 4. Compile the test program
gcc -c test_math.c -o test_math.o

# 5. Link everything together
gcc test_math.o math_lib.o -o test_math -lm

# 6. Run the test
./test_math
```

## Supported Architectures

| Architecture | Command | Notes |
|--------------|---------|-------|
| x86-64 Linux | `./generate_math x86_64` | System V ABI |
| ARM64 Linux | `./generate_math arm64` | AAPCS64 |
| ARM64 macOS | `./generate_math arm64_macos` | Darwin ABI, underscore prefix |
| PowerPC 32 | `./generate_math ppc32` | Big-endian, System V |
| PowerPC 64 BE | `./generate_math ppc64` | Big-endian, ELFv1 |
| PowerPC 64 LE | `./generate_math ppc64le` | Little-endian, ELFv2 |

## Example Output

```
=== ANVIL Floating-Point Math Library Test ===

Test values: a = 10.50, b = 3.50, neg_val = -7.25

Testing fp_add:
  [PASS] fp_add(10.5, 3.5): got 14.000000, expected 14.000000
  [PASS] fp_add(0.0, 0.0): got 0.000000, expected 0.000000
  [PASS] fp_add(-5.0, 5.0): got 0.000000, expected 0.000000
  [PASS] fp_add(1.1, 2.2): got 3.300000, expected 3.300000

Testing fp_sub:
  [PASS] fp_sub(10.5, 3.5): got 7.000000, expected 7.000000
  ...

=== Test Summary ===
Passed: 24
Failed: 0
Total:  24

All tests passed! The ANVIL-generated math library works correctly.
```

## How It Works

1. **Code Generation**: `generate_math.c` uses the ANVIL API to create IR for each
   math function, then generates target-specific assembly code.

2. **Assembly Output**: The generated assembly uses the correct calling convention
   for the target architecture (e.g., XMM registers for x86-64, D registers for ARM64).

3. **Linking**: The C compiler links the test program with the assembled math library,
   allowing C code to call the ANVIL-generated functions.

## Extending the Example

To add more functions:

1. Add a new function creation in `generate_math.c`:
   ```c
   /* Create fp_sqrt: double fp_sqrt(double a) { return sqrt(a); } */
   // Note: ANVIL doesn't have built-in sqrt, but you could implement
   // it using Newton-Raphson iteration or call an external function
   ```

2. Declare the function in `test_math.c`:
   ```c
   extern double fp_sqrt(double a);
   ```

3. Add tests for the new function.

## Troubleshooting

### "undefined reference to fp_add"
Make sure the assembly was generated for the correct architecture and properly assembled.

### Segmentation fault
Check that the calling convention matches between the generated assembly and the C code.
Use `objdump -d math_lib.o` to inspect the generated code.

### Wrong results
Verify the floating-point format (IEEE 754 vs HFP for mainframes) matches your platform.
