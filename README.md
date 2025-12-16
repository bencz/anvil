# ANVIL

A C library for compiler code generation with support for multiple architectures through a portable intermediate representation (IR) system.

## Features

* Portable IR: Architecture-independent intermediate representation
* Multiple Backends: Support for x86, x86-64, S/370, S/370-XA, S/390, z/Architecture, PowerPC 32/64-bit, ARM64
* Assembly Output: Generates assembly text (HLASM for mainframes, GAS for x86/PPC)
* **IR Optimization**: Configurable optimization passes (constant folding, DCE, strength reduction)
* Extensible: Plugin architecture for adding new backends
* Opcode Ready: Design prepared for future binary code generation

## Supported Architectures

| Architecture | Bits | Endianness | Stack | FP Format | ABI | Syntax |
|--------------|------|------------|-------|-----------|-----|--------|
| x86 | 32 | Little | Down | IEEE 754 | System V | GAS/NASM |
| x86-64 | 64 | Little | Down | IEEE 754 | System V | GAS/NASM |
| S/370 | 24 | Big | Up | HFP | MVS | HLASM |
| S/370-XA | 31 | Big | Up | HFP | MVS | HLASM |
| S/390 | 31 | Big | Up | HFP | MVS | HLASM |
| z/Architecture | 64 | Big | Up | HFP+IEEE | MVS | HLASM |
| PowerPC 32 | 32 | Big | Down | IEEE 754 | System V | GAS |
| PowerPC 64 | 64 | Big | Down | IEEE 754 | System V | GAS |
| PowerPC 64 LE | 64 | Little | Down | IEEE 754 | System V | GAS |
| ARM64 (Linux) | 64 | Little | Down | IEEE 754 | System V | GAS |
| ARM64 (macOS) | 64 | Little | Down | IEEE 754 | Darwin | GAS |

**Floating-Point Formats:**
- **IEEE 754**: Standard IEEE floating-point (binary)
- **HFP**: IBM Hexadecimal Floating Point (base-16 exponent, used in S/370, S/390)
- **HFP+IEEE**: Both formats supported (z/Architecture)

**OS ABI Variants:**
- **System V**: Standard Unix/Linux ABI
- **Darwin**: macOS/Apple ABI (underscore prefix, Mach-O format)
- **MVS**: IBM z/OS ABI

## Building

```bash
# Build library and examples
make

# Library only
make lib

# Examples only
make examples

# Clean
make clean

# Install (requires root)
sudo make install
```

## Basic Usage

```c
#include <anvil/anvil.h>

int main(void)
{
    // Create context
    anvil_ctx_t *ctx = anvil_ctx_create();
    
    // Set target architecture
    anvil_ctx_set_target(ctx, ANVIL_ARCH_ZARCH);
    
    // Create module
    anvil_module_t *mod = anvil_module_create(ctx, "my_module");
    
    // Create function type: int add(int a, int b)
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    
    // Create function
    anvil_func_t *func = anvil_func_create(mod, "add", func_type, ANVIL_LINK_EXTERNAL);
    
    // Set insertion point
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    // Get parameters
    anvil_value_t *a = anvil_func_get_param(func, 0);
    anvil_value_t *b = anvil_func_get_param(func, 1);
    
    // Build IR: result = a + b
    anvil_value_t *result = anvil_build_add(ctx, a, b, "result");
    
    // Build IR: return result
    anvil_build_ret(ctx, result);
    
    // Generate code
    char *output = NULL;
    size_t len = 0;
    anvil_module_codegen(mod, &output, &len);
    
    printf("%s", output);
    
    // Cleanup
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    
    return 0;
}
```

## Supported IR Operations

### Arithmetic

* `anvil_build_add` : Addition
* `anvil_build_sub` : Subtraction
* `anvil_build_mul` : Multiplication
* `anvil_build_sdiv` / `anvil_build_udiv` : Division (signed/unsigned)
* `anvil_build_smod` / `anvil_build_umod` : Modulo (signed/unsigned)
* `anvil_build_neg` : Negation

### Bitwise

* `anvil_build_and` : AND
* `anvil_build_or` : OR
* `anvil_build_xor` : XOR
* `anvil_build_not` : NOT
* `anvil_build_shl` : Shift left
* `anvil_build_shr` : Shift right (logical)
* `anvil_build_sar` : Shift right (arithmetic)

### Comparison

* `anvil_build_cmp_eq` / `anvil_build_cmp_ne` : Equal / Not equal
* `anvil_build_cmp_lt` / `anvil_build_cmp_le` : Less than / Less or equal
* `anvil_build_cmp_gt` / `anvil_build_cmp_ge` : Greater than / Greater or equal
* Unsigned versions: `_ult`, `_ule`, `_ugt`, `_uge`

### Memory

* `anvil_build_alloca` : Stack allocation
* `anvil_build_load` : Load from memory
* `anvil_build_store` : Store to memory
* `anvil_build_gep` : Get Element Pointer (array indexing)
* `anvil_build_struct_gep` : Get Struct Field Pointer
* `anvil_module_add_global` : Add global variable

### Control Flow

* `anvil_build_br` : Unconditional branch
* `anvil_build_br_cond` : Conditional branch
* `anvil_build_call` : Function call
* `anvil_build_ret` / `anvil_build_ret_void` : Return

### Type Conversion

* `anvil_build_trunc` : Truncate
* `anvil_build_zext` : Zero extend
* `anvil_build_sext` : Sign extend
* `anvil_build_bitcast` : Bitcast
* `anvil_build_ptrtoint` / `anvil_build_inttoptr` : Pointer/integer conversion

### Floating-Point

* `anvil_build_fadd` : FP Addition
* `anvil_build_fsub` : FP Subtraction
* `anvil_build_fmul` : FP Multiplication
* `anvil_build_fdiv` : FP Division
* `anvil_build_fneg` : FP Negation
* `anvil_build_fabs` : FP Absolute value
* `anvil_build_fcmp` : FP Comparison

### FP Conversions

* `anvil_build_fptrunc` : Truncate (f64 → f32)
* `anvil_build_fpext` : Extend (f32 → f64)
* `anvil_build_fptosi` : FP to signed integer
* `anvil_build_fptoui` : FP to unsigned integer
* `anvil_build_sitofp` : Signed integer to FP
* `anvil_build_uitofp` : Unsigned integer to FP

### Miscellaneous

* `anvil_build_phi` : PHI node
* `anvil_build_select` : Select (ternary)

## Supported Types

* Integers: `i8`, `i16`, `i32`, `i64` (signed)
* Integers: `u8`, `u16`, `u32`, `u64` (unsigned)
* Floating point: `f32`, `f64`
* Pointers: `anvil_type_ptr(ctx, pointee_type)`
* Arrays: `anvil_type_array(ctx, elem_type, count)`
* Structs: `anvil_type_struct(ctx, name, fields, num_fields)`
* Functions: `anvil_type_func(ctx, ret_type, params, num_params, variadic)`

## Calling Conventions

| Architecture | Convention | Description |
|--------------|------------|-------------|
| x86 | CDECL | Parameters on stack, caller cleanup |
| x86-64 | System V | RDI, RSI, RDX, RCX, R8, R9, then stack |
| S/370 | MVS | R1 points to parameter list |
| S/390 | MVS | R1 points to parameter list |
| z/Arch | z/OS 64-bit | R1 points to parameter list (64-bit) |
| PPC32 | System V | r3-r10 for args, r3 for return |
| PPC64 BE | ELFv1 | r3-r10 for args, function descriptors |
| PPC64 LE | ELFv2 | r3-r10 for args, local entry points |
| ARM64 (Linux) | AAPCS64 | x0-x7 for args, x0 for return |
| ARM64 (macOS) | Apple ARM64 | x0-x7 for args, underscore prefix on symbols |

## Mainframe Notes

### GCCMVS Compatibility

ANVIL generates code compatible with GCCMVS conventions:

* **CSECT**: Blank (no module name prefix)
* **AMODE/RMODE**: `AMODE ANY`, `RMODE ANY` for maximum flexibility
* **Function Names**: UPPERCASE (e.g., `FACTORIAL`, `SUM_ARRAY`)
* **Stack Allocation**: Direct stack offset from R13 (no GETMAIN/FREEMAIN)
* **VL Bit**: NOT cleared, allowing full 31/64-bit addressing

### Stack Direction

Unlike x86 where the stack grows downward (toward lower addresses), IBM mainframes grow the stack upward (toward higher addresses). ANVIL handles this automatically.

### Save Areas

Mainframes use chained save areas instead of push/pop on the stack:

* S/370/S/390: 72 bytes (18 fullwords of 4 bytes)
* z/Architecture: 144 bytes (18 doublewords of 8 bytes)

### Stack-Based Code Generation

The mainframe backends generate efficient stack-based code:

* Stack frame allocation via `LA R2,72(,R13)` (no GETMAIN overhead)
* Proper save area chaining
* Thread-safe execution
* Simplified epilogue (no FREEMAIN cleanup)

### HLASM Output

Generated mainframe code is in HLASM (High Level Assembler) format:

* Labels in columns 1-8
* Opcodes starting at column 10
* Operands starting at column 16
* Comments with asterisk in column 1

## Adding New Backends

To add support for a new architecture:

1. Create a new file at `src/backend/<arch>/<arch>.c`

2. Implement the `anvil_backend_ops_t` structure:

```c
const anvil_backend_ops_t anvil_backend_myarch = {
    .name = "MyArch",
    .arch = ANVIL_ARCH_MYARCH,
    .init = myarch_init,
    .cleanup = myarch_cleanup,
    .codegen_module = myarch_codegen_module,
    .codegen_func = myarch_codegen_func,
    .get_arch_info = myarch_get_arch_info
};
```

3. Add the architecture to `anvil.h`:

```c
typedef enum {
    // ...
    ANVIL_ARCH_MYARCH,
    ANVIL_ARCH_COUNT
} anvil_arch_t;
```

4. Register the backend in `backend.c`:

```c
anvil_register_backend(&anvil_backend_myarch);
```

## Recent Updates

### Struct Support
- Struct field access via `anvil_build_struct_gep()` for all mainframe backends
- Automatic field offset calculation at compile time
- Efficient `LA` (Load Address) instruction for small offsets
- Example: `struct Point { int x; int y; }` in `examples/struct_test.c`

### Array Support (GEP)
- Full array indexing via `anvil_build_gep()` for all mainframe backends
- Automatic element size calculation (1, 2, 4, 8 bytes)
- Efficient index multiplication using shifts (`SLL`/`SLLG`)
- Example: `sum_array(int *arr, int n)` in `examples/array_test.c`

### Floating-Point Support (Mainframes)
- Full floating-point arithmetic for all mainframe backends
- **HFP (Hexadecimal FP)**: S/370, S/370-XA, S/390 (ADR, MDR, DDR instructions)
- **IEEE 754 (Binary FP)**: z/Architecture, S/390 optional (ADBR, MDBR, DDBR instructions)
- FP format selection via `anvil_ctx_set_fp_format()`
- Float↔Int conversion using Magic Number technique (HFP) or native CFDBR (IEEE)

### Control Flow Support
- Full support for loops (while, for) and conditionals (if/else)
- Proper branch label generation with function-prefixed names (`func$block`)
- Correct conditional branch code generation

### Local Variable Allocation
- Stack slot allocation for local variables via `anvil_build_alloca`
- Direct stack offset addressing for efficient memory access
- Automatic dynamic area sizing including local variables and FP temps

### Instruction Optimizations
- **AHI/AGHI**: Add Halfword Immediate for small constants (S/390, z/Architecture)
- **Direct stack access**: Load/Store directly from stack slots without intermediate registers
- **Relative branches**: J/JNZ instead of B/BNZ for better performance (S/390+)

### Global Variables Support
- Full support for global variables on all mainframe backends
- Direct load/store to globals without intermediate address calculation
- Type-aware storage allocation (C, H, F, FD, E, D)
- Support for initialized globals with `DC` (Define Constant)
- UPPERCASE naming convention (GCCMVS compatible)
- Example: `examples/global_test.c`

### PowerPC Backend Support
- **PPC32**: 32-bit big-endian, System V ABI, GAS output
- **PPC64 BE**: 64-bit big-endian, ELFv1 ABI with function descriptors (`.opd` section)
- **PPC64 LE**: 64-bit little-endian, ELFv2 ABI with `.localentry` directives
- Full IR operation support: arithmetic, bitwise, memory, control flow, comparisons
- Type conversions: truncation, zero/sign extension, bitcast, pointer-int
- Floating-point operations (IEEE 754): fadd, fsub, fmul, fdiv, fneg, fabs, fcmp
- FP conversions: sitofp, uitofp, fptosi, fptoui, fpext, fptrunc
- Stack slot allocation for local variables (`alloca`)
- String table management for string literals
- Global variable emission with proper alignment
- GEP and STRUCT_GEP for array and struct access

## IR Optimization

ANVIL includes a configurable optimization pass infrastructure that can be enabled or disabled.

### Optimization Levels

| Level | Name | Description |
|-------|------|-------------|
| O0 | `ANVIL_OPT_NONE` | No optimization (default) |
| O1 | `ANVIL_OPT_BASIC` | Constant folding, DCE, copy propagation |
| O2 | `ANVIL_OPT_STANDARD` | O1 + CFG simplification, strength reduction, memory opts, CSE |
| O3 | `ANVIL_OPT_AGGRESSIVE` | O2 + loop unrolling |

### Available Passes

* **Constant Folding**: Evaluates constant expressions at compile time (`3 + 5` → `8`)
* **Dead Code Elimination (DCE)**: Removes unused instructions
* **Strength Reduction**: Replaces expensive ops with cheaper ones (`x * 8` → `x << 3`)
* **CFG Simplification**: Merges blocks, removes unreachable code
* **Copy Propagation**: Replaces uses of copied values with originals
* **Dead Store Elimination**: Removes stores overwritten before read
* **Redundant Load Elimination**: Reuses loaded values from same address
* **Common Subexpression Elimination (CSE)**: Reuses computed values
* **Loop Unrolling**: Unrolls small loops with known trip counts (O3, experimental)

### Usage

```c
#include <anvil/anvil_opt.h>

// Set optimization level
anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);

// Optimize module before codegen
anvil_module_optimize(mod);

// Or fine-grained control
anvil_pass_manager_t *pm = anvil_ctx_get_pass_manager(ctx);
anvil_pass_manager_enable(pm, ANVIL_PASS_CONST_FOLD);
anvil_pass_manager_disable(pm, ANVIL_PASS_DCE);
```

### Example: Constant Folding (S/390)

**Before optimization:**
```hlasm
         LA    R2,3            Load constant 3
         AHI   R2,5            Add 5
         LR    R15,R2          Result in R15
```

**After optimization:**
```hlasm
         LA    R15,8           Load constant 8 directly
```

### Example: Strength Reduction (S/390)

**Before optimization:**
```hlasm
         LA    R3,8            Load constant 8
         MSR   R2,R3           Multiply (expensive)
```

**After optimization:**
```hlasm
         LA    R3,3            Load shift amount
         SLL   R2,0(R3)        Shift left by 3 (x * 8 = x << 3)
```

## Roadmap

* Binary opcode generation
* ASI/AGSI optimization (Add to Storage Immediate)
* Register allocation improvements
* RISC-V support
* Debug info (DWARF)

## Documentation

See `DOCUMENTATION.md` for complete API reference and detailed usage examples.

## License

Unlicense
