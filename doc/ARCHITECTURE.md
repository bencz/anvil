# ANVIL Internal Architecture

This document describes the internal architecture of the ANVIL library.

## Overview

ANVIL follows a classic compiler backend design with these main components:

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
│                                                                      │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  │
│  │ x86  │ │x86-64│ │S/370 │ │S/390 │ │z/Arch│ │ PPC  │ │ARM64 │  │
│  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘ └──────┘  │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Assembly Output (Text)                          │
└─────────────────────────────────────────────────────────────────────┘
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
};
```

**Responsibilities:**
- Manages target architecture selection
- Holds the active backend instance
- Tracks the current insertion point for IR building
- Caches primitive types to avoid duplication
- Provides memory pool for efficient allocation

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

### Backend Interface

Each backend implements the `anvil_backend_ops_t` interface:

```c
typedef struct {
    const char *name;              // Backend name
    anvil_arch_t arch;             // Target architecture
    
    // Initialize backend
    anvil_error_t (*init)(anvil_backend_t *be, anvil_ctx_t *ctx);
    
    // Cleanup backend
    void (*cleanup)(anvil_backend_t *be);
    
    // Generate code for entire module
    anvil_error_t (*codegen_module)(anvil_backend_t *be, anvil_module_t *mod,
                                     char **output, size_t *len);
    
    // Generate code for single function
    anvil_error_t (*codegen_func)(anvil_backend_t *be, anvil_func_t *func,
                                   char **output, size_t *len);
    
    // Get architecture info
    const anvil_arch_info_t *(*get_arch_info)(anvil_backend_t *be);
} anvil_backend_ops_t;
```

### Backend Instance

```c
struct anvil_backend {
    const anvil_backend_ops_t *ops; // Backend operations
    anvil_ctx_t *ctx;               // Parent context
    void *priv;                     // Backend-private data
};
```

### Backend Registration

Backends are registered at library initialization:

```c
// In backend.c
static const anvil_backend_ops_t *backends[] = {
    &anvil_backend_x86,
    &anvil_backend_x86_64,
    &anvil_backend_s370,
    &anvil_backend_s370_xa,
    &anvil_backend_s390,
    &anvil_backend_zarch,
    &anvil_backend_ppc32,
    &anvil_backend_ppc64,
    &anvil_backend_ppc64le,
    &anvil_backend_arm64,
};

const anvil_backend_ops_t *anvil_get_backend(anvil_arch_t arch) {
    for (size_t i = 0; i < sizeof(backends)/sizeof(backends[0]); i++) {
        if (backends[i]->arch == arch) {
            return backends[i];
        }
    }
    return NULL;
}
```

## Code Generation Flow

### Module Code Generation

```
anvil_module_codegen(mod, &output, &len)
    │
    ├── Get backend from context
    │
    ├── Call backend->ops->codegen_module(be, mod, &output, &len)
    │       │
    │       ├── Emit module header (CSECT, directives, etc.)
    │       │
    │       ├── For each function:
    │       │       │
    │       │       ├── Emit function prologue
    │       │       │
    │       │       ├── For each block:
    │       │       │       │
    │       │       │       ├── Emit block label
    │       │       │       │
    │       │       │       └── For each instruction:
    │       │       │               │
    │       │       │               └── Emit instruction
    │       │       │
    │       │       └── (Epilogue emitted by RET instruction)
    │       │
    │       ├── Emit data section (globals, save areas, etc.)
    │       │
    │       └── Emit footer (LTORG, END, etc.)
    │
    └── Return output string
```

### Instruction Emission

Each backend has an `emit_instr` function that handles all IR operations:

```c
static void backend_emit_instr(backend_t *be, anvil_instr_t *instr)
{
    switch (instr->op) {
        case ANVIL_OP_ADD:
            // Load operands to registers
            emit_load_value(be, instr->operands[0], REG_A);
            emit_load_value(be, instr->operands[1], REG_B);
            // Emit add instruction
            emit("ADD REG_A, REG_B");
            // Result is in REG_A (or wherever convention dictates)
            break;
            
        case ANVIL_OP_RET:
            // Load return value to return register
            if (instr->num_operands > 0) {
                emit_load_value(be, instr->operands[0], REG_RETURN);
            }
            // Emit epilogue
            emit_epilogue(be);
            break;
            
        // ... other operations
    }
}
```

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
│   ├── context.c      # Context management
│   ├── types.c        # Type system
│   ├── module.c       # Module management
│   ├── function.c     # Function management
│   ├── value.c        # Value/instruction creation
│   ├── builder.c      # IR builder
│   ├── strbuf.c       # String buffer utilities
│   ├── backend.c      # Backend registry
│   └── memory.c       # Memory management
│
└── backend/
    ├── x86/
    │   └── x86.c      # x86 32-bit backend
    ├── x86_64/
    │   └── x86_64.c   # x86-64 backend
    ├── s370/
    │   └── s370.c     # IBM S/370 backend (24-bit)
    ├── s370_xa/
    │   └── s370_xa.c  # IBM S/370-XA backend (31-bit)
    ├── s390/
    │   └── s390.c     # IBM S/390 backend (31-bit)
    ├── zarch/
    │   └── zarch.c    # IBM z/Architecture backend (64-bit)
    ├── ppc32/
    │   └── ppc32.c    # PowerPC 32-bit backend (big-endian)
    ├── ppc64/
    │   └── ppc64.c    # PowerPC 64-bit backend (big-endian)
    ├── ppc64le/
    │   └── ppc64le.c  # PowerPC 64-bit backend (little-endian)
    └── arm64/
        └── arm64.c    # ARM64/AArch64 backend
```

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
- Multiple syntax options: GAS (AT&T), NASM, MASM
- System V and Windows ABIs supported
- Full floating-point support via SSE/SSE2

**IBM Mainframe (S/370, S/390, z/Architecture):**
- HLASM syntax output
- GCCMVS compatibility mode
- Hexadecimal Floating Point (HFP) and IEEE 754 support
- Chained save areas for stack management
- Uppercase symbol names

**PowerPC:**
- Big-endian (PPC32, PPC64) and little-endian (PPC64LE) variants
- ELFv1 (PPC64) and ELFv2 (PPC64LE) ABIs
- Full floating-point support

**ARM64:**
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

### Why Separate Backends?

1. **Modularity**: Each backend is self-contained
2. **Extensibility**: Easy to add new architectures
3. **Maintainability**: Changes to one backend don't affect others
4. **Specialization**: Each backend can optimize for its target

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
| CFG Simplification | Merge blocks, remove unreachable | O2 |
| Strength Reduction | Replace expensive ops | O2 |
| Dead Store Elimination | Remove overwritten stores | O2 |
| Load Elimination | Reuse loaded values | O2 |
| CSE | Common subexpression elimination | O2 |
| Loop Unrolling | Unroll small loops (experimental) | O3 |

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
| `src/opt/simplify_cfg.c` | CFG simplification |
| `src/opt/strength_reduce.c` | Strength reduction |
| `src/opt/ctx_opt.c` | Context integration |

## Thread Safety

ANVIL is NOT thread-safe. Each thread should use its own context.

```c
// WRONG: Sharing context between threads
anvil_ctx_t *ctx = anvil_ctx_create();
// Thread 1: anvil_build_add(ctx, ...);
// Thread 2: anvil_build_sub(ctx, ...);  // RACE CONDITION!

// CORRECT: Each thread has its own context
void *thread_func(void *arg) {
    anvil_ctx_t *ctx = anvil_ctx_create();
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
