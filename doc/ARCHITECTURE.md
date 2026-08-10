# ANVIL Internal Architecture

This document describes the internal architecture of the ANVIL library.

## Overview

ANVIL follows a classic compiler backend design with these main components.
User code builds source IR; the IR is verified and optimized, then each backend
lowers it through a shared, target-independent **MachineIR** layer (virtual
registers, fixed/ABI registers, frame/spill slots) before register allocation,
spill materialization, and assembly emission.

```
┌─────────────────────────────────────────────────────────────────────┐
│                           User Code                                  │
│         (Creates IR using anvil_build_* functions)                  │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         Context (anvil_ctx_t)                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐                  │
│  │ Type Cache  │  │  Backend    │  │   Insert    │                  │
│  │             │  │  Instance   │  │   Point     │                  │
│  └─────────────┘  └─────────────┘  └─────────────┘                  │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         Module (anvil_module_t)                      │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    Functions List                            │    │
│  │  ┌──────────────────────────────────────────────────────┐   │    │
│  │  │              Function (anvil_func_t)                  │   │    │
│  │  │  ┌────────────────────────────────────────────────┐  │   │    │
│  │  │  │           Blocks List                          │  │   │    │
│  │  │  │  ┌──────────────────────────────────────────┐ │  │   │    │
│  │  │  │  │        Block (anvil_block_t)             │ │  │   │    │
│  │  │  │  │  ┌────────────────────────────────────┐  │ │  │   │    │
│  │  │  │  │  │   Instructions (anvil_instr_t)     │  │ │  │   │    │
│  │  │  │  │  │   [instr] -> [instr] -> [instr]    │  │ │  │   │    │
│  │  │  │  │  └────────────────────────────────────┘  │ │  │   │    │
│  │  │  │  └──────────────────────────────────────────┘ │  │   │    │
│  │  │  └────────────────────────────────────────────────┘  │   │    │
│  │  └──────────────────────────────────────────────────────┘   │    │
│  └─────────────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    Globals List                              │    │
│  └─────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                  Verifier (src/core/verify.c)                        │
│         anvil_module_verify() validates source IR before codegen     │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                   Pass Manager (anvil_pass_manager_t)                │
│                                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐            │
│  │ConstFold │  │   DCE    │  │ Strength │  │SimplifyCFG│            │
│  └──────────┘  └──────────┘  └──────────┘  └──────────┘            │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Backend (anvil_backend_t)                       │
│            prepare_ir (lower) → codegen_module (emit)                │
│                                                                      │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  │
│  │ x86  │ │x86-64│ │S/370 │ │S/390 │ │z/Arch│ │ PPC  │ │ARM64 │  │
│  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘  │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│        MachineIR (src/machine: vregs, fixed/ABI regs, frame/spill)  │
│        linear-scan regalloc → copy coalescing → spill materialize    │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Assembly Output (Text)                          │
└─────────────────────────────────────────────────────────────────────┘
```

### Compilation Pipeline

`anvil_module_codegen()` (in `src/core/module.c`) drives the end-to-end flow:

```
source IR build                       (anvil_build_* functions)
    → verify        (src/core/verify.c: anvil_module_verify)
    → optimize      (src/opt: anvil_module_optimize / pass manager)
    → backend prepare_ir / lower      (source IR → MachineIR)
    → register allocation             (src/machine: linear-scan)
    → spill materialization + copy coalescing
    → emit          (codegen_module → target assembly text)
```

## Core Data Structures

### Context (anvil_ctx_t)

The context is the root object that owns all resources:

```c
struct anvil_ctx {
    anvil_arch_t target;           // Target architecture
    anvil_backend_t *backend;      // Active backend instance
    anvil_block_t *insert_block;   // Current insertion point
    anvil_type_t *type_cache[...]; // Cached primitive types
    anvil_pool_t *pool;            // Memory pool for allocations
    
    // CPU Model System
    anvil_cpu_model_t cpu_model;       // Selected CPU model
    anvil_cpu_features_t cpu_features; // Active CPU features
    anvil_cpu_features_t features_enabled;  // Manually enabled features
    anvil_cpu_features_t features_disabled; // Manually disabled features
};
```

**Responsibilities:**
- Manages target architecture selection
- Holds the active backend instance
- Tracks the current insertion point for IR building
- Caches primitive types to avoid duplication
- Provides memory pool for efficient allocation
- **CPU Model System**: Tracks selected CPU model and feature flags for target-specific code generation

### Module (anvil_module_t)

A module represents a compilation unit:

```c
struct anvil_module {
    anvil_ctx_t *ctx;              // Parent context
    char *name;                    // Module name
    anvil_func_t *funcs;           // Linked list of functions
    size_t num_funcs;              // Function count
    anvil_global_t *globals;       // Linked list of globals
    size_t num_globals;            // Global count
};
```

**Responsibilities:**
- Contains all functions and global variables
- Provides module-level code generation
- Manages symbol visibility

### Function (anvil_func_t)

A function contains basic blocks:

```c
struct anvil_func {
    anvil_module_t *mod;           // Parent module
    char *name;                    // Function name
    anvil_type_t *type;            // Function type
    anvil_linkage_t linkage;       // Linkage type
    anvil_block_t *blocks;         // Linked list of blocks
    anvil_block_t *entry;          // Entry block
    anvil_value_t **params;        // Parameter values
    size_t num_params;             // Parameter count
    size_t stack_size;             // Calculated stack frame size
    anvil_func_t *next;            // Next function in module
};
```

**Responsibilities:**
- Contains all basic blocks
- Manages function parameters
- Tracks stack frame requirements

### Basic Block (anvil_block_t)

A basic block is a sequence of instructions:

```c
struct anvil_block {
    anvil_func_t *func;            // Parent function
    char *name;                    // Block name (label)
    anvil_instr_t *first;          // First instruction
    anvil_instr_t *last;           // Last instruction
    anvil_block_t *next;           // Next block in function
};
```

**Responsibilities:**
- Contains instructions in execution order
- Has single entry point (from top)
- Has single exit point (terminator instruction)

### Instruction (anvil_instr_t)

An instruction represents an operation:

```c
struct anvil_instr {
    anvil_op_t op;                 // Operation type
    anvil_value_t *result;         // Result value (if any)
    anvil_value_t **operands;      // Operand values
    size_t num_operands;           // Operand count
    anvil_block_t *true_block;     // Branch target (for br/br_cond)
    anvil_block_t *false_block;    // False branch target (for br_cond)
    anvil_instr_t *prev;           // Previous instruction
    anvil_instr_t *next;           // Next instruction
};
```

**Responsibilities:**
- Represents a single IR operation
- Links to operands and result
- Forms doubly-linked list within block

### Value (anvil_value_t)

A value represents an SSA value:

```c
struct anvil_value {
    anvil_value_kind_t kind;       // Value kind
    anvil_type_t *type;            // Value type
    char *name;                    // Value name (optional)
    union {
        int64_t i;                 // Integer constant
        double f;                  // Float constant
        char *str;                 // String constant
        struct {                   // Parameter info
            anvil_func_t *func;
            size_t index;
        } param;
        anvil_instr_t *instr;      // Instruction result
        anvil_global_t *global;    // Global variable
    } data;
};
```

**Value Kinds:**
- `ANVIL_VAL_CONST_INT` - Integer constant
- `ANVIL_VAL_CONST_FLOAT` - Float constant
- `ANVIL_VAL_CONST_NULL` - Null pointer
- `ANVIL_VAL_CONST_STRING` - String constant
- `ANVIL_VAL_PARAM` - Function parameter
- `ANVIL_VAL_INSTR` - Instruction result
- `ANVIL_VAL_GLOBAL` - Global variable
- `ANVIL_VAL_FUNC` - Function reference

### Type (anvil_type_t)

A type describes the shape of values:

```c
struct anvil_type {
    anvil_type_kind_t kind;        // Type kind
    size_t size;                   // Size in bytes
    size_t align;                  // Alignment in bytes
    bool is_signed;                // For integers
    union {
        anvil_type_t *pointee;     // For pointers
        struct {                   // For arrays
            anvil_type_t *elem;
            size_t count;
        } array;
        struct {                   // For structs
            char *name;
            anvil_type_t **fields;
            size_t num_fields;
        } struc;
        struct {                   // For functions
            anvil_type_t *ret;
            anvil_type_t **params;
            size_t num_params;
            bool variadic;
        } func;
    } data;
};
```

## Memory Management

### Allocation Strategy

ANVIL uses a simple allocation strategy:

1. **Context-owned resources**: Types are cached in the context
2. **Module-owned resources**: Functions, blocks, instructions
3. **Explicit cleanup**: User must call destroy functions

```c
// Ownership hierarchy:
// Context
//   └── Type cache (freed with context)
//   └── Backend (freed with context)
// Module
//   └── Functions (freed with module)
//       └── Blocks (freed with function)
//           └── Instructions (freed with block)
//   └── Globals (freed with module)
```

### String Buffer (anvil_strbuf_t)

Backends use a string buffer for code generation:

```c
typedef struct {
    char *data;                    // Buffer data
    size_t len;                    // Current length
    size_t cap;                    // Capacity
} anvil_strbuf_t;

void anvil_strbuf_init(anvil_strbuf_t *sb);
void anvil_strbuf_destroy(anvil_strbuf_t *sb);
void anvil_strbuf_append(anvil_strbuf_t *sb, const char *str);
void anvil_strbuf_appendf(anvil_strbuf_t *sb, const char *fmt, ...);
char *anvil_strbuf_detach(anvil_strbuf_t *sb, size_t *len);
```

## Backend System

ANVIL has a single, unified backend path. Every in-tree backend lowers source
IR into the shared, target-independent **MachineIR** (`src/machine`), runs
linear-scan register allocation and spill materialization, then emits assembly
text. The architecture-specific details (calling conventions, register sets,
syntax) live in `BACKENDS.md` and the per-architecture docs.

### Backend Interface

Each backend implements the `anvil_backend_ops_t` interface (defined in
`include/anvil/anvil.h`):

```c
typedef struct anvil_backend_ops {
    const char *name;              // Backend name
    anvil_arch_t arch;             // Target architecture

    // Initialize backend
    anvil_error_t (*init)(anvil_backend_t *be, anvil_ctx_t *ctx);

    // Cleanup backend
    void (*cleanup)(anvil_backend_t *be);

    // Reset backend state (clear cached pointers to IR values)
    void (*reset)(anvil_backend_t *be);

    // Prepare/lower IR for code generation (optional). Called before
    // codegen_module for target-specific lowering, legalization, and the
    // MachineIR lowering + register allocation step.
    anvil_error_t (*prepare_ir)(anvil_backend_t *be, anvil_module_t *mod);

    // Generate code for entire module
    anvil_error_t (*codegen_module)(anvil_backend_t *be, anvil_module_t *mod,
                                     char **output, size_t *len);

    // Generate code for single function
    anvil_error_t (*codegen_func)(anvil_backend_t *be, anvil_func_t *func,
                                   char **output, size_t *len);

    // Get architecture info
    const anvil_arch_info_t *(*get_arch_info)(anvil_backend_t *be);

    // Backend-private static data
    void *priv;
} anvil_backend_ops_t;
```

### Backend Instance

```c
struct anvil_backend {
    const anvil_backend_ops_t *ops; // Backend operations
    anvil_ctx_t *ctx;               // Parent context
    anvil_syntax_t syntax;          // Selected assembly syntax
    void *priv;                     // Backend-private instance data
};
```

### Backend Registration

Built-in backends are registered once at first use via `pthread_once`
(`src/core/backend.c`):

```c
static void register_builtin_backends(void) {
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
}
```

`anvil_register_backend()` appends to a registry (guarded by a mutex, so
out-of-tree backends can be added). `anvil_get_backend(ctx, arch)` looks up the
ops for an architecture and constructs an initialized `anvil_backend_t`
instance.

## Code Generation Flow

### Module Code Generation

`anvil_module_codegen()` verifies and optimizes the module, then runs the
backend's two-stage path (`prepare_ir`/lower followed by `codegen_module`):

```
anvil_module_codegen(mod, &output, &len)
    │
    ├── anvil_module_verify(mod, ...)         (src/core/verify.c)
    │       └── reject malformed source IR before any codegen
    │
    ├── anvil_module_optimize(mod)            (src/opt pass manager)
    │
    ├── backend->ops->prepare_ir(be, mod)     (optional, per backend)
    │       └── lower source IR → MachineIR; legalize; regalloc; spill
    │
    └── backend->ops->codegen_module(be, mod, &output, &len)
            │
            ├── Emit module header (CSECT, directives, .text, etc.)
            │
            ├── For each function:
            │       ├── Emit prologue (frame size from MachineIR slots)
            │       ├── For each MachineIR block: emit label + instructions
            │       │     using the register-allocator's assignments
            │       └── Emit epilogue
            │
            ├── Emit data section (globals, string literals, save areas)
            │
            └── Emit footer (LTORG, END, etc.)
```

### MachineIR Lowering and Emission

All in-tree backends share the MachineIR infrastructure in `src/machine`
(`machine_ir.c`, `regalloc.c`; public API in
`include/anvil/anvil_machine.h`). The backend-specific lowering file
(`*_mir.c`) drives the per-function pipeline:

```
For each source function:
    1. Lower source IR ops to MachineIR ops over virtual registers
       (anvil_mir_add_instr*, frame slots, string literals).
    2. Pin ABI/fixed registers (params, returns, call args/results)
       with anvil_mir_set_fixed_reg.
    3. anvil_mir_coalesce_copies   - remove redundant copies.
    4. anvil_regalloc_linear_scan_classes - per-class linear-scan
       allocation (GPR/FPR), producing phys-reg or spill-slot assignments.
    5. anvil_mir_materialize_spills - insert spill load/store using
       scratch registers reserved per class.
    6. Walk allocated MachineIR and emit target assembly text.
```

The vreg/op model is target-independent: `anvil_mir_vreg_info_t` carries a
register class (GPR/FPR/FLAGS/SPECIAL), bit width, signedness, and an optional
fixed physical register; instructions are SSA-like `(opcode, def, uses[],
imm/symbol)` tuples. This is why adding a backend is largely a matter of
writing a lowering + emission `*_mir.c` plus a target/ABI descriptor.

## SSA Form

ANVIL uses Static Single Assignment (SSA) form:

1. Each value is defined exactly once
2. PHI nodes merge values from different control flow paths
3. Values are immutable after creation

**Example:**

```c
// Source: if (x > 0) y = 1; else y = 2; return y;

// SSA form:
entry:
    %cmp = cmp_gt %x, 0
    br_cond %cmp, then, else

then:
    br merge

else:
    br merge

merge:
    %y = phi [1, then], [2, else]
    ret %y
```

## File Organization

```
src/
├── core/
│   ├── context.c      # Context management, target/CPU/ABI selection
│   ├── types.c        # Type system
│   ├── module.c       # Module management + codegen pipeline driver
│   ├── function.c     # Function management
│   ├── value.c        # Value/instruction creation
│   ├── builder.c      # IR builder
│   ├── verify.c       # Source IR verifier (run before codegen)
│   ├── ir_dump.c      # IR debug/dump (anvil_print_module, etc.)
│   ├── cpu_table.c    # CPU model / feature tables
│   ├── strbuf.c       # String buffer utilities
│   └── backend.c      # Backend registry
│
├── machine/           # Target-independent MachineIR + register allocation
│   ├── machine_ir.c   # MachineIR container (vregs, blocks, slots, coalesce)
│   ├── regalloc.c     # Linear-scan allocation + spill materialization
│   └── machine_internal.h
│
├── opt/               # Optimization passes (see OPTIMIZATION.md)
│   ├── opt.c          # Pass manager
│   ├── const_fold.c, dce.c, copy_prop.c, cse.c,
│   ├── dead_store.c, load_elim.c, store_load_prop.c,
│   ├── simplify_cfg.c, strength_reduce.c, ctx_opt.c
│
└── backend/
    ├── common/
    │   └── anvil_slot_map.h    # shared slot-mapping helper
    ├── x86/                    # x86 32-bit (MachineIR-based)
    │   ├── x86.c               # backend ops / lifecycle
    │   ├── x86_helpers.c, x86_internal.h
    │   └── x86_mir.c           # source IR → MachineIR lowering + emit
    ├── x86_64/                 # x86-64 (MachineIR-based)
    │   ├── x86_64.c
    │   ├── x86_64_helpers.c, x86_64_internal.h
    │   └── x86_64_mir.c        # source IR → MachineIR lowering + emit
    ├── s370/    s370.c         # IBM S/370 dispatcher → mainframe_mir
    ├── s370_xa/ s370_xa.c      # IBM S/370-XA dispatcher → mainframe_mir
    ├── s390/    s390.c         # IBM S/390 dispatcher → mainframe_mir
    ├── zarch/   zarch.c        # IBM z/Architecture dispatcher → mainframe_mir
    ├── ppc32/   ppc32.c        # PowerPC 32-bit dispatcher → ppc_mir
    ├── ppc64/   ppc64.c        # PowerPC 64-bit BE dispatcher → ppc_mir
    ├── ppc64le/ ppc64le.c      # PowerPC 64-bit LE dispatcher → ppc_mir
    ├── ppc/
    │   └── ppc_mir.c           # shared PowerPC MachineIR backend
    ├── mainframe/
    │   └── mainframe_mir.c     # shared IBM mainframe MachineIR backend
    └── arm64/
        ├── arm64.c, arm64_helpers.c, arm64_internal.h
        ├── arm64_mir.c         # reference MachineIR backend
        └── opt/                # ARM64 peephole / branch opt passes
```

The thin per-architecture `*.c` files (e.g. `s370.c`, `ppc32.c`, `x86.c`)
provide the `anvil_backend_ops_t` and delegate the real lowering/emission to the
shared or per-target `*_mir.c` translation units.

## Supported Architectures

ANVIL supports the following target architectures:

| Architecture | Enum | Bits | Endian | Stack | FP Format | ABI |
|--------------|------|------|--------|-------|-----------|-----|
| x86 | `ANVIL_ARCH_X86` | 32 | Little | Down | IEEE 754 | CDECL |
| x86-64 | `ANVIL_ARCH_X86_64` | 64 | Little | Down | IEEE 754 | System V / Win64 |
| S/370 | `ANVIL_ARCH_S370` | 24 | Big | Up | HFP | MVS Linkage |
| S/370-XA | `ANVIL_ARCH_S370_XA` | 31 | Big | Up | HFP | MVS Linkage |
| S/390 | `ANVIL_ARCH_S390` | 31 | Big | Up | HFP/IEEE | MVS Linkage |
| z/Architecture | `ANVIL_ARCH_ZARCH` | 64 | Big | Up | HFP/IEEE | MVS Linkage |
| PowerPC 32 | `ANVIL_ARCH_PPC32` | 32 | Big | Down | IEEE 754 | System V |
| PowerPC 64 | `ANVIL_ARCH_PPC64` | 64 | Big | Down | IEEE 754 | ELFv1 |
| PowerPC 64 LE | `ANVIL_ARCH_PPC64LE` | 64 | Little | Down | IEEE 754 | ELFv2 |
| ARM64 | `ANVIL_ARCH_ARM64` | 64 | Little | Down | IEEE 754 | AAPCS64 |

### Architecture-Specific Features

**x86/x86-64:**
- Rewritten to lower through the shared MachineIR/regalloc path (no longer
  legacy direct source-IR emitters); see `x86_mir.c` / `x86_64_mir.c`
- x86 (32-bit): cdecl, stdcall, and fastcall calling conventions; 64-bit
  integers legalized into lo/hi register pairs
- x86-64: System V and Windows x64 ABIs via target/ABI descriptors
- See `BACKENDS.md` for register sets, syntax, and ABI specifics

**IBM Mainframe (S/370, S/390, z/Architecture):**
- HLASM syntax output
- Shared MachineIR backend with target descriptors for S/370, S/370-XA, S/390, and z/Architecture
- GCCMVS compatibility mode
- Hexadecimal Floating Point (HFP) and IEEE 754 support
- Chained save areas for stack management
- Uppercase symbol names

**PowerPC:**
- Big-endian (PPC32, PPC64) and little-endian (PPC64LE) variants
- Shared MachineIR backend with PPC32/PPC64/PPC64LE target descriptors
- ELFv1 ABI (PPC64 BE): Function descriptors in `.opd` section, 112-byte minimum frame
- ELFv2 ABI (PPC64 LE): Local entry points via `.localentry`, 32-byte minimum frame
- System V ABI (PPC32): Standard 32-bit calling convention
- Full IEEE 754 floating-point support
- GAS syntax output
- Register allocation and spill materialization through shared MachineIR

**ARM64:**
- Stable reference implementation for the MachineIR/regalloc backend path
- AAPCS64 calling convention
- Linux (ELF) and macOS (Darwin/Mach-O) support
- Different symbol naming conventions per OS

## Design Decisions

### Why Text Assembly Output?

1. **Debuggability**: Easy to inspect and verify generated code
2. **Portability**: Works with any assembler
3. **Simplicity**: No need to handle object file formats
4. **Flexibility**: User can post-process or modify output

### Why SSA Form?

1. **Simplicity**: Each value defined once, easy to track
2. **Optimization**: Enables many compiler optimizations
3. **Register Allocation**: Simplifies liveness analysis
4. **Industry Standard**: Similar to LLVM IR

### Why a Shared MachineIR Path?

1. **Reuse**: Register allocation, spilling, and copy coalescing are written
   once in `src/machine` and shared by every backend
2. **Consistency**: All targets (x86, x86-64, PPC, mainframe, ARM64) follow the
   same lower → allocate → emit flow
3. **Extensibility**: A new architecture is mostly a lowering + emission
   `*_mir.c` plus a target/ABI descriptor
4. **Specialization**: Per-target descriptors and emission still let each
   backend honor its own ABI, syntax, and instruction set

## Optimization Infrastructure

ANVIL includes a pass manager for IR optimization.

### Pass Manager (anvil_pass_manager_t)

```c
struct anvil_pass_manager {
    anvil_ctx_t *ctx;
    anvil_opt_level_t level;
    bool enabled[ANVIL_PASS_COUNT];
    anvil_pass_info_t *custom_passes;
    size_t num_custom;
};
```

**Responsibilities:**
- Manages optimization passes
- Controls which passes are enabled
- Runs passes in correct order
- Supports custom pass registration

### Built-in Passes

| Pass | Description | Min Level |
|------|-------------|-----------|
| Constant Folding | Evaluate constant expressions | O1 |
| DCE | Remove unused instructions | O1 |
| Copy Propagation | Replace uses of copied values | O1 |
| CFG Simplification | Merge blocks, remove unreachable code, preserve `switch` edges | O2 |
| Strength Reduction | Replace expensive ops | O2 |
| Dead Store Elimination | Remove overwritten stores | O2 |
| Load Elimination | Reuse loaded values | O2 |
| CSE | Common subexpression elimination | O2 |

### Pass Execution Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                         IR (Before)                                  │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
                    ┌─────────────────────────┐
                    │    Constant Folding     │
                    └─────────────────────────┘
                                  │
                                  ▼
                    ┌─────────────────────────┐
                    │         DCE             │
                    └─────────────────────────┘
                                  │
                                  ▼
                    ┌─────────────────────────┐
                    │   CFG Simplification    │
                    └─────────────────────────┘
                                  │
                                  ▼
                    ┌─────────────────────────┐
                    │   Strength Reduction    │
                    └─────────────────────────┘
                                  │
                                  ▼
                    ┌─────────────────────────┐
                    │    Custom Passes        │
                    └─────────────────────────┘
                                  │
                    ┌─────────────────────────┐
                    │   Changed? Loop back    │◄──┐
                    └─────────────────────────┘   │
                                  │ yes           │
                                  └───────────────┘
                                  │ no
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                         IR (After)                                   │
└─────────────────────────────────────────────────────────────────────┘
```

### Source Files

| File | Description |
|------|-------------|
| `include/anvil/anvil_opt.h` | Public optimization API |
| `src/opt/opt.c` | Pass manager implementation |
| `src/opt/const_fold.c` | Constant folding pass |
| `src/opt/dce.c` | Dead code elimination |
| `src/opt/simplify_cfg.c` | CFG simplification, including `switch` reachability and target rewrites |
| `src/opt/strength_reduce.c` | Strength reduction |
| `src/opt/ctx_opt.c` | Context integration |

## Build Environment

ANVIL builds with a C11 toolchain. The top-level `Makefile` compiles with:

```
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -I./include -g -O2
```

`-D_GNU_SOURCE` is required because the codebase uses `strdup()`, which is not
exposed by the strict `-std=c11` (ISO C) feature set. See `HACKING.md` for the
full build and test workflow.

## Thread Safety

ANVIL is NOT thread-safe. Each thread should use its own context.

```c
// WRONG: Sharing context between threads
anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
// Thread 1: anvil_build_add(ctx, ...);
// Thread 2: anvil_build_sub(ctx, ...);  // RACE CONDITION!

// CORRECT: Each thread has its own context
void *thread_func(void *arg) {
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_X86_64);
    // Use ctx only in this thread
    anvil_ctx_destroy(ctx);
    return NULL;
}
```

## Error Handling

ANVIL uses return codes for error handling:

```c
anvil_error_t err = anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64);
if (err != ANVIL_OK) {
    switch (err) {
        case ANVIL_ERR_NO_BACKEND:
            fprintf(stderr, "No backend for architecture\n");
            break;
        case ANVIL_ERR_INVALID_ARG:
            fprintf(stderr, "Invalid argument\n");
            break;
        default:
            fprintf(stderr, "Unknown error: %d\n", err);
    }
}
```

Functions that return pointers return NULL on failure.
