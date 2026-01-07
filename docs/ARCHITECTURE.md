# ANVIL Compiler Architecture

## Overview

ANVIL is a retargetable compiler backend library written in C99. It provides a complete pipeline from high-level IR to machine code assembly for multiple architectures.

## Core Design Principles

### 1. ZERO #ifdef for Targets
All target-specific behavior is handled through **vtables** (function pointers in structs). This allows:
- Cross-compilation from any host to any target
- Clean separation of concerns
- Easy addition of new targets

### 2. Vtable-Based Architecture
Every target-specific component uses vtables:
- `AnvilABI` - Calling conventions, argument passing, symbol formatting
- `AnvilBackend` - Register allocation, instruction emission, optimizations
- `AnvilTargetInfo` - Architecture properties (word size, endianness, etc.)
- `AnvilRegSet` - Available registers and their properties

### 3. Arena-Based Memory Management
All allocations go through `AnvilArena`, providing:
- Fast bump allocation
- Automatic cleanup on arena destruction
- No individual free() calls needed

## Directory Structure

```
anvil/
├── include/
│   ├── anvil.h              # Main public API header
│   └── anvil/
│       ├── types.h          # Type definitions
│       ├── target.h         # Target/ABI definitions
│       └── result.h         # Compilation result
├── src/
│   ├── core/                # Core utilities
│   │   ├── arena.c/h        # Arena allocator
│   │   ├── vec.c/h          # Dynamic vector
│   │   ├── hash.c/h         # Hash table
│   │   ├── str.c/h          # String utilities
│   │   └── error.c/h        # Error handling
│   ├── ir/                  # High-level IR
│   │   ├── types.c/h        # Type system
│   │   ├── value.c/h        # Values and variables
│   │   ├── inst.c/h         # IR instructions
│   │   ├── func.c/h         # Functions and blocks
│   │   ├── module.c/h       # Module container
│   │   └── builder.c/h      # IR builder helpers
│   ├── mir/                 # Machine IR
│   │   ├── mir.c/h          # MIR structures
│   │   ├── lower.c/h        # IR to MIR lowering
│   │   ├── cfg.c/h          # Control flow graph
│   │   ├── liveness.c/h     # Liveness analysis
│   │   └── regalloc.c/h     # Register allocation
│   ├── opt/                 # Optimizations
│   │   ├── ir_opt.c/h       # IR-level optimizations
│   │   └── mir_opt.c/h      # MIR-level optimizations
│   ├── backend/             # Target backends
│   │   ├── backend.c/h      # Backend interface
│   │   ├── x86_64/          # x86_64 backend
│   │   │   ├── target.c/h   # Target info
│   │   │   ├── regs.c/h     # Register definitions
│   │   │   ├── emit.c/h     # Assembly emission
│   │   │   ├── x86_64.c     # Backend entry point
│   │   │   └── abi/
│   │   │       ├── sysv.c/h # System V AMD64 ABI
│   │   │       └── win64.c/h# Windows x64 ABI
│   │   ├── arm64/           # ARM64 backend
│   │   │   ├── target.c/h
│   │   │   ├── regs.c/h
│   │   │   ├── emit.c/h
│   │   │   ├── arm64.c
│   │   │   └── abi/
│   │   │       ├── aapcs64.c/h  # AAPCS64 ABI (Linux)
│   │   │       └── apple.c/h    # Apple ARM64 ABI
│   │   └── ppc64/           # PPC64 Big Endian backend
│   │       ├── target.c/h   # Target info (Big Endian)
│   │       ├── regs.c/h     # Register definitions (r0-r31, f0-f31)
│   │       ├── emit.c/h     # Assembly emission
│   │       ├── ppc64.c      # Backend entry point
│   │       ├── abi/
│   │       │   └── elfv2.c/h    # ELFv2 ABI (Linux)
│   │       └── opt/
│   │           └── peephole.c/h # Target-specific optimizations
│   └── api.c                # Public API implementation
└── examples/                # Example programs
```

## Compilation Pipeline

```
Source Code (User)
       │
       ▼
┌──────────────┐
│   IR Build   │  anvil_func_new(), anvil_add(), anvil_ret(), etc.
└──────────────┘
       │
       ▼
┌──────────────┐
│  IR Optimize │  anvil_opt_run_all() - Constant folding, DCE, CFG simplification
└──────────────┘
       │
       ▼
┌──────────────┐
│  MIR Lower   │  anvil_lower_module_with_abi() - IR to MIR with ABI awareness
└──────────────┘
       │
       ▼
┌──────────────┐
│ MIR Analyze  │  anvil_mir_analyze_function() - Detect leaf, compute needs_frame
└──────────────┘
       │
       ▼
┌──────────────┐
│ MIR Optimize │  anvil_mir_opt_run_all() - Peephole, strength reduction, copy prop
└──────────────┘
       │
       ▼
┌──────────────┐
│  Reg Alloc   │  backend->regalloc() - Linear scan with ABI-aware preallocation
└──────────────┘
       │
       ▼
┌──────────────┐
│Target Peephole│  backend->peephole_optimize() - Architecture-specific opts
└──────────────┘
       │
       ▼
┌──────────────┐
│  Emit ASM    │  backend->emit_mir() - Target-specific assembly generation
└──────────────┘
       │
       ▼
   Assembly Output
```

### Pipeline Implementation (api.c)

```c
AnvilCompileResult anvil_compile(AnvilModule* mod, AnvilTarget target, int opt_level) {
    AnvilBackend* backend = anvil_get_backend(target.arch);
    
    // 1. IR Optimization
    anvil_opt_run_all(mod, opt_level, NULL);
    
    // 2. MIR Lowering with ABI
    const AnvilABI* abi = backend->get_abi(target.os, target.abi_name);
    AnvilMIR* mir = anvil_lower_module_with_abi(mod, abi);
    
    // 3. MIR Analysis + Optimization
    anvil_mir_opt_run_all(mir, opt_level, NULL);  // Includes analyze_function
    
    // 4. Register Allocation + Target Peephole
    for (AnvilMFunc* func = mir->first_func; func; func = func->next) {
        backend->regalloc(backend, func, target.os, target.abi_name);
        backend->peephole_optimize(backend, func);
    }
    
    // 5. Emit Assembly
    backend->emit_mir(backend, mir, &asm_buf, target.os, target.abi_name);
    
    return result;
}
```

## Key Data Structures

### AnvilType
Represents types in the IR:
- Primitive: void, bool, i8-i64, u8-u64, f32, f64
- Compound: pointers, arrays, structs, functions

### AnvilValue
Represents values:
- Constants (int, float, string)
- Variables (parameters, locals)
- Temporaries (instruction results)

### AnvilInst
IR instructions:
- Arithmetic: ADD, SUB, MUL, DIV, MOD
- Bitwise: AND, OR, XOR, SHL, SHR
- Comparison: EQ, NE, LT, LE, GT, GE
- Memory: LOAD, STORE, ALLOCA
- Control: BR, BR_COND, RET, CALL

### AnvilMInst
MIR instructions (closer to machine):
- Similar to IR but with explicit operands
- Uses virtual registers (vregs) initially
- Physical registers (pregs) after allocation

### AnvilMOperand
MIR operand types:
- VREG: Virtual register
- PREG: Physical register
- IMM: Immediate value
- MEM: Memory reference (base + index*scale + disp)
- LABEL: Branch target
- FUNC: Function reference

### AnvilABI
Calling convention vtable:
```c
typedef struct AnvilABI {
    const char* name;
    
    // Argument registers
    const int* arg_regs_int;
    int num_arg_regs_int;
    const int* arg_regs_float;
    int num_arg_regs_float;
    
    // Return registers
    int ret_reg_int_lo;
    int ret_reg_int_hi;
    int ret_reg_float;
    
    // Saved registers
    const int* callee_saved_regs;
    int num_callee_saved;
    const int* caller_saved_regs;
    int num_caller_saved;
    
    // Stack properties
    int stack_alignment;
    int arg_area_alignment;
    int red_zone_size;
    
    // Calling convention flags
    bool args_right_to_left;
    bool callee_cleans_stack;
    bool return_in_memory_hidden_arg;
    
    // Symbol formatting
    bool uses_underscore_prefix;
    
    // Variadic function handling
    bool variadic_args_on_stack;  // ARM64 Apple: variadic args go on stack
    
    // Vtable functions
    AnvilClassifyArgFn classify_argument;
    AnvilClassifyRetFn classify_return;
    AnvilComputeFrameFn compute_frame_layout;
    AnvilFormatSymbolFn format_symbol;
    AnvilEmitCallFn emit_call;
    AnvilEmitStringFn emit_string;
    AnvilIsVariadicFn is_variadic;
    AnvilEmitVariadicArgFn emit_variadic_arg;
} AnvilABI;
```

**Key ABI differences:**
- **ARM64 Apple**: `variadic_args_on_stack = true`, `uses_underscore_prefix = true`
- **ARM64 Linux (AAPCS64)**: `variadic_args_on_stack = false`, `uses_underscore_prefix = false`
- **x86_64 System V**: `variadic_args_on_stack = false`, requires `al` register for float arg count
- **x86_64 Win64**: Shadow space required, different register allocation

## Adding a New Target

### 1. Create Target Directory
```
src/backend/newtarget/
├── target.c/h     # AnvilTargetInfo
├── regs.c/h       # Register definitions
├── emit.c/h       # Assembly emission
├── newtarget.c    # Backend entry point
└── abi/
    └── newabi.c/h # ABI implementation
```

### 2. Define Registers
```c
// regs.h
typedef enum {
    NEWTARGET_R0 = 0,
    NEWTARGET_R1,
    // ...
    NEWTARGET_NUM_REGS
} NewTargetReg;

extern const AnvilRegSet newtarget_reg_set;
```

### 3. Implement ABI
```c
// abi/newabi.c
static const int newabi_arg_regs_int[] = { R0, R1, R2, R3 };

const AnvilABI newtarget_newabi = {
    .name = "newabi",
    .arg_regs_int = newabi_arg_regs_int,
    .num_arg_regs_int = 4,
    .uses_underscore_prefix = false,
    // ...
};
```

### 4. Implement Backend
```c
// newtarget.c
static void newtarget_emit_mir_full(AnvilBackend* backend, AnvilMIR* mir, 
                                     AnvilAsmBuffer* out, int os, const char* abi_name) {
    const AnvilABI* abi = newtarget_get_abi(os, abi_name);
    // Emit assembly using abi->uses_underscore_prefix, etc.
}

static AnvilBackend newtarget_backend = {
    .name = "newtarget",
    .emit_mir = newtarget_emit_mir_full,
    .regalloc = newtarget_regalloc,
    // ...
};
```

### 5. Register Backend
```c
// backend.c
void anvil_backends_init(void) {
    anvil_register_backend(ANVIL_ARCH_NEWTARGET, anvil_create_newtarget_backend());
}
```

## Optimization Passes

### IR Optimizations (src/opt/ir_opt.c)

1. **Constant Folding**: Evaluate constant expressions at compile time
2. **Dead Code Elimination**: Remove unused instructions
3. **CFG Simplification**: Merge/eliminate basic blocks

### MIR Optimizations (src/opt/mir_opt.c)

1. **Redundant Move Removal**: Remove `mov r0, r0`
2. **Strength Reduction**: Replace MUL by power of 2 with SHL
3. **Copy Propagation**: Propagate register copies through uses
4. **Move Chain Elimination**: Collapse `mov a, b; mov c, a` to `mov c, b`
5. **MOV-OP-MOV Folding**: Optimize patterns like `mov tmp, src; op tmp, x; mov dst, tmp`
6. **Dead Code Elimination**: Remove NOP and unused instructions

### MIR Analysis (src/opt/mir_opt.c)

The `anvil_mir_analyze_function()` pass computes:
- **is_leaf**: True if function contains no CALL instructions
- **needs_frame**: True if function needs prologue/epilogue (has calls, stack usage, or spills)

This information is used by backends to skip prologue/epilogue for simple leaf functions.

### Target-Specific Optimizations (backend/*/opt/)

Each backend can have its own `opt/peephole.c` for architecture-specific optimizations:
- **x86_64**: LEA optimization, redundant move elimination after regalloc
- **ARM64**: MOV-OP-MOV folding for 3-operand instructions
- **PPC64**: Similar patterns for PowerPC instruction set

## Register Allocation

Uses linear scan with ABI-aware preallocation:

1. **Compute live intervals** for each virtual register
2. **Preallocate parameters** to ABI argument registers
3. **Allocate remaining** vregs using linear scan
4. **Spill** to stack when registers exhausted

```c
AnvilRegAllocConfig config = {
    .available_regs = available_regs,
    .num_available_regs = count,
    .callee_saved = abi->callee_saved_regs,
    .num_callee_saved = abi->num_callee_saved,
    .prealloc = param_prealloc,  // vreg -> preg mappings
    .num_prealloc = num_params,
};
```

## API Usage Example

```c
#include <anvil.h>

int main(void) {
    anvil_init();
    
    AnvilModule* mod = anvil_module_new("example");
    
    // Create function: int add(int a, int b)
    AnvilFunc* fn = anvil_func_new(mod, "add", anvil_type_i32());
    AnvilVar* a = anvil_func_add_param(fn, "a", anvil_type_i32());
    AnvilVar* b = anvil_func_add_param(fn, "b", anvil_type_i32());
    
    // Build IR: return a + b
    AnvilValue* sum = anvil_add(fn, anvil_load(fn, a), anvil_load(fn, b));
    anvil_ret(fn, sum);
    
    // Compile for ARM64 macOS
    AnvilTarget target = anvil_target_arm64_macos();
    AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_NONE);
    
    printf("%s", result.code);
    
    anvil_result_free(&result);
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
```

## Supported Targets

| Architecture | OS      | ABI      | Endianness | Status |
|-------------|---------|----------|------------|--------|
| x86_64      | Linux   | System V | Little     | ✓      |
| x86_64      | Windows | Win64    | Little     | ✓      |
| ARM64       | Linux   | AAPCS64  | Little     | ✓      |
| ARM64       | macOS   | Apple    | Little     | ✓      |
| PPC64       | Linux   | ELFv2    | Big        | ✓      |

## PPC64 Backend

The PPC64 backend targets PowerPC 64-bit Big Endian Linux systems using the ELFv2 ABI.

### Key Features
- **Big Endian support**: Proper byte ordering for cross-compilation from little-endian hosts
- **ELFv2 ABI**: Modern PowerPC ABI with efficient parameter passing
- **Target-specific optimizations**: Located in `ppc64/opt/peephole.c`

### Registers
- **GPRs**: r0-r31 (64-bit general purpose)
- **FPRs**: f0-f31 (64-bit floating point)
- **Argument registers**: r3-r10 (integers), f1-f13 (floats)
- **Callee-saved**: r14-r31
- **Stack pointer**: r1
- **TOC pointer**: r2
- **Link register**: LR

### Example
```c
AnvilTarget target = anvil_target_ppc64_linux();
AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
```

## Future Work

### Completed
- [x] Multi-target support (x86_64, ARM64, PPC64)
- [x] Multiple ABIs per target
- [x] Leaf function detection
- [x] Prologue/epilogue optimization
- [x] Target-specific peephole optimizations
- [x] Copy propagation and move chain elimination
- [x] ABI-aware register allocation

### In Progress
- [ ] Constant materialization (`materialize_constant`)
- [ ] Instruction scheduling (`schedule_instructions`)

### Planned
- [ ] Floating-point register allocation
- [ ] SIMD/vector support
- [ ] More optimization passes (SCCP, mem2reg, loop opts)
- [ ] Object file emission (ELF, Mach-O, PE)
- [ ] Debug info (DWARF)
- [ ] Additional targets (RISC-V, x86, ARM32)
