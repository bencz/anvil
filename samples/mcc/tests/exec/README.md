# MCC Execution Tests

This directory contains execution tests that compare MCC-generated code against native compiler output.

## Running Tests

```bash
# Run all tests
make test-exec

# Run a single test
./tests/exec/run_exec_tests.sh tests/exec/arithmetic.c
```

## Test Categories

### Basic Tests (11 original)
- `return_42.c` - Simple return value
- `hello_putchar.c` - Character output
- `arithmetic.c` - Basic arithmetic operations
- `conditionals.c` - If/else statements
- `loops.c` - For/while loops
- `arrays.c` - 1D array operations
- `pointers.c` - Basic pointer operations
- `structs.c` - Structure access
- `function_call.c` - Function calls
- `global_vars.c` - Global variables
- `sizeof_test.c` - sizeof operator

### Preprocessor Tests
- `preprocessor_basic.c` - #define macros
- `preprocessor_ifdef.c` - #ifdef/#ifndef/#else

### C Standard Tests
- `c89_features.c` - C89/ANSI C features
- `c99_features.c` - C99 features (mixed declarations, for-loop init)

### Advanced Tests
- `bitwise_ops.c` - Bitwise operators (&, |, ^, ~, <<, >>)
- `compound_assign.c` - Compound assignment (+=, -=, etc.)
- `recursion.c` - Recursive functions (factorial, fibonacci, gcd)
- `nested_loops.c` - Triple nested loops
- `string_ops.c` - String operations (strlen, strcmp)
- `struct_nested.c` - Nested structures
- `do_while.c` - Do-while loops

## Known Limitations (disabled/)

The following tests are disabled because they use features not fully implemented in MCC:

| Test | Issue |
|------|-------|
| `switch_case.c` | Switch statement codegen incomplete |
| `ternary_op.c` | Ternary operator codegen incomplete |
| `logical_ops.c` | Logical AND/OR short-circuit evaluation buggy |
| `matrix_ops.c` | 2D array parameter passing not working |
| `matrix_3x3.c` | 2D array parameter passing not working |
| `array_2d.c` | 2D array indexing not working |
| `pointer_arith.c` | Pointer arithmetic with arrays buggy |
| `preprocessor_include.c` | #include with standard headers crashes |

## Test Output

- `output/` - Temporary files during test execution (cleaned up on success)
- Tests compare both stdout output and exit codes

## Adding New Tests

1. Create a `.c` file in this directory
2. Use `putchar()` for output (always available)
3. Include helper functions for printing numbers if needed
4. Test should work with both native compiler and MCC
