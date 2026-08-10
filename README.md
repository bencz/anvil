# ANVIL

A C library for compiler code generation with support for multiple architectures through a portable intermediate representation (IR) system.

## Features

* Portable source IR: Architecture-independent modules, functions, basic blocks, SSA values, typed constants, globals, and structured control flow.
* Source IR verifier: `anvil_module_codegen()` rejects invalid IR before optimization or backend lowering.
* Configurable optimizer: Copy propagation, constant folding, CSE, strength reduction, memory optimizations, CFG simplification, and DCE with deterministic pass ordering.
* MachineIR and register allocation: A generic backend layer with target-independent virtual registers, fixed ABI registers, stack/frame slots, spill materialization, copy coalescing, and linear-scan allocation.
* Backend architecture: Backends can either emit directly from source IR or lower through MachineIR; the ARM64 backend is the current reference implementation for the MachineIR path.
* Multiple backend targets: x86, x86-64, S/370, S/370-XA, S/390, z/Architecture, PowerPC 32/64-bit, PPC64LE, and ARM64.
* CPU model system: Target-specific CPU model selection and feature flags.
* Assembly output: Generates assembly text (HLASM for mainframes, GAS/Mach-O-compatible ARM64 output, and GAS/NASM-style x86/PPC output).

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

# Unit/regression tests for core, optimizer, MachineIR/regalloc, and reference ARM64 MIR lowering
make tests

# Runtime example: generate assembly, assemble/link it, and execute it
make test-examples

# Advanced examples (fp_math_lib, dynamic_array, base64_lib)
make examples-advanced

# Advanced runtime tests: generate, assemble/link, and execute generated libraries
make test-examples-advanced

# MCC sample compiler execution suite
make -C samples/mcc test-exec

# Clean
make clean

# Clean advanced examples
make clean-examples-advanced

# Install (requires root)
sudo make install
```

## Basic Usage

```c
#include <anvil/anvil.h>

int main(void)
{
    // Create context
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    
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
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    if (err != ANVIL_OK) {
        fprintf(stderr, "codegen failed: %s\n", anvil_ctx_get_error(ctx));
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
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
* `anvil_build_alloca_dyn` : Runtime-sized stack allocation
* `anvil_build_load` : Load from memory
* `anvil_build_store` : Store to memory
* `anvil_build_gep` : Get Element Pointer (array indexing)
* `anvil_build_struct_gep` : Get Struct Field Pointer
* `anvil_module_add_global` : Add global variable

### Control Flow

* `anvil_build_br` : Unconditional branch
* `anvil_build_br_cond` : Conditional branch
* `anvil_build_switch` / `anvil_switch_add_case` : Multi-way branch
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
* `anvil_phi_add_incoming` : Add PHI predecessor/value edge
* `anvil_build_select` : Select (ternary)

## Supported Types

* Boolean: `i1` (one semantic bit, one-byte storage)
* Integers: `i8`, `i16`, `i32`, `i64` (signed)
* Integers: `u8`, `u16`, `u32`, `u64` (unsigned)
* Floating point: `f32`, `f64`
* Pointers: `anvil_type_ptr(ctx, pointee_type)`
* Arrays: `anvil_type_array(ctx, elem_type, count)`
* Structs: `anvil_type_struct(ctx, name, fields, num_fields)`
* Functions: `anvil_type_func(ctx, ret_type, params, num_params, variadic)`

Function definitions/declarations are exposed as callable address values. In practice, `anvil_func_get_value(func)` has type `ptr<func>`, so it can be stored in memory, loaded back, and called indirectly. Direct and indirect calls use the same `anvil_build_call()` API; the function type passed to the builder describes the callee signature.

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

1. Create backend files under `src/backend/<arch>/`.

2. Choose the implementation path:
   - Use a direct source-IR emitter for a minimal bootstrap backend.
   - Use MachineIR for production-quality backend work so register allocation, spills, fixed ABI registers, and frame/spill slots stay shared.

3. Implement the `anvil_backend_ops_t` structure:

```c
const anvil_backend_ops_t anvil_backend_myarch = {
    .name = "MyArch",
    .arch = ANVIL_ARCH_MYARCH,
    .init = myarch_init,
    .cleanup = myarch_cleanup,
    .reset = myarch_reset,      // Clear cached IR pointers (optional but recommended)
    .prepare_ir = myarch_prepare_ir,  // Prepare/lower IR before codegen (optional)
    .codegen_module = myarch_codegen_module,
    .codegen_func = myarch_codegen_func,
    .get_arch_info = myarch_get_arch_info
};
```

4. Add the architecture to `anvil.h`:

```c
typedef enum {
    // ...
    ANVIL_ARCH_MYARCH,
    ANVIL_ARCH_COUNT
} anvil_arch_t;
```

5. Register the backend in `backend.c`:

```c
anvil_register_backend(&anvil_backend_myarch);
```

## Current Architecture

ANVIL now has a two-level IR architecture:

```
Frontend/API
   │
   ▼
Source IR
   Modules -> Functions -> Blocks -> Instructions -> Values
   │
   ├── source verifier
   ├── target-independent optimizer
   ▼
Backend lowering
   │
   ├── direct emitter path
   │      Source IR -> target assembly
   │
   └── MachineIR path
          Source IR -> target lowerer -> virtual registers
          -> ABI constraints -> regalloc -> spill materialization
          -> target assembly
```

MachineIR is the production backend implementation path. ARM64 is the stable
reference backend for validating the design and should be treated as the
canonical implementation model. PowerPC and IBM mainframe targets also use
shared MachineIR lowerers with target descriptors. The x86 and x86-64 backends
are older direct source-IR emitters and are useful only as legacy/bootstrap
code, not as references for new backend work.

### Source IR Verifier

`anvil_module_codegen()` validates source IR before optimization and codegen. The verifier checks:

- value ownership: parameters and instruction results must belong to the function being verified
- typed memory operations: `load`/`store` address pointee types must match value/result types
- call signatures: argument count, fixed parameter types, variadic minimum arity, and return type
- function values: direct and indirect callees are accepted as `func` or `ptr<func>`
- PHI incoming blocks: incoming values must correspond to real predecessors
- switch terminators: selector/case type consistency and valid destinations
- block termination: non-declaration functions must terminate every block

### Optimizer

Optimization is target-independent and runs before backend `prepare_ir`/codegen. Passes are managed by `anvil_pass_manager_t` and executed in a fixed order designed to expose new opportunities while keeping DCE last in each iteration:

1. copy propagation
2. constant folding
3. common subexpression elimination
4. strength reduction
5. store-load propagation
6. dead store elimination
7. load elimination
8. CFG simplification
9. dead code elimination

The pass manager verifies the current function after every pass and iterates to
a bounded fixpoint. The bound defaults to 10 and is configurable through
`anvil_pass_manager_set_iteration_limit()`; pass failure, invalid IR, or failure
to converge is reported as an error. Only implemented passes are exposed by the
public pass enum.

### MachineIR and Regalloc

`include/anvil/anvil_machine.h` exposes a target-independent MachineIR layer:

- typed virtual registers with GPR/FPR/flags/special classes
- fixed physical register constraints for ABI values such as arguments and returns
- basic blocks, branches, direct calls, indirect calls, frame slots, string literals, and spill slots
- copy coalescing before allocation
- linear-scan allocation by register class
- spill materialization using backend-provided scratch register classes
- verifier for MachineIR structural invariants

Backends can lower source IR to MachineIR, allocate registers, materialize spills, then emit target assembly from allocated machine instructions. This keeps optimization, value lifetime handling, spill insertion, and ABI fixed-register constraints in shared infrastructure while leaving instruction selection and final assembly emission target-specific.

### Reference MachineIR Backend: ARM64

ARM64 is documented here as the current reference implementation for the generic MachineIR design, not as the final focus of the project. It is the backend that currently exercises the full path from source IR to MachineIR, register allocation, spill materialization, and ABI-aware assembly emission. The same structure is the intended implementation model for the remaining targets.

- `src/backend/arm64/arm64.c`: backend lifecycle and module/function codegen entry points
- `src/backend/arm64/arm64_helpers.c`: target sizes, alignment, ABI helpers, and constants
- `src/backend/arm64/arm64_mir.c`: source IR lowering, ARM64 MachineIR legality checks, regalloc bridge, and assembly emission
- `src/backend/arm64/opt/`: target-specific preparation/optimization hooks

The ARM64 reference implementation currently covers:

- integer and floating-point arithmetic, comparisons, casts, and select
- stack frame slots, static `alloca`, dynamic `alloca`, globals, and string literals
- typed loads/stores including signed byte/halfword/word extension
- GEP lowering with constant-offset folding and runtime index scaling
- struct field addressing
- PHI lowering via edge copies, including conditional branches and parallel-copy cycles
- switch lowering to compare/branch chains
- direct calls (`bl symbol`) and indirect function pointer calls (`blr x16`)
- ABI fixed registers for x0-x7/d0-d7 arguments and x0/d0 returns
- outgoing stack arguments and incoming stack arguments
- Darwin variadic-call behavior with variadic arguments placed on the stack
- Darwin/Mach-O symbol prefixes and `@PAGE`/`@PAGEOFF` relocation forms

### Function Pointers

Function definitions and declarations keep their canonical `ANVIL_TYPE_FUNC` signature internally, but the callable value exposed through `anvil_func_get_value()` is `ptr<func>`. This matches C-style function pointer behavior:

```c
anvil_type_t *params[] = { anvil_type_i32(ctx), anvil_type_i32(ctx) };
anvil_type_t *fn_type = anvil_type_func(ctx, anvil_type_i32(ctx), params, 2, false);
anvil_func_t *add_fn = anvil_func_create(mod, "add", fn_type, ANVIL_LINK_EXTERNAL);

anvil_type_t *fn_ptr_type = anvil_type_ptr(ctx, fn_type);
anvil_value_t *slot = anvil_build_alloca(ctx, fn_ptr_type, "slot");
anvil_build_store(ctx, anvil_func_get_value(add_fn), slot);

anvil_value_t *loaded = anvil_build_load(ctx, fn_ptr_type, slot, "loaded_fn");
anvil_value_t *args[] = { anvil_const_i32(ctx, 3), anvil_const_i32(ctx, 4) };
anvil_value_t *result = anvil_build_call(ctx, fn_type, loaded, args, 2, "result");
```

Backend emitters choose the concrete call instruction. In the current ARM64 reference backend, direct calls emit `bl`, while loaded function pointers are copied to `x16` and emitted as `blr x16`.

### MCC Integration

`samples/mcc` is the integration stress test for the generic IR, verifier, optimizer, and backend contract. It is a small C compiler frontend that drives ANVIL codegen. On the current development host, `make -C samples/mcc test-exec` validates generated ARM64/macOS executables because that is the most complete executable backend path today. The execution suite covers arithmetic, arrays, pointer arithmetic, structs, switch, recursion, long long arithmetic, preprocessing, strings, matrix operations, and function pointers, and should be reused as additional MachineIR-backed targets come online.

### IR Debug/Dump API
New debugging functionality for inspecting IR structures:

```c
#include <anvil/anvil.h>  // anvil_debug.h is now included automatically

// Print module IR to stdout
anvil_print_module(mod);

// Print function IR to stdout
anvil_print_func(func);

// Dump to FILE*
anvil_dump_module(stderr, mod);
anvil_dump_func(stderr, func);
anvil_dump_block(stderr, block);
anvil_dump_instr(stderr, instr);

// Convert to string (caller must free)
char *ir_str = anvil_module_to_string(mod);
printf("%s", ir_str);
free(ir_str);

// Check if block has terminator (ret, br, br_cond, switch)
if (!anvil_block_has_terminator(block)) {
    anvil_build_ret_void(ctx);  // Add implicit return
}

// Check if value is boolean (comparison result)
if (anvil_value_is_bool(cond)) {
    // Already boolean, use directly in br_cond
} else {
    // Need to compare with zero first
    cond = anvil_build_cmp_ne(ctx, cond, zero, "tobool");
}

// Get type of a value
anvil_type_t *type = anvil_value_get_type(val);
```

**String escaping**: String constants in IR dumps are properly escaped (`\n`, `\t`, `\0`, `\xHH` for non-printable characters).

**Output format:**
```
; ModuleID = 'my_module'
; Functions: 2, Globals: 1

@counter = external global i32 42

define external i32 @factorial(i32 %arg0) {
entry:
    %cmp = cmp_le i8 %arg0, 1
    br_cond %cmp, label %base_case, label %recurse
...
}
```

### Memory Management Improvements
Improved cleanup flow to prevent dangling pointers and use-after-free issues:

- **Backend reset function**: New `reset` callback in `anvil_backend_ops_t` to clear cached IR pointers
- **Safe cleanup order**: `anvil_ctx_destroy()` now resets backend state before destroying modules
- **All backends updated**: x86, x86-64, ARM64, S/370, S/370-XA, S/390, z/Architecture, PPC32, PPC64, PPC64LE

### Advanced Examples
Three advanced examples demonstrate ANVIL's capabilities for generating linkable libraries:

- **`examples/fp_math_lib/`**: Floating-point math library
  - Generates exportable FP functions: `fp_add`, `fp_sub`, `fp_mul`, `fp_div`, `fp_neg`, `fp_abs`
  - Demonstrates ANVIL IR for floating-point operations
  - Includes C test program that links with generated assembly (24 tests)

- **`examples/dynamic_array/`**: Dynamic array library with C library calls
  - Demonstrates calling external C functions: `malloc`, `free`, `memcpy`
  - Functions: `array_create`, `array_destroy`, `array_copy`, `array_sum`, `array_max`, `array_min`, `array_count_if`, `array_scale`
  - Shows pointer arithmetic, loops, conditionals, and memory management
  - Includes comprehensive test suite (41 tests)

- **`examples/base64_lib/`**: Base64 encoding library
  - Demonstrates complex bitwise operations, byte manipulation, and lookup table logic
  - Functions: `base64_encode`, `base64_encoded_len`
  - Shows `select` operations for conditional value computation
  - Includes test suite with RFC 4648 test vectors (28 tests)

## IR Optimization

ANVIL includes a configurable optimization pass infrastructure that can be enabled or disabled.

### Optimization Levels

| Level | Name | Description |
|-------|------|-------------|
| O0 | `ANVIL_OPT_NONE` | No optimization (default) |
| Og | `ANVIL_OPT_DEBUG` | Debug-friendly: copy propagation, store-load propagation |
| O1 | `ANVIL_OPT_BASIC` | Og + constant folding, DCE |
| O2 | `ANVIL_OPT_STANDARD` | O1 + CFG simplification, strength reduction, memory opts, CSE |
| O3 | `ANVIL_OPT_AGGRESSIVE` | Currently the same verified pass set as O2 |

### Available Passes

| Pass | Level | Description |
|------|-------|-------------|
| **Constant Folding** | O1+ | Evaluates constant expressions at compile time (`3 + 5` → `8`) |
| **Dead Code Elimination (DCE)** | O1+ | Removes unused instructions |
| **Copy Propagation** | Og+ | Replaces uses of copied values with originals |
| **Store-Load Propagation** | Og+ | Replaces load after store with stored value |
| **Strength Reduction** | O2+ | Replaces expensive ops with cheaper ones (`x * 8` → `x << 3`) |
| **CFG Simplification** | O2+ | Merges blocks, removes unreachable code, and preserves `switch` CFG edges |
| **Dead Store Elimination** | O2+ | Removes stores overwritten before read |
| **Redundant Load Elimination** | O2+ | Reuses loaded values from same address |
| **Common Subexpression Elimination (CSE)** | O2+ | Reuses computed values |

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
* Migrate existing direct text emitters to the MachineIR/regalloc path
* Complete and enable loop unrolling
* Broaden MachineIR coverage for additional ABIs and future targets
* RISC-V support
* Debug info (DWARF)
* Deeper CPU-model-specific instruction selection

## Documentation

See `DOCUMENTATION.md` for API reference and detailed usage examples. See `samples/mcc/README.md` and `samples/mcc/docs/` for the C compiler sample.

## License

Unlicense
