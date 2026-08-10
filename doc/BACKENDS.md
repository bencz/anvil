# ANVIL Backend Development Guide

This document explains how backends work and how to implement new ones.

## Backend Architecture

### Overview

A backend translates ANVIL IR to target-specific assembly code. Each backend is a self-contained module that implements the `anvil_backend_ops_t` interface.

### CPU Model System

Backends can access CPU model information to generate optimized code for specific processors. The context provides:

```c
// Check if a CPU feature is available
if (anvil_ctx_has_feature(ctx, ANVIL_FEATURE_PPC_VSX)) {
    // Emit VSX instructions
} else {
    // Emit fallback code
}

// Get current CPU model
anvil_cpu_model_t cpu = anvil_ctx_get_cpu(ctx);
```

All actively maintained backends now follow the same MachineIR pipeline that
ARM64 established as the reference path: **lower source IR to MachineIR ->
verify target legality -> coalesce copies -> linear-scan register allocation ->
materialize spills -> emit assembly**. ARM64, x86, x86-64, and the PowerPC family
share this contract; the mainframe (S/370 family) backends use the same pipeline
through their own `*_mir.c` lowering. The linear-scan allocator and the
copy-coalescing / spill-materialization passes are target-independent and live in
`src/machine/` (`regalloc.c`, `machine_ir.c`).

**Example: Shared PowerPC Backend Organization**

PowerPC targets share one MachineIR-backed implementation with
target-specific descriptors for PPC32, PPC64 ELFv1, and PPC64LE ELFv2:

```
src/backend/ppc/
└── ppc_mir.c         # Source IR -> MachineIR -> PPC assembly

src/backend/ppc32/ppc32.c    # thin backend_ops driver (PPC32)
src/backend/ppc64/ppc64.c    # thin backend_ops driver (PPC64 ELFv1)
src/backend/ppc64le/ppc64le.c # thin backend_ops driver (PPC64LE ELFv2)
```

**Example: Shared x86 / x86-64 Backend Organization**

Both x86 backends were rewritten onto the generic MachineIR pipeline, mirroring
ARM64. Each is split into a thin `backend_ops` driver plus a large `*_mir.c`
that holds lowering, legality, the regalloc bridge, and emission:

```
src/backend/x86_64/
├── x86_64.c           # thin backend_ops driver (init/cleanup/codegen entry)
├── x86_64_internal.h  # backend state, register constants, ABI descriptor struct
├── x86_64_helpers.c   # type size/alignment helpers
└── x86_64_mir.c       # Source IR -> MachineIR -> x86-64 assembly (SysV/Darwin/Win64)

src/backend/x86/
├── x86.c              # thin backend_ops driver
├── x86_internal.h     # backend state, register constants, CC/platform descriptors
├── x86_helpers.c      # type sizing + byte-register name tables
└── x86_mir.c          # Source IR -> MachineIR -> x86 assembly (cdecl/stdcall/fastcall)
```

The public per-target MIR entry points are declared in
`include/anvil/anvil_x86_64_mir.h` and `include/anvil/anvil_x86_mir.h`
(e.g. `anvil_x86_64_lower_func_to_mir`, `anvil_x86_64_verify_mir_legal`,
`anvil_x86_64_regalloc_mir`, `anvil_x86_64_emit_mir_abi`, and the x86 analogs).

**Example: ARM64 Backend Organization**

The ARM64 backend is the stable reference implementation for the MachineIR path:

```
src/backend/arm64/
├── arm64.c           # Backend lifecycle and codegen entry points
├── arm64_internal.h  # Register constants and backend state
├── arm64_helpers.c   # Type size/alignment and register names
├── arm64_mir.c       # Source IR -> MachineIR -> ARM64 assembly
└── opt/              # ARM64-specific optimizations
    ├── arm64_opt.h       # Optimization interface
    ├── arm64_opt.c       # Pass manager
    ├── arm64_peephole.c  # Peephole optimizations
    └── arm64_branch.c     # Branch optimization
```

**Key ARM64 Components:**
- **`arm64_mir.c`**: source IR lowering, ABI constraints, PHI edge copies, switch lowering, MachineIR legality checks, register allocation bridge, spill materialization, and assembly emission
- **`arm64_internal.h`**: backend state and ARM64 register constants
- **`arm64_helpers.c`**: target type sizing/alignment helpers and register name tables
- **`arm64.c`**: `arm64_init()`, `arm64_cleanup()`, and module/function codegen entry points

ARM64 deliberately has no target `prepare_ir` hook or dormant target peephole
directory. Source-IR transformations run through the generic pass manager;
target-specific work starts at verified MachineIR lowering so observable memory
operations cannot be erased by an untracked backend rewrite.

The x86 and x86-64 backends are no longer direct source-IR emitters: they were
rewritten onto the same MachineIR pipeline as ARM64 and PowerPC. New backend work
should follow ARM64's MachineIR structure.

### Supported Backends

| Backend | Arch enum | Pipeline | ABIs / conventions | Output | Maturity |
|---------|-----------|----------|--------------------|--------|----------|
| x86-64 | `ANVIL_ARCH_X86_64` | MachineIR | SysV (Linux/BSD), Darwin, Win64 | GAS/AT&T | Linux/SysV and Win64 execution-tested; Darwin emit-validated |
| x86 (32-bit) | `ANVIL_ARCH_X86` | MachineIR | cdecl, stdcall, fastcall; ELF/Mach-O/COFF decoration | GAS/AT&T | ELF execution-tested (`as --32` / `gcc -m32`); Mach-O/COFF emit-validated only |
| ARM64 | `ANVIL_ARCH_ARM64` | MachineIR (reference) | AAPCS64 (Linux SysV), Darwin | GAS | Reference path; execution-tested |
| PPC32 | `ANVIL_ARCH_PPC32` | MachineIR | System V (ELF) | GAS | Shared `ppc_mir.c` |
| PPC64 (ELFv1) | `ANVIL_ARCH_PPC64` | MachineIR | ELFv1 (BE) | GAS | Shared `ppc_mir.c` |
| PPC64LE (ELFv2) | `ANVIL_ARCH_PPC64LE` | MachineIR | ELFv2 (LE) | GAS | Shared `ppc_mir.c` |
| S/370 | `ANVIL_ARCH_S370` | MachineIR | GCCMVS/MVS linkage | HLASM | Mainframe (`mainframe_mir`) |
| S/370-XA | `ANVIL_ARCH_S370_XA` | MachineIR | MVS linkage | HLASM | Mainframe |
| S/390 | `ANVIL_ARCH_S390` | MachineIR | MVS linkage | HLASM | Mainframe |
| z/Architecture | `ANVIL_ARCH_ZARCH` | MachineIR | MVS linkage | HLASM | Mainframe |

> **Output syntax:** All non-mainframe backends currently emit **GAS / AT&T
> syntax only**. Intel/NASM/MASM output is not exposed until an emitter
> implements it completely. Mainframe backends emit IBM HLASM.

All five MachineIR emitter families implement `i1` as a normalized 0/1 value
with one-byte memory storage. Loads and stores mask the low bit, casts (including
`FPTOUI` to `i1`) normalize explicitly, and comparisons, branches, selects,
PHIs, parameters, and returns use the GPR8 boolean contract. `FCMP` carries one
of the 16 explicit ordered/unordered predicates through MachineIR. The native
x86-64 conformance gate executes every predicate over ordered values, NaNs,
infinities, and signed zero; ARM64 and PowerPC comparison sequences are
cross-assembled, while S/370 HFP and z/Architecture BFP mnemonic/condition-code
invariants are checked separately.

```
┌─────────────────────────────────────────────────────────────────┐
│                          ANVIL IR                                │
│  (Modules, Functions, Blocks, Instructions, Values, Types)      │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Backend Interface                           │
│                   (anvil_backend_ops_t)                         │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│ x86/x86-64 BE │    │ S/370 Backend │    │ Your Backend  │
│ ARM64 / PPC   │    │  (mainframe)  │    │               │
└───────────────┘    └───────────────┘    └───────────────┘
        │                     │                     │
        ▼ (MachineIR)         ▼ (MachineIR)         ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│  GAS (AT&T)   │    │    HLASM      │    │  Your ASM     │
│  Assembly     │    │   Assembly    │    │   Format      │
└───────────────┘    └───────────────┘    └───────────────┘
```

### MachineIR Pipeline (shared)

Every backend listed above lowers source IR into target MachineIR and then runs
the shared pipeline. The per-target driver (e.g. `anvil_x86_64_regalloc_mir`)
calls into the generic passes in this order:

```
verify-legal  ->  coalesce_copies  ->  verify-legal  ->
linear-scan regalloc  ->  materialize_spills  ->  verify-legal
```

- **Legality verification** (`anvil_<arch>_verify_mir_legal`): per-target rules,
  run before coalescing, after coalescing, and once more after spill insertion.
- **Copy coalescing** (`anvil_mir_coalesce_copies`, `src/machine/machine_ir.c`).
- **Register allocation** (`anvil_regalloc_linear_scan_classes`,
  `src/machine/regalloc.c`): classic linear scan over typed virtual registers
  using per-register-class allocatable pools
  (`anvil_regalloc_class_config_t { reg_class, num_phys_regs, phys_regs }`).
  The allocator has **no call-clobber model** — it knows nothing about
  caller-saved vs callee-saved registers and does not treat call-crossing live
  ranges specially. Backends keep call-safety correct by handing the allocator
  **callee-saved-only** allocatable pools and reserving caller-saved registers
  for ABI argument/return marshaling and spill scratch.
- **Spill materialization** (`anvil_mir_materialize_spills`,
  `src/machine/machine_ir.c`): inserts `SPILL_LOAD`/`SPILL_STORE` using a
  per-target scratch-register pool; the final load/store assembly is emitted by
  the target's emitter.

### Backend Interface

```c
typedef struct {
    const char *name;              // Human-readable name
    anvil_arch_t arch;             // Architecture enum value
    
    // Initialize backend state
    anvil_error_t (*init)(anvil_backend_t *be, anvil_ctx_t *ctx);
    
    // Free backend resources
    void (*cleanup)(anvil_backend_t *be);
    
    // Clear cached IR pointers (called before module destruction)
    void (*reset)(anvil_backend_t *be);
    
    // Prepare/lower IR before code generation (optional)
    // Called automatically by anvil_module_codegen() before codegen_module()
    anvil_error_t (*prepare_ir)(anvil_backend_t *be, anvil_module_t *mod);
    
    // Generate code for entire module
    anvil_error_t (*codegen_module)(anvil_backend_t *be, anvil_module_t *mod,
                                     char **output, size_t *len);
    
    // Generate code for single function (optional)
    anvil_error_t (*codegen_func)(anvil_backend_t *be, anvil_func_t *func,
                                   char **output, size_t *len);
    
    // Return architecture information
    const anvil_arch_info_t *(*get_arch_info)(anvil_backend_t *be);
} anvil_backend_ops_t;
```

### IR Preparation Phase

The `prepare_ir` callback is called automatically before `codegen_module` to allow architecture-specific IR preparation:

```c
static anvil_error_t myarch_prepare_ir(anvil_backend_t *be, anvil_module_t *mod)
{
    myarch_backend_t *priv = be->priv;
    
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        if (!func->is_declaration) {
            // Analyze function (detect leaf functions, calculate stack layout)
            myarch_analyze_function(priv, func);
            
            // Lower unsupported operations
            // myarch_lower_unsupported_ops(priv, func);
            
            // Perform target-specific peephole optimizations
            // myarch_peephole_optimize(priv, func);
        }
    }
    
    return ANVIL_OK;
}
```

**Use cases for `prepare_ir`:**
- **Function analysis**: Detect leaf functions, calculate stack frame layout
- **IR lowering**: Convert unsupported operations to sequences of supported ones
- **Type legalization**: Split 64-bit operations on 32-bit targets
- **Peephole optimization**: Target-specific optimizations at IR level
- **Register pressure analysis**: Insert spill/reload hints

### Architecture-Specific Optimizations

Backends can implement their own optimization passes that run during `prepare_ir`. These are separate from the generic IR optimizations in `src/opt/` and can leverage architecture-specific knowledge.

**Example: ARM64 Optimization Structure**

```
src/backend/arm64/opt/
├── arm64_opt.h       # Interface declarations
├── arm64_opt.c       # Pass manager
├── arm64_peephole.c  # Peephole optimizations
├── arm64_dead_store.c # Dead store elimination
├── arm64_load_elim.c  # Redundant load elimination
├── arm64_branch.c     # Branch optimization
└── arm64_immediate.c  # Immediate optimization
```

**Pass Manager Example:**
```c
void arm64_opt_function(arm64_backend_t *be, anvil_func_t *func)
{
    /* Run optimization passes in order */
    arm64_opt_peephole(be, func);     // Local improvements
    arm64_opt_dead_store(be, func);   // Remove dead stores
    arm64_opt_load_elim(be, func);    // Eliminate redundant loads
    arm64_opt_branch(be, func);       // Optimize branches
    arm64_opt_immediate(be, func);    // Use immediate forms
}
```

**ARM64-Specific Optimizations:**
- **Peephole**: Redundant store elimination, load-store same address removal
- **Branch**: Combine `cmp`+`cset`+`cbnz` into `cmp`+`b.cond`, use `cbz`/`cbnz`/`tbz`/`tbnz`
- **Immediate**: Use `add`/`sub` immediate forms, efficient constant loading with `mov`/`movn`/`movz`

### Architecture Information

```c
typedef struct {
    anvil_arch_t arch;             // Architecture enum
    const char *name;              // Architecture name
    int ptr_size;                  // Pointer size in bytes
    int addr_bits;                 // Address bits (24, 31, 32, 64)
    int word_size;                 // Native word size
    int num_gpr;                   // General purpose registers
    int num_fpr;                   // Floating point registers
    anvil_endian_t endian;         // ANVIL_ENDIAN_LITTLE or ANVIL_ENDIAN_BIG
    anvil_stack_dir_t stack_dir;   // ANVIL_STACK_DOWN or ANVIL_STACK_UP
    bool has_condition_codes;      // Has condition code register
    bool has_delay_slots;          // Has branch delay slots
} anvil_arch_info_t;
```

## Implementing a New Backend

### Step 1: Create Backend File

Create `src/backend/<arch>/<arch>.c`:

```c
#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Register names for your architecture
static const char *myarch_reg_names[] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    // ...
};

// Register indices
#define MYARCH_R0   0
#define MYARCH_R1   1
// ...

// Backend private data
typedef struct {
    anvil_strbuf_t code;           // Output buffer
    anvil_strbuf_t data;           // Data section buffer
    int label_counter;             // For generating unique labels
    // Add your backend-specific state here
} myarch_backend_t;
```

### Step 2: Define Architecture Info

```c
static const anvil_arch_info_t myarch_arch_info = {
    .arch = ANVIL_ARCH_MYARCH,
    .name = "MyArch",
    .ptr_size = 4,                 // 4 bytes for 32-bit
    .addr_bits = 32,
    .word_size = 4,
    .num_gpr = 16,
    .num_fpr = 8,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};
```

### Step 3: Implement Init and Cleanup

```c
static anvil_error_t myarch_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)ctx;  // May be unused
    
    myarch_backend_t *priv = calloc(1, sizeof(myarch_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void myarch_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    myarch_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv);
    be->priv = NULL;
}

static void myarch_reset(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    myarch_backend_t *priv = be->priv;
    
    // Clear cached pointers to anvil_value_t
    // This prevents dangling pointers when modules are destroyed
    priv->label_counter = 0;
    // Reset any stack_slots, strings, or other cached IR pointers
}

static const anvil_arch_info_t *myarch_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &myarch_arch_info;
}
```

### Step 4: Implement Code Emission Helpers

```c
// Emit file header
static void myarch_emit_header(myarch_backend_t *be, const char *module_name)
{
    anvil_strbuf_appendf(&be->code, "; Module: %s\n", module_name);
    anvil_strbuf_append(&be->code, "; Generated by ANVIL\n\n");
    anvil_strbuf_append(&be->code, ".text\n");
}

// Emit function prologue
static void myarch_emit_prologue(myarch_backend_t *be, anvil_func_t *func)
{
    anvil_strbuf_appendf(&be->code, ".global %s\n", func->name);
    anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
    
    // Save frame pointer, allocate stack, etc.
    anvil_strbuf_append(&be->code, "    push rbp\n");
    anvil_strbuf_append(&be->code, "    mov rbp, rsp\n");
    // ...
}

// Emit function epilogue
static void myarch_emit_epilogue(myarch_backend_t *be)
{
    anvil_strbuf_append(&be->code, "    mov rsp, rbp\n");
    anvil_strbuf_append(&be->code, "    pop rbp\n");
    anvil_strbuf_append(&be->code, "    ret\n");
}

// Load a value into a register
static void myarch_emit_load_value(myarch_backend_t *be, anvil_value_t *val, int reg)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            anvil_strbuf_appendf(&be->code, "    mov %s, %lld\n",
                myarch_reg_names[reg], (long long)val->data.i);
            break;
            
        case ANVIL_VAL_PARAM:
            // Load parameter based on calling convention
            // ...
            break;
            
        case ANVIL_VAL_INSTR:
            // Result of previous instruction (may already be in a register)
            // ...
            break;
            
        // Handle other value kinds...
    }
}
```

### Step 5: Implement Instruction Emission

```c
static void myarch_emit_instr(myarch_backend_t *be, anvil_instr_t *instr)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_ADD:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            myarch_emit_load_value(be, instr->operands[1], MYARCH_R1);
            anvil_strbuf_append(&be->code, "    add R0, R1\n");
            // Result is in R0
            break;
            
        case ANVIL_OP_SUB:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            myarch_emit_load_value(be, instr->operands[1], MYARCH_R1);
            anvil_strbuf_append(&be->code, "    sub R0, R1\n");
            break;
            
        case ANVIL_OP_MUL:
            // Handle multiplication
            break;
            
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
            // Handle division
            break;
            
        case ANVIL_OP_AND:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
        case ANVIL_OP_NOT:
            // Handle bitwise operations
            break;
            
        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
            // Handle shifts
            break;
            
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
            // Handle comparisons
            break;
            
        case ANVIL_OP_LOAD:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            anvil_strbuf_append(&be->code, "    load R0, [R0]\n");
            break;
            
        case ANVIL_OP_STORE:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);  // value
            myarch_emit_load_value(be, instr->operands[1], MYARCH_R1);  // address
            anvil_strbuf_append(&be->code, "    store [R1], R0\n");
            break;
            
        case ANVIL_OP_BR:
            anvil_strbuf_appendf(&be->code, "    jmp %s\n",
                instr->true_block->name);
            break;
            
        case ANVIL_OP_BR_COND:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            anvil_strbuf_append(&be->code, "    test R0, R0\n");
            anvil_strbuf_appendf(&be->code, "    jnz %s\n",
                instr->true_block->name);
            anvil_strbuf_appendf(&be->code, "    jmp %s\n",
                instr->false_block->name);
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
                // Move to return register if needed
            }
            myarch_emit_epilogue(be);
            break;
            
        case ANVIL_OP_CALL:
            // Handle function calls
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "    ; Unimplemented: op %d\n",
                instr->op);
            break;
    }
}
```

### Step 6: Implement Block and Function Emission

```c
static void myarch_emit_block(myarch_backend_t *be, anvil_block_t *block)
{
    if (!block) return;
    
    // Emit block label
    anvil_strbuf_appendf(&be->code, "%s:\n", block->name);
    
    // Emit all instructions
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        myarch_emit_instr(be, instr);
    }
}

static void myarch_emit_func(myarch_backend_t *be, anvil_func_t *func)
{
    if (!func) return;
    
    myarch_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        myarch_emit_block(be, block);
    }
    
    anvil_strbuf_append(&be->code, "\n");
}
```

### Step 7: Implement Module Code Generation

```c
static anvil_error_t myarch_codegen_module(anvil_backend_t *be,
                                            anvil_module_t *mod,
                                            char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    myarch_backend_t *priv = be->priv;
    
    // Reset buffers
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    
    // Emit header
    myarch_emit_header(priv, mod->name);
    
    // Emit all functions
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        myarch_emit_func(priv, func);
    }
    
    // Emit data section
    if (mod->num_globals > 0) {
        anvil_strbuf_append(&priv->code, "\n.data\n");
        for (anvil_global_t *g = mod->globals; g; g = g->next) {
            anvil_strbuf_appendf(&priv->code, "%s: .long 0\n",
                g->value->name);
        }
    }
    
    // Return output
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

static anvil_error_t myarch_codegen_func(anvil_backend_t *be,
                                          anvil_func_t *func,
                                          char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    myarch_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    myarch_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}
```

### Step 8: Export Backend Operations

```c
const anvil_backend_ops_t anvil_backend_myarch = {
    .name = "MyArch",
    .arch = ANVIL_ARCH_MYARCH,
    .init = myarch_init,
    .cleanup = myarch_cleanup,
    .reset = myarch_reset,
    .codegen_module = myarch_codegen_module,
    .codegen_func = myarch_codegen_func,
    .get_arch_info = myarch_get_arch_info
};
```

### Step 9: Register Backend

Add to `include/anvil/anvil.h`:

```c
typedef enum {
    ANVIL_ARCH_UNKNOWN = 0,
    ANVIL_ARCH_X86,
    ANVIL_ARCH_X86_64,
    ANVIL_ARCH_S370,
    ANVIL_ARCH_S370_XA,
    ANVIL_ARCH_S390,
    ANVIL_ARCH_ZARCH,
    ANVIL_ARCH_PPC32,
    ANVIL_ARCH_PPC64,
    ANVIL_ARCH_PPC64LE,
    ANVIL_ARCH_ARM64,
    ANVIL_ARCH_MYARCH,      // Add your architecture (before ANVIL_ARCH_COUNT)
    ANVIL_ARCH_COUNT
} anvil_arch_t;
```

Register it in `src/core/backend.c` (backends are registered at startup via
`anvil_register_backend()` in `anvil_init_backends()`):

```c
extern const anvil_backend_ops_t anvil_backend_myarch;

/* In anvil_init_backends(): */
anvil_register_backend(&anvil_backend_x86);
anvil_register_backend(&anvil_backend_x86_64);
anvil_register_backend(&anvil_backend_s370);
anvil_register_backend(&anvil_backend_s370_xa);
anvil_register_backend(&anvil_backend_s390);
anvil_register_backend(&anvil_backend_zarch);
anvil_register_backend(&anvil_backend_ppc32);
anvil_register_backend(&anvil_backend_ppc64);
anvil_register_backend(&anvil_backend_ppc64le);
anvil_register_backend(&anvil_backend_arm64);
anvil_register_backend(&anvil_backend_myarch);  // Add your backend
```

Add your source files to the `Makefile` (a MachineIR backend is typically a thin
driver plus an `*_mir.c`):

```makefile
BACKEND_SRCS = \
    src/backend/x86/x86.c src/backend/x86/x86_mir.c \
    src/backend/x86_64/x86_64.c src/backend/x86_64/x86_64_mir.c \
    src/backend/arm64/arm64.c src/backend/arm64/arm64_mir.c \
    src/backend/myarch/myarch.c src/backend/myarch/myarch_mir.c  # Add yours
```

## Calling Conventions

### Handling Parameters

Different architectures pass parameters differently:

The x86 and x86-64 backends select their convention from `func->cc` (per-function
override) falling back to `ctx->abi` (module default); platform decoration is
selected from `ctx->abi`. See the ABI/CC descriptor tables in
`src/backend/x86_64/x86_64_mir.c` (`anvil_x64_abi_desc_t x64_abi_descs[]`) and
`src/backend/x86/x86_mir.c` (`anvil_x86_cc_desc_t x86_cc_descs[]` /
`anvil_x86_plat_desc_t x86_plat_descs[]`).

**x86 (32-bit) — cdecl / stdcall / fastcall:**
- **cdecl**: all integer args on stack, caller cleans up (`ret`).
- **stdcall**: all integer args on stack, callee cleans up (`ret $N`).
- **fastcall**: first two integer args in ECX, EDX (`x86_fastcall_int_args`),
  remainder on stack, callee cleans up.
- Integer return: EAX (high half of a 64-bit pair: EDX).
- Platform decoration: ELF (no prefix), Mach-O (`_` prefix), Win32 COFF
  (`_name@N` for stdcall, `@name@N` for fastcall, applied at call sites).

**x86-64 — SysV / Darwin / Win64 (selected via descriptor table):**
- **SysV / Darwin** integer args: RDI, RSI, RDX, RCX, R8, R9; float args:
  XMM0-XMM7; integer return RAX (high half RDX), float return XMM0. Darwin adds
  a `_` symbol prefix.
- **Win64** integer args: RCX, RDX, R8, R9; float args: XMM0-XMM3; 32-byte shadow
  space; positional arg slots (GPR/FPR share positions); integer return RAX,
  float return XMM0.

**ARM64 (AAPCS64):**
- First 8 integer args: X0-X7
- First 8 float args: D0-D7
- Return value: X0 (integer), D0 (float)
- Frame pointer: X29
- Link register: X30
- Stack pointer: SP (16-byte aligned)
- Scratch register: X16 (used for large offsets)

**ARM64 Backend Implementation Details:**

The ARM64 backend includes several important features for robust code generation:

1. **MachineIR Value Preservation**: Source IR values are lowered to typed MachineIR virtual registers. The shared allocator assigns physical registers and inserts spill loads/stores only when required.

2. **ABI Constraints**: Function parameters and returns are represented as fixed MachineIR registers (`x0`-`x7`/`d0`-`d7` and `x0`/`d0`), then copied into allocatable local vregs.

3. **Large Frame/Spill Offsets**: Uses scratch registers when an address is outside ARM64 immediate ranges:
   ```asm
   sub x16, x29, #512
   ldr x0, [x16]
   ```

4. **Very Large Stack Frames (>4095 bytes)**: Materializes the frame size before adjusting `sp`:
   ```asm
   ldr x16, =5920          ; Load large frame size
   sub sp, sp, x16         ; Allocate stack space
   ```

5. **PHI and Switch Lowering**: PHI nodes are lowered into edge copies, including parallel-copy cycle handling. `switch` terminators lower to compare/branch chains with PHI-aware edge blocks.

6. **Type-Aware Load/Store**: Correct instruction selection based on type class and size (`ldr`, `ldrsb`, `ldrsh`, `ldrsw`, `strb`, `strh`, floating-point `ldr`/`str`).

**PowerPC 32-bit (System V):**
- First 8 integer args: R3-R10
- First 8 float args: F1-F8
- Return value: R3 (integer), F1 (float)
- Frame pointer: R31
- Link register: LR (saved via mflr/mtlr)
- Stack pointer: R1 (16-byte aligned)

**PowerPC 64-bit BE (ELFv1):**
- First 8 integer args: R3-R10
- First 8 float args: F1-F8
- Return value: R3 (integer), F1 (float)
- TOC pointer: R2 (must be saved/restored)
- Frame pointer: R31
- Link register: LR
- Stack pointer: R1
- Minimum frame: 112 bytes
- Function descriptors in `.opd` section

**PowerPC 64-bit LE (ELFv2):**
- First 8 integer args: R3-R10
- First 8 float args: F1-F8
- Return value: R3 (integer), F1 (float)
- TOC pointer: R2
- Frame pointer: R31
- Link register: LR
- Stack pointer: R1
- Minimum frame: 32 bytes
- No function descriptors (uses `.localentry`)

**IBM Mainframe (MVS/GCCMVS):**
- R1 points to parameter list
- Each entry is address of parameter
- High bit of last entry is set (VL bit)
- VL bit is NOT cleared when loading parameters (allows full 31/64-bit addressing)

### GCCMVS Conventions for Mainframes

ANVIL generates code compatible with GCCMVS conventions:

| Convention | Implementation |
|------------|----------------|
| **CSECT** | Blank (no module name prefix) |
| **AMODE/RMODE** | `AMODE ANY`, `RMODE ANY` |
| **Function Names** | UPPERCASE (e.g., `FACTORIAL`, `SUM_ARRAY`) |
| **Block Labels** | `FUNCNAME$BLOCKNAME` format |
| **Stack Allocation** | Direct stack offset from R13 (no GETMAIN/FREEMAIN) |
| **VL Bit** | NOT cleared (allows full addressing) |

**Why Stack Allocation Instead of GETMAIN:**
- **Performance**: `LA R2,72(,R13)` is faster than `GETMAIN R,LV=size`
- **Simplicity**: No need to save/restore R15 around FREEMAIN in epilogue
- **Compatibility**: Matches GCCMVS output for easier interoperability

**Prologue (Stack-Based):**
```hlasm
FACTORIAL DS    0H
         STM   R14,R12,12(R13)    Save caller's registers
         LR    R12,R15            Copy entry point to base reg
         USING FACTORIAL,R12      Establish addressability
         LR    R11,R1             Save parameter list pointer
*        Set up save area chain (stack allocation)
         LA    R2,72(,R13)        R2 -> our save area
         ST    R13,4(,R2)         Chain: new->prev = caller's
         ST    R2,8(,R13)         Chain: caller->next = new
         LR    R13,R2             R13 -> our save area
```

**Epilogue (Stack-Based):**
```hlasm
*        Function epilogue
         L     R13,4(,R13)        Restore caller's SA pointer
         L     R14,12(,R13)       Restore return address
         LM    R0,R12,20(,R13)    Restore R0-R12
         BR    R14                Return to caller
```

### Handling Return Values

**x86/x86-64:**
- Integer: EAX/RAX
- Float: XMM0 or ST(0)

**ARM64:**
- Integer: X0
- Float: D0 (double) or S0 (float)

**IBM Mainframe:**
- R15 contains return value

## Register Allocation

All MachineIR backends share one allocator: a classic **linear scan** over typed
virtual registers, `anvil_regalloc_linear_scan_classes()` in
`src/machine/regalloc.c`. Each backend supplies per-register-class allocatable
pools (`anvil_regalloc_class_config_t`) plus separate scratch pools used by spill
materialization.

Key design points to keep in mind when wiring up a new backend:

- **No call-clobber model.** The allocator does not distinguish caller-saved from
  callee-saved registers and does not detect live ranges that cross calls.
  Backends therefore make the **allocatable pools callee-saved-only** so any value
  kept in a register survives calls automatically. Caller-saved registers are
  reserved for ABI argument/return marshaling during lowering and for spill
  scratch during materialization.
- **ABI registers are modeled as fixed vregs.** Incoming arguments and return
  values are pinned to the physical ABI registers via `anvil_mir_set_fixed_reg`,
  then copied into ordinary allocatable vregs (`ANVIL_MIR_OP_COPY`) before
  allocation. The allocator honors fixed assignments.
- **Spills are explicit.** `anvil_mir_materialize_spills()` rewrites the
  instruction stream with `SPILL_LOAD`/`SPILL_STORE` using the scratch pool; the
  target emitter turns those into real load/store instructions.

**Example pools (per backend):**

| Backend | Allocatable GPR | Allocatable FPR | GPR scratch | FPR scratch |
|---------|-----------------|-----------------|-------------|-------------|
| ARM64 | x19-x28 | v8-v15 | x12-x15 | v16-v19 |
| x86-64 (SysV) | rbx, r12-r14 | *(empty — all FP spilled)* | r10, r11, r15 | xmm8-xmm10 |
| x86 (32-bit) | EBX, ESI, EDI | *(empty)* | EAX, ECX, EDX | xmm5-xmm7 |

> **x86-64 FP note:** Under SysV/Darwin there are **no callee-saved XMM
> registers**, so the allocatable FPR pool is **empty** and every floating-point
> value is spilled to the stack. Win64 allocation uses its ABI descriptor's
> callee-saved `xmm6-xmm12` pool and wider GPR pool. The emitter saves and
> restores every allocated or spill-scratch nonvolatile XMM register actually
> used by the function.

## Handling Special Cases

### Division

Many architectures have special requirements for division:

**x86:**
```asm
; Signed division: EDX:EAX / operand
mov eax, dividend
cdq                 ; Sign-extend EAX into EDX
idiv divisor        ; Result in EAX, remainder in EDX
```

**S/370:**
```asm
; Division uses even-odd register pair
L     R3,dividend
SRDA  R2,32         ; Sign-extend R3 into R2:R3
D     R2,divisor    ; Quotient in R3, remainder in R2
```

### Comparisons

**x86:**
```asm
cmp eax, ebx
sete al             ; Set AL to 1 if equal
movzx eax, al       ; Zero-extend to full register
```

**ARM64:**
```asm
cmp x9, x10         ; Compare registers
cset x0, eq         ; Set X0 to 1 if equal, 0 otherwise
```

**S/370:**
```asm
CR    R2,R3         ; Compare registers
LA    R15,1         ; Assume true
BE    *+6           ; Skip if equal
SR    R15,R15       ; Set false
```

### Function Calls

**x86-64:**
```asm
; Call printf("Hello %d", 42)
lea rdi, [rel fmt]  ; First arg: format string
mov esi, 42         ; Second arg: integer
xor eax, eax        ; No vector args
call printf
```

**ARM64 (Linux):**
```asm
// Call printf("Hello %d", 42) - non-variadic style
adrp x0, fmt            // Load page address of fmt
add x0, x0, :lo12:fmt   // Add page offset
mov w1, #42             // Second arg: integer
bl printf               // Branch with link
```

**ARM64 (Darwin/macOS) - Variadic Functions:**
```asm
// Call printf("Hello %d", 42) - variadic args on stack
sub sp, sp, #16         // Allocate stack for variadic args
adrp x9, fmt@PAGE       // Load page address of fmt
add x9, x9, fmt@PAGEOFF // Add page offset
mov x0, x9              // First arg (fixed): format string
mov x9, #42             // Load variadic arg
str x9, [sp, #0]        // Store variadic arg on stack
bl _printf              // Branch with link (underscore prefix)
add sp, sp, #16         // Restore stack
```

**Note:** On Darwin/macOS ARM64, variadic function arguments (those after the fixed parameters) must be passed on the stack, not in registers. This is a key difference from Linux AAPCS64 where all arguments can go in registers.

**S/370:**
```asm
; Build parameter list in dynamic area
ST    R0,72(,R13)   ; Store first param address
ST    R1,76(,R13)   ; Store second param address
OI    76(R13),X'80' ; Mark last param
LA    R1,72(,R13)   ; R1 -> param list
L     R15,=V(FUNC)  ; Load function address
BALR  R14,R15       ; Call
```

## Testing Your Backend

### Unit Tests

Create test cases for each instruction type:

```c
void test_add(void) {
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_MYARCH);
    
    anvil_module_t *mod = anvil_module_create(ctx, "test");
    // Create add function...
    
    char *output = NULL;
    anvil_module_codegen(mod, &output, NULL);
    
    // Verify output contains expected instructions
    assert(strstr(output, "add") != NULL);
    
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}
```

### Integration Tests

Test complete programs:

```c
void test_factorial(void) {
    // Generate factorial function
    // Assemble output
    // Link and run
    // Verify result
}
```

## ARM64 Backend Implementation Notes

The ARM64 backend has several important implementation details:

### MachineIR Value Preservation

ARM64 is the reference MachineIR backend. It does not use the old
stack-slot-for-every-SSA-value strategy. Instead, lowering creates typed virtual
registers, marks ABI registers as fixed where needed, runs shared copy
coalescing and linear-scan allocation, and materializes spills only for values
that cannot stay in physical registers.

### Large Stack Frame Support

ARM64 addressing has immediate range limits. For larger local, spill, incoming
argument, or memory offsets, the emitter materializes an address through a
scratch register:

```c
ldr x16, =8192
sub x16, x29, x16
ldr x19, [x16]
```

### External Function Calls

Direct calls carry a symbol in MachineIR and emit `bl`. Indirect calls carry a
target register; ARM64 legalizes that target to fixed `x16` and emits `blr x16`:

```asm
bl puts
blr x16
```

### Type-Aware Load/Store

Use correct instruction variants based on type size:

| Type | Load | Store |
|------|------|-------|
| i8/u8 | `ldrb w0` | `strb w9` |
| i16/u16 | `ldrh w0` | `strh w9` |
| i32/u32/f32 | `ldr w0` | `str w9` |
| i64/u64/f64/ptr | `ldr x0` | `str x9` |

### Parameter Lowering

Incoming register arguments are modeled as fixed MachineIR vregs and copied into
normal local vregs before allocation. Additional arguments are represented with
`ANVIL_MIR_OP_INCOMING_STACK_ARG`. Darwin variadic arguments after the fixed
parameter list are forced to outgoing stack slots during call lowering.

### Array Stack Allocation

When allocating arrays via `ANVIL_OP_ALLOCA`, the backend must calculate the correct size based on the pointee type:

```c
static int arm64_type_size(anvil_type_t *type)
{
    if (!type) return 8;
    
    switch (type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return 1;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return 2;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_F32:
            return 4;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F64:
        case ANVIL_TYPE_PTR:
            return 8;
        case ANVIL_TYPE_ARRAY:
            return type->data.array.count * arm64_type_size(type->data.array.elem);
        case ANVIL_TYPE_STRUCT:
            /* Sum of field sizes */
            int size = 0;
            for (size_t i = 0; i < type->data.struc.num_fields; i++) {
                size += arm64_type_size(type->data.struc.fields[i]);
            }
            return size > 0 ? size : 8;
        default:
            return 8;
    }
}

/* In arm64_emit_func first pass: */
if (instr->op == ANVIL_OP_ALLOCA) {
    int size = 8;
    if (instr->result && instr->result->type && 
        instr->result->type->kind == ANVIL_TYPE_PTR &&
        instr->result->type->data.pointee) {
        size = arm64_type_size(instr->result->type->data.pointee);
    }
    arm64_add_stack_slot_sized(be, instr->result, size);
}
```

### Global Variable Access (macOS vs Linux)

The backend must use different relocation syntax for global variables:

**Linux (ELF):**
```asm
adrp x9, counter
ldr w0, [x9, :lo12:counter]
str w9, [x10, :lo12:counter]
```

**macOS (Mach-O):**
```asm
adrp x9, _counter@PAGE
ldr w0, [x9, _counter@PAGEOFF]
str w9, [x10, _counter@PAGEOFF]
```

Implementation:
```c
if (arm64_is_darwin(be)) {
    emit("\tadrp x9, %s%s@PAGE\n", prefix, name);
    emit("\tldr w0, [x9, %s%s@PAGEOFF]\n", prefix, name);
} else {
    emit("\tadrp x9, %s\n", name);
    emit("\tldr w0, [x9, :lo12:%s]\n", name);
}
```

## x86 / x86-64 Backend Implementation Notes

Both x86 backends follow the ARM64 MachineIR contract
(`anvil_<arch>_lower_func_to_mir` -> `..._verify_mir_legal` ->
`..._regalloc_mir` -> `..._emit_mir_abi`). Implementation-specific details:

### Multi-ABI selection (x86-64)

The descriptor struct `anvil_x64_abi_desc_t` and table `x64_abi_descs[]` in
`x86_64_mir.c` hold one entry per ABI (SysV, Darwin, Win64) with argument
register sequences, return registers, shadow-space size, symbol prefix, and
register pools. `x64_lower_abi()` resolves the active ABI: `func->cc`
(`ANVIL_CC_WIN64` / `ANVIL_CC_SYSV`) overrides `ctx->abi`, defaulting to SysV.

### Multi-convention selection (x86 32-bit)

`x86_cc_descs[]` covers cdecl, stdcall, and fastcall; `x86_plat_descs[]` covers
ELF / Mach-O / Win32 COFF decoration. The emitter consumes `func->cc` for the
convention (register args, callee `ret $N` cleanup) and `ctx->abi` for platform
decoration. Stack cleanup bytes are computed by `x86_compute_ret_pop()`.

### 64-bit integers on x86 (32-bit)

`i64`/`u64` are legalized into **lo/hi register pairs** (little-endian).
Supported operations on pairs are limited to constant materialization, compares
(`lower_i64_cmp_pair`), bitwise AND/OR/XOR (`lower_i64_bitwise_pair`),
load/store, and parameter/return passing. **Runtime 64-bit ADD, SUB, MUL, and
shifts are not supported** — lowering of those returns failure (the same posture
as the PPC32 backend). Constant i64 NEG/NOT are folded at lowering time.

### Byte registers on x86 (32-bit)

ESI and EDI have no 8-bit sub-register on 32-bit x86 (only EAX/ECX/EDX/EBX do,
per `x86_reg_has_byte()`). Byte stores and byte casts of a value living in
ESI/EDI are routed through a byte-addressable scratch register (`movl` into
EAX/ECX, then use `al`/`cl`). Byte/word loads use `movzbl`/`movsbl` writing a
full 32-bit destination and need no fixup.

### Known limitations (x86 / x86-64)

- **GAS / AT&T output only.** NASM/Intel output is not implemented or exposed
  as a selectable syntax.
- **`u64 -> floating point` is simplified.** `UITOFP` uses the signed
  `cvtsi2ss/cvtsi2sd` path, treating the unsigned source as signed (no full
  unsigned-64 fixup).
- **struct-by-value is limited.** Aggregates are handled by address (GEP /
  struct-GEP); there is no System V field-classification of structs into
  registers. Stack arguments use fixed 8-byte (x86-64) / 4-byte-rounded (x86)
  slots.
- **Win64 is execution-tested bidirectionally** with a MinGW C shim under Wine:
  C calls Anvil-generated code, Anvil calls a C variadic function, and the gate
  checks stack arguments, shadow space and nonvolatile XMM preservation. Darwin
  remains emit-validated on the Linux host. SysV/Linux x86-64 and ELF x86-32 are
  also execution-tested paths.

### Validation

x86-64 passes the `basic_runtime` execution test and the `mcc` exec suite, and
both backends have MachineIR lowering regression tests
(`tests/x86_64_mir_lowering_regression.c`, `tests/x86_mir_lowering_regression.c`).
x86-32 output is execution-validated by assembling with `as --32` and linking
with `gcc -m32`.

## Debugging Tips

1. **Print IR**: Add debug output to see what IR you're processing
2. **Check Operand Types**: Many bugs come from type mismatches
3. **Verify Offsets**: Stack and memory offsets are common error sources
4. **Test Incrementally**: Start with simple functions, add complexity gradually
5. **Compare with Reference**: Generate code with a known compiler and compare

## Performance Considerations

1. **Minimize Memory Operations**: Keep values in registers when possible
2. **Use Efficient Instructions**: Some operations have faster alternatives
3. **Align Data**: Misaligned access is slow on most architectures
4. **Branch Prediction**: Arrange code to help branch predictors
5. **Instruction Scheduling**: Order instructions to avoid pipeline stalls

## Platform-Specific Code Generation

### OS ABI Support

ANVIL supports multiple OS ABIs through the `anvil_ctx_set_abi()` function. This affects:

- **Symbol naming**: Darwin uses underscore prefix (`_main` vs `main`)
- **Assembly directives**: `.type` (ELF) vs `.p2align` (Mach-O)
- **Section names**: `.text` (ELF) vs `__TEXT,__text` (Mach-O)
- **Relocation syntax**: `:lo12:` (ELF) vs `@PAGE/@PAGEOFF` (Mach-O)

### ARM64 macOS (Apple Silicon) Example

```c
anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_ARM64);
anvil_ctx_set_abi(ctx, ANVIL_ABI_DARWIN);  // Enable macOS mode

// ... build IR ...

// Generated code will have:
// - Underscore prefix on symbols (_main, _printf)
// - Mach-O section directives
// - @PAGE/@PAGEOFF relocations
```

**Linux ARM64:**
```asm
        .globl main
        .type main, %function
main:
        adrp x0, .LC0
        add x0, x0, :lo12:.LC0
        bl printf
        .size main, .-main
```

**macOS ARM64:**
```asm
        .globl _main
        .p2align 2
_main:
        adrp x0, L_.str@PAGE
        add x0, x0, L_.str@PAGEOFF
        bl _printf
```

### Implementing ABI Support in Backends

When implementing a backend that needs ABI-specific code:

```c
static void emit_function(my_backend_t *be, anvil_func_t *func)
{
    bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
    const char *prefix = is_darwin ? "_" : "";
    
    if (is_darwin) {
        // Mach-O directives
        emit(".globl %s%s\n", prefix, func->name);
        emit(".p2align 2\n");
    } else {
        // ELF directives
        emit(".globl %s\n", func->name);
        emit(".type %s, %%function\n", func->name);
    }
    
    emit("%s%s:\n", prefix, func->name);
    // ... emit function body ...
}
```
