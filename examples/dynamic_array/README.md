# ANVIL Dynamic Array Library Example

This example demonstrates how to use ANVIL to generate code that calls C library
functions like `malloc`, `free`, and `memcpy`.

## Overview

The example consists of:

1. **`generate_dynarray.c`** - An ANVIL program that generates assembly code for array functions
2. **`test_dynarray.c`** - A C program that links with the generated assembly and tests the functions
3. **`Makefile`** - Build automation

## Generated Functions

The library provides these functions:

```c
/* Memory management (calls malloc/free) */
int* array_create(int capacity);      // Allocate array of 'capacity' ints
void array_destroy(int* arr);         // Free array memory
int* array_copy(int* src, int count); // Copy array (malloc + memcpy)

/* Array operations */
int array_sum(int* arr, int count);                    // Sum all elements
int array_max(int* arr, int count);                    // Find maximum
int array_min(int* arr, int count);                    // Find minimum
int array_count_if(int* arr, int n, int threshold);   // Count elements > threshold
void array_scale(int* arr, int n, int factor);        // Multiply all by factor
```

## Key Features Demonstrated

1. **Calling External C Functions**: The generated code calls `malloc`, `free`, and `memcpy`
2. **Pointer Arithmetic**: Using GEP (Get Element Pointer) for array indexing
3. **Control Flow**: Loops with conditions for array traversal
4. **Local Variables**: Stack allocation with `alloca` for loop counters
5. **Type Conversions**: Bitcast between `void*` and `int*`, zero-extension for size_t

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
cd examples/dynamic_array
make
```

### Run Tests

```bash
make test
```

## How It Works

### Declaring External Functions

The generator declares C library functions as external:

```c
/* Declare malloc */
anvil_type_t *malloc_params[] = { size_type };
anvil_value_t *malloc_func = declare_extern_func(ctx, mod, "malloc", 
                                                  ptr_void, malloc_params, 1);
```

### Calling External Functions

Then calls them like any other function:

```c
/* Call malloc(size) */
anvil_value_t *args[] = { size_arg };
anvil_value_t *ptr = anvil_build_call(ctx, malloc_func, args, 1, "ptr");
```

### Generated Assembly (ARM64 macOS example)

```asm
_array_create:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    ; size = capacity * 4
    mov w9, w0
    lsl w9, w9, #2
    ; zero-extend to 64-bit for malloc
    uxtw x0, w9
    ; call malloc
    bl _malloc
    ; return pointer (already in x0)
    ldp x29, x30, [sp], #16
    ret
```

## Supported Architectures

| Architecture | Command | Notes |
|--------------|---------|-------|
| x86-64 Linux | `./generate_dynarray x86_64` | System V ABI |
| ARM64 Linux | `./generate_dynarray arm64` | AAPCS64 |
| ARM64 macOS | `./generate_dynarray arm64_macos` | Darwin ABI |
| PowerPC 64 LE | `./generate_dynarray ppc64le` | ELFv2 ABI |

## Example Output

```
=== ANVIL Dynamic Array Library Test ===
Testing functions that call malloc, free, memcpy

Testing array_create / array_destroy:
  [PASS] array_create returns non-NULL
  arr = [0, 10, 20, 30, 40, 50, 60, 70, 80, 90]
  [PASS] arr[0]: got 0, expected 0
  [PASS] arr[5]: got 50, expected 50
  [PASS] arr[9]: got 90, expected 90
  [INFO] array_destroy completed (no crash = success)

Testing array_sum:
  arr1 = [1, 2, 3, 4, 5]
  [PASS] sum of [1,2,3,4,5]: got 15, expected 15
  ...

=== Test Summary ===
Passed: 45
Failed: 0
Total:  45

All tests passed! The ANVIL-generated library works correctly.
Successfully demonstrated calling malloc, free, and memcpy from ANVIL code.
```

## Extending the Example

To add more C library function calls:

1. Declare the function type and add it as external:
   ```c
   anvil_type_t *qsort_params[] = { ptr_void, size_type, size_type, ptr_cmp_func };
   anvil_value_t *qsort_func = declare_extern_func(ctx, mod, "qsort", 
                                                    void_type, qsort_params, 4);
   ```

2. Build the call with appropriate arguments:
   ```c
   anvil_value_t *args[] = { arr_void, count_ext, elem_size, cmp_func };
   anvil_build_call(ctx, qsort_func, args, 4, NULL);
   ```

## Troubleshooting

### "undefined reference to malloc"
The C library should be linked automatically. If not, add `-lc` to the linker flags.

### Segmentation fault in array operations
Check that:
- Array indices are within bounds
- Pointers are properly cast between `void*` and `int*`
- Size calculations account for `sizeof(int)` (4 bytes)

### Wrong results on 64-bit systems
Make sure to zero-extend 32-bit sizes to 64-bit when calling functions like `malloc`
that expect `size_t` (which is 64-bit on 64-bit systems).
