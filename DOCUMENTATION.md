# ANVIL Library Documentation

## Overview

ANVIL (Abstract Intermediate Representation Library) is a C library for compiler code generation. It provides a portable intermediate representation (IR) that can be lowered to assembly code for multiple target architectures.

## Table of Contents

1. [Architecture](#architecture)
2. [Core Concepts](#core-concepts)
3. [API Reference](#api-reference)
4. [Backend Implementation](#backend-implementation)
5. [Target Architectures](#target-architectures)
6. [Code Generation Examples](#code-generation-examples)

## Architecture

### Directory Structure

```
anvil/
├── include/
│   └── anvil/
│       ├── anvil.h           # Public API header
│       ├── anvil_internal.h  # Internal structures
│       ├── anvil_cpu.h       # CPU model system
│       ├── anvil_debug.h     # IR debug/dump API
│       └── anvil_opt.h       # Optimization API
├── src/
│   ├── core/
│   │   ├── context.c         # Context management
│   │   ├── types.c           # Type system
│   │   ├── module.c          # Module management
│   │   ├── function.c        # Function management
│   │   ├── value.c           # Value/instruction creation
│   │   ├── builder.c         # IR builder
│   │   ├── strbuf.c          # String buffer utilities
│   │   ├── backend.c         # Backend registry
│   │   ├── memory.c          # Memory management
│   │   └── ir_dump.c         # IR debug/dump implementation
│   ├── opt/                  # Optimization passes
│   │   ├── opt.c             # Pass manager
│   │   ├── const_fold.c      # Constant folding
│   │   ├── dce.c             # Dead code elimination
│   │   └── ...               # Other passes
│   └── backend/
│       ├── x86/x86.c         # x86 32-bit backend
│       ├── x86_64/x86_64.c   # x86-64 backend
│       ├── s370/s370.c       # IBM S/370 backend
│       ├── s370_xa/s370_xa.c # IBM S/370-XA backend
│       ├── s390/s390.c       # IBM S/390 backend
│       ├── zarch/zarch.c     # IBM z/Architecture backend
│       ├── ppc32/ppc32.c     # PowerPC 32-bit backend
│       ├── ppc64/             # PowerPC 64-bit BE backend
│       │   ├── ppc64.c
│       │   ├── ppc64_emit.c
│       │   └── ppc64_cpu.c
│       ├── ppc64le/ppc64le.c # PowerPC 64-bit LE backend
│       └── arm64/             # ARM64/AArch64 backend (modular)
│           ├── arm64.c        # Main backend (lifecycle, codegen)
│           ├── arm64_internal.h # Definitions and structures
│           ├── arm64_helpers.c  # Helper functions
│           └── arm64_emit.c     # Instruction emission
├── examples/
│   ├── simple.c              # Basic usage example
│   ├── multiarch.c           # Multi-architecture example
│   ├── ir_dump_test.c        # IR dump/debug example
│   └── ...                   # Other examples
├── doc/                      # Documentation
├── Makefile
└── README.md
```

### Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                      User Application                        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                     ANVIL Public API                         │
│  anvil_ctx_create, anvil_module_create, anvil_build_*       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    IR (Intermediate Representation)          │
│  Modules → Functions → Blocks → Instructions → Values        │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      Backend Registry                        │
└─────────────────────────────────────────────────────────────┘
         │              │              │              │
         ▼              ▼              ▼              ▼
    ┌────────┐    ┌────────┐    ┌────────┐    ┌────────┐
    │  x86   │    │ x86-64 │    │ S/370  │    │ z/Arch │
    └────────┘    └────────┘    └────────┘    └────────┘
         │              │              │              │
         ▼              ▼              ▼              ▼
    ┌─────────────────────────────────────────────────────┐
    │              Assembly Output (Text)                  │
    └─────────────────────────────────────────────────────┘
```

## Core Concepts

### Context (anvil_ctx_t)

The context is the root object that manages all ANVIL resources. It holds:
- Target architecture configuration
- OS ABI variant (System V, Darwin, MVS)
- Floating-point format
- Type cache
- Backend instance
- Memory pools

```c
// Create a new context
anvil_ctx_t *ctx = anvil_ctx_create();

// Set target architecture
anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64);

// Set OS ABI (optional - for platform-specific code generation)
anvil_ctx_set_abi(ctx, ANVIL_ABI_DARWIN);  // For macOS ARM64

// Set FP format (optional - for mainframes)
anvil_ctx_set_fp_format(ctx, ANVIL_FP_IEEE754);

// Destroy context (frees all associated resources)
anvil_ctx_destroy(ctx);
```

### Module (anvil_module_t)

A module represents a compilation unit containing functions and global variables.

```c
// Create module
anvil_module_t *mod = anvil_module_create(ctx, "my_module");

// Add functions and globals...

// Generate code
char *output = NULL;
size_t len = 0;
anvil_module_codegen(mod, &output, &len);

// Cleanup
free(output);
anvil_module_destroy(mod);
```

**Code Generation Flow:**

When `anvil_module_codegen()` is called, the following steps occur:

1. **IR Preparation** (`prepare_ir`): If the backend provides a `prepare_ir` callback, it is called first to perform architecture-specific IR analysis and lowering
2. **Code Generation** (`codegen_module`): The backend generates assembly code from the prepared IR

```
anvil_module_codegen()
    │
    ├── prepare_ir()        [optional - architecture-specific IR preparation]
    │   ├── Analyze functions (detect leaf functions, stack layout)
    │   ├── Lower unsupported operations
    │   └── Perform target-specific optimizations
    │
    └── codegen_module()    [generate assembly output]
        ├── Emit header/directives
        ├── Emit functions
        ├── Emit globals
        └── Emit string literals
```

### Function (anvil_func_t)

Functions contain basic blocks and have a signature (return type and parameters).

```c
// Define function type: int add(int a, int b)
anvil_type_t *i32 = anvil_type_i32(ctx);
anvil_type_t *params[] = { i32, i32 };
anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);

// Create function
anvil_func_t *func = anvil_func_create(mod, "add", func_type, ANVIL_LINK_EXTERNAL);

// Get entry block
anvil_block_t *entry = anvil_func_get_entry(func);

// Get parameters
anvil_value_t *param0 = anvil_func_get_param(func, 0);
anvil_value_t *param1 = anvil_func_get_param(func, 1);
```

### Basic Block (anvil_block_t)

Basic blocks are sequences of instructions with a single entry point and single exit point.

```c
// Get entry block (created automatically)
anvil_block_t *entry = anvil_func_get_entry(func);

// Create additional blocks
anvil_block_t *then_block = anvil_block_create(func, "then");
anvil_block_t *else_block = anvil_block_create(func, "else");
anvil_block_t *merge_block = anvil_block_create(func, "merge");

// Set insertion point
anvil_set_insert_point(ctx, entry);

// Check if block has a terminator instruction
if (!anvil_block_has_terminator(entry)) {
    anvil_build_ret_void(ctx);  // Add implicit return
}
```

### Value (anvil_value_t)

Values represent SSA (Static Single Assignment) values. They can be:
- Constants
- Parameters
- Instruction results
- Global variables

```c
// Integer constants
anvil_value_t *c1 = anvil_const_i32(ctx, 42);
anvil_value_t *c2 = anvil_const_i64(ctx, 1000000);

// Float constants
anvil_value_t *f1 = anvil_const_f32(ctx, 3.14f);
anvil_value_t *f2 = anvil_const_f64(ctx, 2.71828);

// Null pointer
anvil_value_t *null_ptr = anvil_const_null(ctx, ptr_type);
```

### Type System (anvil_type_t)

ANVIL supports the following types:

| Category | Types | Creation Function |
|----------|-------|-------------------|
| Void | void | `anvil_type_void(ctx)` |
| Integer | i8, i16, i32, i64 | `anvil_type_i8(ctx)`, etc. |
| Unsigned | u8, u16, u32, u64 | `anvil_type_u8(ctx)`, etc. |
| Float | f32, f64 | `anvil_type_f32(ctx)`, `anvil_type_f64(ctx)` |
| Pointer | ptr | `anvil_type_ptr(ctx, pointee)` |
| Array | [N x T] | `anvil_type_array(ctx, elem, count)` |
| Struct | { T1, T2, ... } | `anvil_type_struct(ctx, name, fields, n)` |
| Function | T(T1, T2, ...) | `anvil_type_func(ctx, ret, params, n, va)` |

## API Reference

### Context Functions

```c
// Create/destroy context
anvil_ctx_t *anvil_ctx_create(void);
void anvil_ctx_destroy(anvil_ctx_t *ctx);

// Target configuration
anvil_error_t anvil_ctx_set_target(anvil_ctx_t *ctx, anvil_arch_t arch);
anvil_arch_t anvil_ctx_get_target(anvil_ctx_t *ctx);

// Set insertion point for IR building
void anvil_set_insert_point(anvil_ctx_t *ctx, anvil_block_t *block);
```

### Module Functions

```c
// Create/destroy module
anvil_module_t *anvil_module_create(anvil_ctx_t *ctx, const char *name);
void anvil_module_destroy(anvil_module_t *mod);

// Code generation
anvil_error_t anvil_module_codegen(anvil_module_t *mod, char **output, size_t *len);

// Global variables
anvil_value_t *anvil_module_add_global(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type, anvil_linkage_t linkage);

// External declarations
anvil_value_t *anvil_module_add_extern(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type);
```

### Global Variables

Global variables are module-level storage that persists across function calls. They are supported on all backends including mainframes.

```c
// Create a global integer variable
anvil_value_t *counter = anvil_module_add_global(mod, "counter", 
    anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);

// Use in a function - load value
anvil_value_t *val = anvil_build_load(ctx, anvil_type_i32(ctx), counter, "val");

// Store to global
anvil_build_store(ctx, new_val, counter);
```

**Mainframe Backend Notes:**
- Global names are converted to UPPERCASE (GCCMVS convention)
- Storage types are selected based on variable type:
  - `C` (1 byte) for i8/u8
  - `H` (2 bytes) for i16/u16
  - `F` (4 bytes) for i32/u32/pointers
  - `FD` (8 bytes) for i64/u64
  - `E` (4 bytes) for f32
  - `D` (8 bytes) for f64
- Direct load/store instructions are used (`L`/`ST` for S/370-S/390, `LGRL`/`STGRL` for z/Architecture)

### Function Functions

```c
// Create function
anvil_func_t *anvil_func_create(anvil_module_t *mod, const char *name,
                                 anvil_type_t *type, anvil_linkage_t linkage);

// Get entry block
anvil_block_t *anvil_func_get_entry(anvil_func_t *func);

// Get parameter
anvil_value_t *anvil_func_get_param(anvil_func_t *func, size_t index);
```

### Block Functions

```c
// Create block
anvil_block_t *anvil_block_create(anvil_func_t *func, const char *name);
```

### IR Builder Functions

#### Arithmetic Operations

```c
anvil_value_t *anvil_build_add(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_sub(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_mul(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_sdiv(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                 anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_udiv(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                 anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_smod(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                 anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_umod(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                 anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_neg(anvil_ctx_t *ctx, anvil_value_t *val,
                                const char *name);
```

#### Bitwise Operations

```c
anvil_value_t *anvil_build_and(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_or(anvil_ctx_t *ctx, anvil_value_t *lhs,
                               anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_xor(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_not(anvil_ctx_t *ctx, anvil_value_t *val,
                                const char *name);
anvil_value_t *anvil_build_shl(anvil_ctx_t *ctx, anvil_value_t *val,
                                anvil_value_t *amt, const char *name);
anvil_value_t *anvil_build_shr(anvil_ctx_t *ctx, anvil_value_t *val,
                                anvil_value_t *amt, const char *name);
anvil_value_t *anvil_build_sar(anvil_ctx_t *ctx, anvil_value_t *val,
                                anvil_value_t *amt, const char *name);
```

#### Comparison Operations

```c
// Signed comparisons
anvil_value_t *anvil_build_cmp_eq(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                   anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_ne(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                   anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_lt(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                   anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_le(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                   anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_gt(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                   anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_ge(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                   anvil_value_t *rhs, const char *name);

// Unsigned comparisons
anvil_value_t *anvil_build_cmp_ult(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                    anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_ule(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                    anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_ugt(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                    anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_uge(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                    anvil_value_t *rhs, const char *name);
```

#### Memory Operations

```c
anvil_value_t *anvil_build_alloca(anvil_ctx_t *ctx, anvil_type_t *type,
                                   const char *name);
anvil_value_t *anvil_build_load(anvil_ctx_t *ctx, anvil_value_t *ptr,
                                 const char *name);
anvil_value_t *anvil_build_store(anvil_ctx_t *ctx, anvil_value_t *val,
                                  anvil_value_t *ptr);
anvil_value_t *anvil_build_gep(anvil_ctx_t *ctx, anvil_value_t *ptr,
                                anvil_value_t **indices, size_t num_indices,
                                const char *name);
```

#### Control Flow Operations

```c
anvil_value_t *anvil_build_br(anvil_ctx_t *ctx, anvil_block_t *dest);
anvil_value_t *anvil_build_br_cond(anvil_ctx_t *ctx, anvil_value_t *cond,
                                    anvil_block_t *then_block,
                                    anvil_block_t *else_block);
anvil_value_t *anvil_build_call(anvil_ctx_t *ctx, anvil_value_t *func,
                                 anvil_value_t **args, size_t num_args,
                                 const char *name);
anvil_value_t *anvil_build_ret(anvil_ctx_t *ctx, anvil_value_t *val);
anvil_value_t *anvil_build_ret_void(anvil_ctx_t *ctx);
```

#### Type Conversion Operations

```c
anvil_value_t *anvil_build_trunc(anvil_ctx_t *ctx, anvil_value_t *val,
                                  anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_zext(anvil_ctx_t *ctx, anvil_value_t *val,
                                 anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_sext(anvil_ctx_t *ctx, anvil_value_t *val,
                                 anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_bitcast(anvil_ctx_t *ctx, anvil_value_t *val,
                                    anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_ptrtoint(anvil_ctx_t *ctx, anvil_value_t *val,
                                     anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_inttoptr(anvil_ctx_t *ctx, anvil_value_t *val,
                                     anvil_type_t *type, const char *name);
```

#### Miscellaneous Operations

```c
anvil_value_t *anvil_build_phi(anvil_ctx_t *ctx, anvil_type_t *type,
                                const char *name);
void anvil_phi_add_incoming(anvil_value_t *phi, anvil_value_t *val,
                             anvil_block_t *block);
anvil_value_t *anvil_build_select(anvil_ctx_t *ctx, anvil_value_t *cond,
                                   anvil_value_t *then_val,
                                   anvil_value_t *else_val, const char *name);
```

### Constants

```c
anvil_value_t *anvil_const_i8(anvil_ctx_t *ctx, int8_t val);
anvil_value_t *anvil_const_i16(anvil_ctx_t *ctx, int16_t val);
anvil_value_t *anvil_const_i32(anvil_ctx_t *ctx, int32_t val);
anvil_value_t *anvil_const_i64(anvil_ctx_t *ctx, int64_t val);
anvil_value_t *anvil_const_f32(anvil_ctx_t *ctx, float val);
anvil_value_t *anvil_const_f64(anvil_ctx_t *ctx, double val);
anvil_value_t *anvil_const_null(anvil_ctx_t *ctx, anvil_type_t *ptr_type);
anvil_value_t *anvil_const_string(anvil_ctx_t *ctx, const char *str);
anvil_value_t *anvil_const_array(anvil_ctx_t *ctx, anvil_type_t *elem_type,
                                  anvil_value_t **elements, size_t num_elements);

// Set initializer for global variable
void anvil_global_set_initializer(anvil_value_t *global, anvil_value_t *init);
```

### Error Codes

```c
typedef enum {
    ANVIL_OK = 0,
    ANVIL_ERR_NOMEM,        // Out of memory
    ANVIL_ERR_INVALID_ARG,  // Invalid argument
    ANVIL_ERR_NOT_FOUND,    // Resource not found
    ANVIL_ERR_TYPE_MISMATCH,// Type mismatch
    ANVIL_ERR_NO_BACKEND,   // No backend for target
    ANVIL_ERR_CODEGEN       // Code generation error
} anvil_error_t;
```

### Linkage Types

```c
typedef enum {
    ANVIL_LINK_INTERNAL,    // Static/internal linkage
    ANVIL_LINK_EXTERNAL,    // External linkage (visible)
    ANVIL_LINK_WEAK,        // Weak linkage
    ANVIL_LINK_COMMON       // Common linkage
} anvil_linkage_t;
```

| Linkage | Description | Use Case |
|---------|-------------|----------|
| `ANVIL_LINK_INTERNAL` | Symbol is only visible within the module (static) | Private helper functions, module-local globals |
| `ANVIL_LINK_EXTERNAL` | Symbol is visible to other modules and can be linked | Public API functions, exported globals |
| `ANVIL_LINK_WEAK` | Symbol can be overridden by another definition | Default implementations, optional hooks |
| `ANVIL_LINK_COMMON` | Tentative definition (like C uninitialized globals) | Uninitialized global variables |

**Example:**
```c
// Internal function (not exported)
anvil_func_t *helper = anvil_func_create(mod, "helper", type, ANVIL_LINK_INTERNAL);

// Public function (exported)
anvil_func_t *api_func = anvil_func_create(mod, "my_api", type, ANVIL_LINK_EXTERNAL);

// Weak function (can be overridden)
anvil_func_t *default_handler = anvil_func_create(mod, "handler", type, ANVIL_LINK_WEAK);
```

### Architecture Types

```c
typedef enum {
    ANVIL_ARCH_UNKNOWN = 0,
    ANVIL_ARCH_X86,         // x86 32-bit
    ANVIL_ARCH_X86_64,      // x86-64
    ANVIL_ARCH_S370,        // IBM S/370 (24-bit)
    ANVIL_ARCH_S370_XA,     // IBM S/370-XA (31-bit)
    ANVIL_ARCH_S390,        // IBM S/390 (31-bit)
    ANVIL_ARCH_ZARCH,       // IBM z/Architecture (64-bit)
    ANVIL_ARCH_PPC32,       // PowerPC 32-bit (big-endian)
    ANVIL_ARCH_PPC64,       // PowerPC 64-bit (big-endian)
    ANVIL_ARCH_PPC64LE,     // PowerPC 64-bit (little-endian)
    ANVIL_ARCH_ARM64,       // ARM64/AArch64
    ANVIL_ARCH_COUNT
} anvil_arch_t;
```

### Assembly Syntax

```c
typedef enum {
    ANVIL_SYNTAX_DEFAULT,   // Default for architecture
    ANVIL_SYNTAX_HLASM,     // IBM HLASM for mainframes
    ANVIL_SYNTAX_GAS,       // GNU Assembler syntax
    ANVIL_SYNTAX_NASM,      // NASM syntax for x86
    ANVIL_SYNTAX_MASM       // Microsoft MASM syntax
} anvil_syntax_t;
```

| Syntax | Architectures | Description |
|--------|---------------|-------------|
| `ANVIL_SYNTAX_DEFAULT` | All | Use default syntax for target architecture |
| `ANVIL_SYNTAX_HLASM` | S/370, S/390, z/Architecture | IBM High Level Assembler |
| `ANVIL_SYNTAX_GAS` | x86, x86-64, ARM64, PowerPC | GNU Assembler (AT&T style for x86) |
| `ANVIL_SYNTAX_NASM` | x86, x86-64 | Netwide Assembler (Intel style) |
| `ANVIL_SYNTAX_MASM` | x86, x86-64 | Microsoft Macro Assembler |

### Endianness

```c
typedef enum {
    ANVIL_ENDIAN_LITTLE,    // Little-endian (x86, ARM64, PPC64LE)
    ANVIL_ENDIAN_BIG        // Big-endian (S/370, S/390, z/Arch, PPC)
} anvil_endian_t;
```

### Stack Direction

```c
typedef enum {
    ANVIL_STACK_DOWN,       // Stack grows toward lower addresses (x86, ARM64)
    ANVIL_STACK_UP          // Stack grows toward higher addresses (mainframes)
} anvil_stack_dir_t;
```

### OS ABI

```c
typedef enum {
    ANVIL_ABI_DEFAULT,      // Default for architecture
    ANVIL_ABI_SYSV,         // System V ABI (Linux, BSD)
    ANVIL_ABI_DARWIN,       // Darwin/macOS (Mach-O, underscore prefix)
    ANVIL_ABI_WIN64,        // Windows x64 ABI
    ANVIL_ABI_MVS           // IBM MVS/z/OS
} anvil_abi_t;
```

| ABI | Architectures | Description |
|-----|---------------|-------------|
| `ANVIL_ABI_DEFAULT` | All | Use default ABI for target |
| `ANVIL_ABI_SYSV` | x86, x86-64, ARM64 | System V ABI (Linux, BSD, most Unix) |
| `ANVIL_ABI_DARWIN` | x86-64, ARM64 | macOS/iOS (underscore prefix, Mach-O directives) |
| `ANVIL_ABI_WIN64` | x86-64 | Windows x64 calling convention |
| `ANVIL_ABI_MVS` | S/370, S/390, z/Architecture | IBM z/OS MVS linkage conventions |

**Example:**
```c
// For macOS ARM64
anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64);
anvil_ctx_set_abi(ctx, ANVIL_ABI_DARWIN);

// For Linux x86-64
anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64);
anvil_ctx_set_abi(ctx, ANVIL_ABI_SYSV);
```

### Floating-Point Formats

```c
typedef enum {
    ANVIL_FP_IEEE754,       // IEEE 754 (x86, x86-64, PowerPC, ARM64, z/Architecture)
    ANVIL_FP_HFP,           // IBM Hexadecimal Floating Point (S/370, S/390)
    ANVIL_FP_HFP_IEEE       // HFP with IEEE 754 support (z/Architecture)
} anvil_fp_format_t;
```

| Architecture | FP Format | Notes |
|--------------|-----------|-------|
| x86, x86-64 | IEEE 754 | Standard IEEE floating-point |
| PowerPC | IEEE 754 | Standard IEEE floating-point |
| ARM64 | IEEE 754 | Standard IEEE floating-point |
| S/370 | HFP | IBM Hexadecimal FP only |
| S/370-XA | HFP | IBM Hexadecimal FP only |
| S/390 | HFP/IEEE | HFP default, IEEE 754 optional |
| z/Architecture | HFP+IEEE | Both HFP and IEEE 754 supported |

### Mainframe FP Instruction Sets

The mainframe backends generate different instructions based on the selected FP format:

| Operation | HFP (Hexadecimal) | IEEE 754 (Binary) |
|-----------|-------------------|-------------------|
| Add Short | `AER` | `AEBR` |
| Add Long | `ADR` | `ADBR` |
| Sub Short | `SER` | `SEBR` |
| Sub Long | `SDR` | `SDBR` |
| Mul Short | `MER` | `MEEBR` |
| Mul Long | `MDR` | `MDBR` |
| Div Short | `DER` | `DEBR` |
| Div Long | `DDR` | `DDBR` |
| Neg Short | `LCER` | `LCEBR` |
| Neg Long | `LCDR` | `LCDBR` |
| Abs Short | `LPER` | `LPEBR` |
| Abs Long | `LPDR` | `LPDBR` |
| Cmp Short | `CER` | `CEBR` |
| Cmp Long | `CDR` | `CDBR` |

**Float→Int Conversion:**
- **HFP**: Uses "Magic Number" technique (`AW 0,=X'4E00000000000000'`)
- **IEEE**: Uses native `CFDBR` instruction (z/Architecture)

### Floating-Point API

```c
/* Set floating-point format (for architectures that support multiple formats) */
anvil_error_t anvil_ctx_set_fp_format(anvil_ctx_t *ctx, anvil_fp_format_t fp_format);

/* Get current floating-point format */
anvil_fp_format_t anvil_ctx_get_fp_format(anvil_ctx_t *ctx);
```

**Usage Example:**
```c
/* For S/390 with IEEE 754 (instead of default HFP) */
anvil_ctx_set_target(ctx, ANVIL_ARCH_S390);
anvil_ctx_set_fp_format(ctx, ANVIL_FP_IEEE754);

/* For z/Architecture with IEEE 754 */
anvil_ctx_set_target(ctx, ANVIL_ARCH_ZARCH);
anvil_ctx_set_fp_format(ctx, ANVIL_FP_IEEE754);
```

### CPU Model API

The CPU model system allows target-specific code generation by specifying the exact processor model. Each CPU model has a set of features (instruction set extensions) that can be queried and used to generate optimized code.

```c
/* Set CPU model for target-specific code generation */
anvil_error_t anvil_ctx_set_cpu(anvil_ctx_t *ctx, anvil_cpu_model_t cpu);

/* Get current CPU model */
anvil_cpu_model_t anvil_ctx_get_cpu(anvil_ctx_t *ctx);

/* Get CPU features for current model (including overrides) */
anvil_cpu_features_t anvil_ctx_get_cpu_features(anvil_ctx_t *ctx);

/* Check if a specific feature is available */
bool anvil_ctx_has_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature);

/* Manually enable/disable features (override model defaults) */
anvil_error_t anvil_ctx_enable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature);
anvil_error_t anvil_ctx_disable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature);

/* Get CPU model name as string */
const char *anvil_cpu_model_name(anvil_cpu_model_t cpu);

/* Get default CPU model for an architecture */
anvil_cpu_model_t anvil_arch_default_cpu(anvil_arch_t arch);

/* Get features for a specific CPU model (without context overrides) */
anvil_cpu_features_t anvil_cpu_model_features(anvil_cpu_model_t cpu);
```

**Usage Example:**
```c
/* Generate code optimized for IBM POWER9 */
anvil_ctx_set_target(ctx, ANVIL_ARCH_PPC64);
anvil_ctx_set_cpu(ctx, ANVIL_CPU_PPC64_POWER9);

/* Check for VSX support */
if (anvil_ctx_has_feature(ctx, ANVIL_FEATURE_PPC_VSX)) {
    // Can use VSX vector instructions
}

/* Force disable a feature for compatibility */
anvil_ctx_disable_feature(ctx, ANVIL_FEATURE_PPC_VSX);

/* Force enable a feature (even if not in model defaults) */
anvil_ctx_enable_feature(ctx, ANVIL_FEATURE_PPC_HTM);
```

**Supported CPU Models:**

| Architecture | CPU Models |
|--------------|------------|
| PowerPC 32 | G3, G4, G4e |
| PowerPC 64 | 970, POWER4-POWER10 |
| z/Architecture | z900, z9, z10, z196, zEC12, z13-z16 |
| ARM64 | Generic, Cortex-A53/A72/A76, Neoverse N1/V1, Apple M1/M2/M3 |
| x86-64 | Generic, Core2, Nehalem, Sandy Bridge, Haswell, Skylake, Ice Lake, Zen/Zen3/Zen4 |

**CPU Feature Flags (PowerPC):**
- `ANVIL_FEATURE_PPC_ALTIVEC` - AltiVec/VMX SIMD
- `ANVIL_FEATURE_PPC_VSX` - VSX (Vector-Scalar Extension)
- `ANVIL_FEATURE_PPC_DFP` - Decimal Floating Point
- `ANVIL_FEATURE_PPC_POPCNTD` - Population count instruction
- `ANVIL_FEATURE_PPC_HTM` - Hardware Transactional Memory
- `ANVIL_FEATURE_PPC_MMA` - Matrix-Multiply Assist (POWER10)
- `ANVIL_FEATURE_PPC_PCREL` - PC-relative addressing (POWER10)

### Floating-Point Operations

```c
/* FP arithmetic */
anvil_value_t *anvil_build_fadd(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_fsub(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_fmul(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_fdiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_fneg(anvil_ctx_t *ctx, anvil_value_t *val, const char *name);
anvil_value_t *anvil_build_fabs(anvil_ctx_t *ctx, anvil_value_t *val, const char *name);
anvil_value_t *anvil_build_fcmp(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);

/* FP conversions */
anvil_value_t *anvil_build_fptrunc(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_fpext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_fptosi(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_fptoui(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_sitofp(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_uitofp(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);

/* FP constants */
anvil_value_t *anvil_const_f32(anvil_ctx_t *ctx, float val);
anvil_value_t *anvil_const_f64(anvil_ctx_t *ctx, double val);
```

### IR Debug/Dump API

ANVIL provides functions for inspecting IR structures, useful for debugging and understanding the generated IR. The debug API is automatically included via `anvil.h`.

```c
/* Print to stdout (convenience functions) */
void anvil_print_module(anvil_module_t *mod);
void anvil_print_func(anvil_func_t *func);
void anvil_print_instr(anvil_instr_t *instr);

/* Dump to FILE* (for custom output destinations) */
void anvil_dump_module(FILE *out, anvil_module_t *mod);
void anvil_dump_func(FILE *out, anvil_func_t *func);
void anvil_dump_block(FILE *out, anvil_block_t *block);
void anvil_dump_instr(FILE *out, anvil_instr_t *instr);
void anvil_dump_value(FILE *out, anvil_value_t *val);
void anvil_dump_type(FILE *out, anvil_type_t *type);

/* Convert to string (caller must free returned string) */
char *anvil_module_to_string(anvil_module_t *mod);
char *anvil_func_to_string(anvil_func_t *func);
```

**Example Usage:**
```c
#include <anvil/anvil.h>

// After building IR...
anvil_print_module(mod);  // Print to stdout

// Or dump to file
FILE *f = fopen("ir_dump.txt", "w");
anvil_dump_module(f, mod);
fclose(f);

// Or get as string
char *ir = anvil_module_to_string(mod);
printf("IR length: %zu\n", strlen(ir));
free(ir);
```

**Output Format:**
```
; ModuleID = 'my_module'
; Functions: 2, Globals: 1

@counter = external global i32 42

define external i32 @factorial(i32 %arg0) {
; Stack size: 0 bytes, max call args: 1
entry:
    %cmp = cmp_le i8 %arg0, 1
    br_cond %cmp, label %base_case, label %recurse

recurse:
    %n_minus_1 = sub i32 %arg0, 1
    %rec_result = call i32 @factorial, %n_minus_1
    %product = mul i32 %arg0, %rec_result
    ret %product

base_case:
    ret 1
}
```

### Array Access (GEP)

The `anvil_build_gep()` function computes element addresses for array indexing:

```c
/* Get Element Pointer - compute address of array element
 * type: element type
 * ptr: base pointer
 * indices: array of index values
 * num_indices: number of indices
 * name: optional name for result
 */
anvil_value_t *anvil_build_gep(anvil_ctx_t *ctx, anvil_type_t *type, 
                                anvil_value_t *ptr, anvil_value_t **indices, 
                                size_t num_indices, const char *name);
```

**Example - Array Sum:**
```c
/* Compute &arr[i] */
anvil_value_t *indices[] = { i };
anvil_value_t *elem_ptr = anvil_build_gep(ctx, i32, arr, indices, 1, "elem_ptr");
anvil_value_t *elem_val = anvil_build_load(ctx, i32, elem_ptr, "elem_val");
```

**Generated Assembly (S/390):**
```hlasm
         L     R2,arr_addr        Load base pointer
         L     R3,i_val           Load index
         SLL   R3,2               Index * 4 (sizeof int)
         AR    R2,R3              Base + offset
         L     R15,0(,R2)         Load arr[i]
```

**Element Size Calculation:**
| Type | Size | Shift |
|------|------|-------|
| i8/u8 | 1 | none |
| i16/u16 | 2 | SLL 1 |
| i32/u32/f32 | 4 | SLL 2 |
| i64/u64/f64/ptr | 8 | SLL 3 (SLLG on z/Arch) |

### Struct Field Access (STRUCT_GEP)

The `anvil_build_struct_gep()` function computes field addresses for struct access:

```c
/* Get Struct Field Pointer - compute address of struct field
 * struct_type: the struct type definition
 * ptr: base pointer to struct
 * field_idx: index of field to access (0-based)
 * name: optional name for result
 */
anvil_value_t *anvil_build_struct_gep(anvil_ctx_t *ctx, anvil_type_t *struct_type,
                                       anvil_value_t *ptr, unsigned field_idx, 
                                       const char *name);
```

**Example - Struct Access:**
```c
/* Define struct Point { int x; int y; } */
anvil_type_t *point_fields[] = { i32, i32 };
anvil_type_t *point_type = anvil_type_struct(ctx, "Point", point_fields, 2);

/* Access p->y (field 1) */
anvil_value_t *y_ptr = anvil_build_struct_gep(ctx, point_type, p, 1, "y_ptr");
anvil_value_t *y_val = anvil_build_load(ctx, i32, y_ptr, "y_val");
```

**Generated Assembly (S/390):**
```hlasm
*        p->x (field 0, offset 0)
         LR    R15,R2             Struct field at offset 0

*        p->y (field 1, offset 4)
         LA    R15,4(,R2)        Struct field at offset 4
```

**Offset Handling:**
| Offset Range | Instruction |
|--------------|-------------|
| 0 | `LR R15,R2` (copy base) |
| 1-4095 | `LA R15,offset(,R2)` |
| >4095 | `LR R15,R2` + `AHI R15,offset` |

## Backend Implementation

### Backend Interface

Each backend must implement the `anvil_backend_ops_t` interface:

```c
typedef struct {
    const char *name;
    anvil_arch_t arch;
    
    anvil_error_t (*init)(anvil_backend_t *be, anvil_ctx_t *ctx);
    void (*cleanup)(anvil_backend_t *be);
    void (*reset)(anvil_backend_t *be);  // Clear cached IR pointers (optional)
    anvil_error_t (*codegen_module)(anvil_backend_t *be, anvil_module_t *mod,
                                     char **output, size_t *len);
    anvil_error_t (*codegen_func)(anvil_backend_t *be, anvil_func_t *func,
                                   char **output, size_t *len);
    const anvil_arch_info_t *(*get_arch_info)(anvil_backend_t *be);
} anvil_backend_ops_t;
```

**Function Descriptions:**
- `init`: Initialize backend state (allocate private data, buffers)
- `cleanup`: Free all backend resources (called during context destruction)
- `reset`: Clear cached pointers to IR values (called before module destruction to prevent dangling pointers)
- `codegen_module`: Generate assembly for entire module
- `codegen_func`: Generate assembly for single function
- `get_arch_info`: Return architecture information

### Architecture Info

```c
typedef struct {
    anvil_arch_t arch;
    const char *name;
    int ptr_size;           // Pointer size in bytes
    int addr_bits;          // Address bits (24, 31, 32, 64)
    int word_size;          // Native word size
    int num_gpr;            // Number of general purpose registers
    int num_fpr;            // Number of floating point registers
    anvil_endian_t endian;  // ANVIL_ENDIAN_LITTLE or ANVIL_ENDIAN_BIG
    anvil_stack_dir_t stack_dir; // ANVIL_STACK_DOWN or ANVIL_STACK_UP
    bool has_condition_codes;
    bool has_delay_slots;
} anvil_arch_info_t;
```

### Creating a New Backend

1. Create backend file `src/backend/<arch>/<arch>.c`
2. Define architecture info
3. Implement backend operations
4. Register backend

Example skeleton:

```c
#include "anvil/anvil_internal.h"

typedef struct {
    anvil_strbuf_t code;
    // Backend-specific state
} myarch_backend_t;

static const anvil_arch_info_t myarch_arch_info = {
    .arch = ANVIL_ARCH_MYARCH,
    .name = "MyArch",
    .ptr_size = 4,
    .addr_bits = 32,
    .word_size = 4,
    .num_gpr = 16,
    .num_fpr = 8,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};

static anvil_error_t myarch_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    myarch_backend_t *priv = calloc(1, sizeof(myarch_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    anvil_strbuf_init(&priv->code);
    be->priv = priv;
    return ANVIL_OK;
}

static void myarch_cleanup(anvil_backend_t *be)
{
    myarch_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    free(priv);
}

static void myarch_reset(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    myarch_backend_t *priv = be->priv;
    // Clear any cached pointers to anvil_value_t
    // This is called before modules are destroyed
    priv->num_stack_slots = 0;
    priv->num_strings = 0;
}

static anvil_error_t myarch_codegen_module(anvil_backend_t *be,
                                            anvil_module_t *mod,
                                            char **output, size_t *len)
{
    myarch_backend_t *priv = be->priv;
    
    // Emit header
    // Emit functions
    // Emit data section
    // Emit footer
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

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

## Target Architectures

### x86 (32-bit)

| Property | Value |
|----------|-------|
| Pointer size | 4 bytes |
| Endianness | Little |
| Stack direction | Down |
| Calling convention | CDECL |
| Output syntax | GAS or NASM |

Register usage:
- EAX: Return value, scratch
- EBX: Callee-saved
- ECX: Scratch, counter
- EDX: Scratch
- ESI, EDI: Callee-saved
- EBP: Frame pointer (optional)
- ESP: Stack pointer

### x86-64

| Property | Value |
|----------|-------|
| Pointer size | 8 bytes |
| Endianness | Little |
| Stack direction | Down |
| Calling convention | System V AMD64 ABI |
| Output syntax | GAS or NASM |

Register usage (System V):
- RAX: Return value
- RDI, RSI, RDX, RCX, R8, R9: Arguments 1-6
- R10, R11: Scratch
- RBX, R12-R15: Callee-saved
- RBP: Frame pointer (optional)
- RSP: Stack pointer

### IBM S/370 (24-bit)

| Property | Value |
|----------|-------|
| Pointer size | 4 bytes |
| Address bits | 24 |
| Endianness | Big |
| Stack direction | Up |
| Calling convention | MVS Linkage |
| Output syntax | HLASM |
| GPRs | 16 |
| FPRs | 4 (F0, F2, F4, F6) |
| Call instruction | BALR R14,R15 |

Register usage:
- R0: Work register (volatile)
- R1: Parameter list pointer
- R2-R11: General purpose
- R12: Base register
- R13: Save area pointer
- R14: Return address
- R15: Entry point / return code

Save area format (72 bytes):
```
+0   Reserved
+4   Previous save area pointer
+8   Next save area pointer
+12  R14 (return address)
+16  R15 (entry point)
+20  R0
...
+68  R12
```

**GCCMVS Conventions:**
- CSECT blank, AMODE ANY, RMODE ANY
- Uppercase function names
- Stack allocation (no GETMAIN)
- VL bit NOT cleared

### IBM S/370-XA (31-bit)

| Property | Value |
|----------|-------|
| Pointer size | 4 bytes |
| Address bits | 31 |
| Endianness | Big |
| Stack direction | Up |
| Calling convention | MVS Linkage |
| Output syntax | HLASM |
| GPRs | 16 |
| FPRs | 4 (F0, F2, F4, F6) |
| Call instruction | BASR R14,R15 |

Same register conventions as S/370.
- Uses `BASR` for calls (31-bit safe) instead of `BALR`
- Does NOT support S/390 immediate instructions or relative branches

**GCCMVS Conventions:** Same as S/370 (CSECT blank, AMODE ANY, RMODE ANY, uppercase names, stack allocation)

### IBM S/390 (31-bit)

| Property | Value |
|----------|-------|
| Pointer size | 4 bytes |
| Address bits | 31 |
| Endianness | Big |
| Stack direction | Up |
| Calling convention | MVS Linkage |
| Output syntax | HLASM |
| GPRs | 16 |
| FPRs | 16 (F0-F15) |
| Call instruction | BASR R14,R15 |

Same register conventions as S/370. Additional instructions:
- LHI (Load Halfword Immediate)
- AHI (Add Halfword Immediate)
- Relative branches (J, JE, JNE, JH, JL, etc.)
- MSR (Multiply Single Register)
- IEEE 754 FP instructions (AEBR, ADBR, etc.) when `ANVIL_FP_IEEE754` is set

**GCCMVS Conventions:** Same as S/370 (CSECT blank, AMODE ANY, RMODE ANY, uppercase names, stack allocation)

### IBM z/Architecture (64-bit)

| Property | Value |
|----------|-------|
| Pointer size | 8 bytes |
| Address bits | 64 |
| Endianness | Big |
| Stack direction | Up |
| Calling convention | z/OS 64-bit Linkage |
| Output syntax | HLASM |
| GPRs | 16 |
| FPRs | 16 (F0-F15) |
| Call instruction | BRASL R14,target |

Same register conventions. 64-bit instructions:
- LGR, AGR, SGR, MSGR, NGR, OGR, XGR (64-bit register operations)
- LGHI, LGFI (Load immediate)
- AGHI (Add Halfword Immediate 64-bit)
- STMG, LMG (Store/Load multiple 64-bit)
- SLLG, SRLG, SRAG (64-bit shifts)
- CGR, LTGR (64-bit compare/test)
- BRASL (Branch relative and save long)
- LARL, LGRL, STGRL (Relative long addressing)
- DSGR (Divide Single 64-bit)
- IEEE 754 FP: CEFBR, CDFBR, CFDBR (direct int↔float conversion)

F4SA Save area format (144 bytes):
```
+0   Reserved
+8   Previous save area pointer
+16  Next save area pointer
+24  R14 (return address)
+32  R15 (entry point)
+40  R0
...
+136 R12
```

**GCCMVS Conventions:** Same as S/370 (CSECT blank, AMODE ANY, RMODE ANY, uppercase names, stack allocation)

### PowerPC 32-bit (Big-Endian)

| Property | Value |
|----------|-------|
| Pointer size | 4 bytes |
| Address bits | 32 |
| Endianness | Big |
| Stack direction | Down |
| Calling convention | System V PPC32 |
| Output syntax | GAS |
| GPRs | 32 |
| FPRs | 32 |

Register usage (System V PPC32):
- R0: Volatile, used in prologue/epilogue
- R1: Stack pointer (SP)
- R2: Reserved (TOC pointer in some ABIs)
- R3-R10: Function arguments (R3 = return value)
- R11-R12: Volatile, used for linkage
- R13: Small data area pointer (reserved)
- R14-R30: Non-volatile (callee-saved)
- R31: Frame pointer
- F0: Volatile
- F1-F8: Floating-point arguments (F1 = FP return)
- F9-F13: Volatile
- F14-F31: Non-volatile (callee-saved)
- CR0-CR7: Condition registers
- LR: Link register (return address)
- CTR: Count register

**Example Prologue:**
```asm
	.globl my_func
	.type my_func, @function
my_func:
	mflr r0
	stw r0, 4(r1)
	stw r31, -4(r1)
	stwu r1, -64(r1)
	addi r31, r1, 64
```

### PowerPC 64-bit Big-Endian (ELFv1 ABI)

| Property | Value |
|----------|-------|
| Pointer size | 8 bytes |
| Address bits | 64 |
| Endianness | Big |
| Stack direction | Down |
| Calling convention | ELFv1 ABI |
| Output syntax | GAS |
| GPRs | 32 |
| FPRs | 32 |
| Min frame size | 112 bytes |

Register usage (ELFv1):
- R0: Volatile
- R1: Stack pointer
- R2: TOC pointer (must be preserved)
- R3-R10: Arguments (R3 = return value)
- R11-R12: Volatile, linkage
- R13: Thread pointer (reserved)
- R14-R31: Non-volatile (callee-saved)
- F0-F13: Volatile (F1-F8 for FP args)
- F14-F31: Non-volatile

**ELFv1 Function Descriptors:**
```asm
	.section ".opd","aw"
	.align 3
	.globl my_func
my_func:
	.quad .L.my_func,.TOC.@tocbase,0
	.previous
	.type my_func, @function
.L.my_func:
	mflr r0
	std r0, 16(r1)
	std r31, -8(r1)
	stdu r1, -128(r1)
```

### PowerPC 64-bit Little-Endian (ELFv2 ABI)

| Property | Value |
|----------|-------|
| Pointer size | 8 bytes |
| Address bits | 64 |
| Endianness | Little |
| Stack direction | Down |
| Calling convention | ELFv2 ABI |
| Output syntax | GAS |
| GPRs | 32 |
| FPRs | 32 |
| Min frame size | 32 bytes |

Register usage (ELFv2 - same as ELFv1):
- R0: Volatile
- R1: Stack pointer
- R2: TOC pointer
- R3-R10: Arguments (R3 = return value)
- R11-R12: Volatile, linkage
- R13: Thread pointer (reserved)
- R14-R31: Non-volatile (callee-saved)

**ELFv2 Differences from ELFv1:**
- No function descriptors (simpler calling)
- Local entry point via `.localentry` directive
- Smaller minimum frame size (32 vs 112 bytes)
- TOC save area at offset 24 (vs 40 in ELFv1)

**Example Prologue (ELFv2):**
```asm
	.abiversion 2
	.globl my_func
	.type my_func, @function
my_func:
0:	addis r2, r12, (.TOC.-0b)@ha
	addi r2, r2, (.TOC.-0b)@l
	.localentry my_func, .-0b
	mflr r0
	std r0, 16(r1)
	std r31, -8(r1)
	stdu r1, -64(r1)
```

### ARM64/AArch64

| Property | Value |
|----------|-------|
| Pointer size | 8 bytes |
| Endianness | Little |
| Stack direction | Down |
| Calling convention | AAPCS64 |
| Output syntax | GAS |

Register usage (AAPCS64):
- X0-X7: Arguments and return value (X0 = return)
- X8: Indirect result location
- X9-X15: Temporary/scratch registers
- X16-X17: Intra-procedure call scratch (IP0, IP1)
- X18: Platform register (reserved)
- X19-X28: Callee-saved registers
- X29 (FP): Frame pointer
- X30 (LR): Link register
- SP: Stack pointer

**Floating-Point Registers:**
- D0-D7: FP arguments and return value
- D8-D15: Callee-saved
- D16-D31: Temporary

**OS ABI Support:**
- **Linux (ELF)**: Standard AAPCS64, no symbol prefix
- **macOS (Darwin)**: Underscore prefix for symbols, Mach-O directives

**Example Prologue (Linux):**
```asm
	.globl my_func
	.type my_func, %function
my_func:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	sub sp, sp, #32          // Allocate locals
```

**Example Prologue (macOS):**
```asm
	.globl _my_func
	.p2align 2
_my_func:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	sub sp, sp, #32          // Allocate locals
```

## Code Generation Examples

### Simple Addition Function

```c
// Create: int add(int a, int b) { return a + b; }

anvil_ctx_t *ctx = anvil_ctx_create();
anvil_ctx_set_target(ctx, ANVIL_ARCH_S370);

anvil_module_t *mod = anvil_module_create(ctx, "example");

anvil_type_t *i32 = anvil_type_i32(ctx);
anvil_type_t *params[] = { i32, i32 };
anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);

anvil_func_t *func = anvil_func_create(mod, "add", func_type, ANVIL_LINK_EXTERNAL);

anvil_block_t *entry = anvil_func_get_entry(func);
anvil_set_insert_point(ctx, entry);

anvil_value_t *a = anvil_func_get_param(func, 0);
anvil_value_t *b = anvil_func_get_param(func, 1);
anvil_value_t *result = anvil_build_add(ctx, a, b, "result");
anvil_build_ret(ctx, result);

char *output = NULL;
size_t len = 0;
anvil_module_codegen(mod, &output, &len);
printf("%s", output);

free(output);
anvil_module_destroy(mod);
anvil_ctx_destroy(ctx);
```

### Conditional Branch

```c
// Create: int max(int a, int b) { return a > b ? a : b; }

anvil_func_t *func = anvil_func_create(mod, "max", func_type, ANVIL_LINK_EXTERNAL);

anvil_block_t *entry = anvil_func_get_entry(func);
anvil_block_t *then_bb = anvil_block_create(func, "then");
anvil_block_t *else_bb = anvil_block_create(func, "else");
anvil_block_t *merge_bb = anvil_block_create(func, "merge");

anvil_set_insert_point(ctx, entry);
anvil_value_t *a = anvil_func_get_param(func, 0);
anvil_value_t *b = anvil_func_get_param(func, 1);
anvil_value_t *cmp = anvil_build_cmp_gt(ctx, a, b, "cmp");
anvil_build_br_cond(ctx, cmp, then_bb, else_bb);

anvil_set_insert_point(ctx, then_bb);
anvil_build_br(ctx, merge_bb);

anvil_set_insert_point(ctx, else_bb);
anvil_build_br(ctx, merge_bb);

anvil_set_insert_point(ctx, merge_bb);
anvil_value_t *phi = anvil_build_phi(ctx, i32, "result");
anvil_phi_add_incoming(phi, a, then_bb);
anvil_phi_add_incoming(phi, b, else_bb);
anvil_build_ret(ctx, phi);
```

### Loop with Local Variables

```c
// Create: int sum_to_n(int n) { int sum = 0, i = 1; while (i <= n) { sum += i; i++; } return sum; }

anvil_block_t *entry = anvil_func_get_entry(func);
anvil_block_t *loop_cond = anvil_block_create(func, "loop_cond");
anvil_block_t *loop_body = anvil_block_create(func, "loop_body");
anvil_block_t *loop_end = anvil_block_create(func, "loop_end");

anvil_set_insert_point(ctx, entry);
anvil_value_t *n = anvil_func_get_param(func, 0);

// Allocate local variables on stack
anvil_value_t *sum_ptr = anvil_build_alloca(ctx, i32, "sum");
anvil_value_t *i_ptr = anvil_build_alloca(ctx, i32, "i");

// Initialize: sum = 0, i = 1
anvil_value_t *zero = anvil_const_i32(ctx, 0);
anvil_value_t *one = anvil_const_i32(ctx, 1);
anvil_build_store(ctx, zero, sum_ptr);
anvil_build_store(ctx, one, i_ptr);
anvil_build_br(ctx, loop_cond);

// Loop condition: while (i <= n)
anvil_set_insert_point(ctx, loop_cond);
anvil_value_t *i_val = anvil_build_load(ctx, i_ptr, "i_val");
anvil_value_t *cmp = anvil_build_cmp_le(ctx, i_val, n, "cmp");
anvil_build_br_cond(ctx, cmp, loop_body, loop_end);

// Loop body: sum += i; i++;
anvil_set_insert_point(ctx, loop_body);
anvil_value_t *sum_val = anvil_build_load(ctx, sum_ptr, "sum_val");
anvil_value_t *i_val2 = anvil_build_load(ctx, i_ptr, "i_val2");
anvil_value_t *new_sum = anvil_build_add(ctx, sum_val, i_val2, "new_sum");
anvil_build_store(ctx, new_sum, sum_ptr);
anvil_value_t *new_i = anvil_build_add(ctx, i_val2, one, "new_i");
anvil_build_store(ctx, new_i, i_ptr);
anvil_build_br(ctx, loop_cond);

// Return sum
anvil_set_insert_point(ctx, loop_end);
anvil_value_t *result = anvil_build_load(ctx, sum_ptr, "result");
anvil_build_ret(ctx, result);
```

### Loop with PHI Nodes (SSA Style)

```c
// Create: int sum(int n) { int s = 0; for (int i = 0; i < n; i++) s += i; return s; }

anvil_block_t *entry = anvil_func_get_entry(func);
anvil_block_t *loop_bb = anvil_block_create(func, "loop");
anvil_block_t *body_bb = anvil_block_create(func, "body");
anvil_block_t *exit_bb = anvil_block_create(func, "exit");

anvil_set_insert_point(ctx, entry);
anvil_value_t *n = anvil_func_get_param(func, 0);
anvil_value_t *zero = anvil_const_i32(ctx, 0);
anvil_build_br(ctx, loop_bb);

anvil_set_insert_point(ctx, loop_bb);
anvil_value_t *i_phi = anvil_build_phi(ctx, i32, "i");
anvil_value_t *s_phi = anvil_build_phi(ctx, i32, "s");
anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_phi, n, "cmp");
anvil_build_br_cond(ctx, cmp, body_bb, exit_bb);

anvil_set_insert_point(ctx, body_bb);
anvil_value_t *new_s = anvil_build_add(ctx, s_phi, i_phi, "new_s");
anvil_value_t *one = anvil_const_i32(ctx, 1);
anvil_value_t *new_i = anvil_build_add(ctx, i_phi, one, "new_i");
anvil_build_br(ctx, loop_bb);

anvil_phi_add_incoming(i_phi, zero, entry);
anvil_phi_add_incoming(i_phi, new_i, body_bb);
anvil_phi_add_incoming(s_phi, zero, entry);
anvil_phi_add_incoming(s_phi, new_s, body_bb);

anvil_set_insert_point(ctx, exit_bb);
anvil_build_ret(ctx, s_phi);
```

## Generated Assembly Examples

### S/370 Output (GCCMVS Style)

```hlasm
***********************************************************************
*        Generated by ANVIL for IBM S/370
***********************************************************************
         CSECT
         AMODE ANY
         RMODE ANY
*
ADD      DS    0H
         STM   R14,R12,12(R13)    Save caller's registers
         LR    R12,R15            Copy entry point to base reg
         USING ADD,R12            Establish addressability
         LR    R11,R1             Save parameter list pointer
*        Set up save area chain (stack allocation)
         LA    R2,72(,R13)        R2 -> our save area
         ST    R13,4(,R2)         Chain: new->prev = caller's
         ST    R2,8(,R13)         Chain: caller->next = new
         LR    R13,R2             R13 -> our save area
*
ADD$ENTRY DS    0H
         L     R2,0(,R11)         Load addr of param 0
         L     R2,0(,R2)          Load param value
         L     R3,4(,R11)         Load addr of param 1
         L     R3,0(,R3)          Load param value
         AR    R2,R3              Add registers
         LR    R15,R2             Result in R15
*        Function epilogue
         L     R13,4(,R13)        Restore caller's SA pointer
         L     R14,12(,R13)       Restore return address
         LM    R0,R12,20(,R13)    Restore R0-R12
         BR    R14                Return to caller
*
         DROP  R12
*
DYN@ADD  EQU   88                 Stack frame size for ADD
         LTORG
R0       EQU   0
...
R15      EQU   15
         END   ADD
```

### z/Architecture Output (GCCMVS Style)

```hlasm
***********************************************************************
*        Generated by ANVIL for IBM z/Architecture
***********************************************************************
         CSECT
         AMODE ANY
         RMODE ANY
*
ADD      DS    0H
         STMG  R14,R12,24(R13)    Save caller's registers
         LGR   R12,R15            Copy entry point to base reg
         USING ADD,R12            Establish addressability
         LGR   R11,R1             Save parameter list pointer
*        Set up save area chain (stack allocation)
         LA    R2,144(,R13)       R2 -> our save area (144 bytes for 64-bit)
         STG   R13,8(,R2)         Chain: new->prev = caller's
         STG   R2,16(,R13)        Chain: caller->next = new
         LGR   R13,R2             R13 -> our save area
*
ADD$ENTRY DS    0H
         LG    R2,0(,R11)         Load addr of param 0
         LG    R2,0(,R2)          Load param value
         LG    R3,8(,R11)         Load addr of param 1
         LG    R3,0(,R3)          Load param value
         AGR   R2,R3              Add 64-bit registers
         LGR   R15,R2             Result in R15
*        Function epilogue
         LG    R13,8(,R13)        Restore caller's SA pointer
         LG    R14,24(,R13)       Restore return address
         LMG   R0,R12,40(,R13)    Restore R0-R12
         BR    R14                Return to caller
*
         DROP  R12
*
DYN@ADD  EQU   176                Stack frame size for ADD
         LTORG
R0       EQU   0
...
R15      EQU   15
         END   ADD
```

### ARM64 Output (Linux)

```asm
// Generated by ANVIL for ARM64 (AArch64) - Linux
	.arch armv8-a
	.text

	.globl add
	.type add, %function
add:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	mov x9, x0                   // Load param 0
	mov x10, x1                  // Load param 1
	add x0, x9, x10              // Add registers
	ldp x29, x30, [sp], #16
	ret
	.size add, .-add
```

### ARM64 Output (macOS)

```asm
// Generated by ANVIL for ARM64 (AArch64) - macOS
	.build_version macos, 11, 0
	.section __TEXT,__text,regular,pure_instructions

	.globl _add
	.p2align 2
_add:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	mov x9, x0                   // Load param 0
	mov x10, x1                  // Load param 1
	add x0, x9, x10              // Add registers
	ldp x29, x30, [sp], #16
	ret
```

### PowerPC 32-bit Output

```asm
# Generated by ANVIL for PowerPC 32-bit (big-endian)
	.text

	.globl add
	.type add, @function
add:
	mflr r0
	stw r0, 4(r1)
	stw r31, -4(r1)
	stwu r1, -64(r1)
	addi r31, r1, 64
	mr r4, r3                    # Load param 0
	mr r5, r4                    # Load param 1
	add r3, r4, r5               # Add registers
	addi r1, r1, 64
	lwz r31, -4(r1)
	lwz r0, 4(r1)
	mtlr r0
	blr
	.size add, .-add
```

### PowerPC 64-bit BE Output (ELFv1)

```asm
# Generated by ANVIL for PowerPC 64-bit (big-endian, ELFv1 ABI)
	.abiversion 1
	.text

	.section ".opd","aw"
	.align 3
	.globl add
add:
	.quad .L.add,.TOC.@tocbase,0
	.previous
	.type add, @function
.L.add:
	mflr r0
	std r0, 16(r1)
	std r2, 40(r1)               # Save TOC
	std r31, -8(r1)
	stdu r1, -128(r1)
	addi r31, r1, 128
	mr r4, r3                    # Load param 0
	mr r5, r4                    # Load param 1
	add r3, r4, r5               # Add registers
	addi r1, r1, 128
	ld r31, -8(r1)
	ld r0, 16(r1)
	mtlr r0
	blr
	.size add, .-add
```

### PowerPC 64-bit LE Output (ELFv2)

```asm
# Generated by ANVIL for PowerPC 64-bit (little-endian, ELFv2 ABI)
	.abiversion 2
	.text

	.globl add
	.type add, @function
add:
0:	addis r2, r12, (.TOC.-0b)@ha
	addi r2, r2, (.TOC.-0b)@l
	.localentry add, .-0b
	mflr r0
	std r0, 16(r1)
	std r31, -8(r1)
	stdu r1, -64(r1)
	addi r31, r1, 64
	mr r4, r3                    # Load param 0
	mr r5, r4                    # Load param 1
	add r3, r4, r5               # Add registers
	addi r1, r1, 64
	ld r31, -8(r1)
	ld r0, 16(r1)
	mtlr r0
	blr
	.size add, .-add
```

## IR Optimization

ANVIL includes a configurable optimization pass infrastructure.

### Optimization Levels

| Level | Constant | Description |
|-------|----------|-------------|
| O0 | `ANVIL_OPT_NONE` | No optimization (default) |
| O1 | `ANVIL_OPT_BASIC` | Constant folding, dead code elimination |
| O2 | `ANVIL_OPT_STANDARD` | O1 + CFG simplification, strength reduction |
| O3 | `ANVIL_OPT_AGGRESSIVE` | O2 + common subexpression elimination |

### Pass Manager API

```c
#include <anvil/anvil_opt.h>

/* Create pass manager */
anvil_pass_manager_t *anvil_pass_manager_create(anvil_ctx_t *ctx);
void anvil_pass_manager_destroy(anvil_pass_manager_t *pm);

/* Set optimization level */
void anvil_pass_manager_set_level(anvil_pass_manager_t *pm, anvil_opt_level_t level);
anvil_opt_level_t anvil_pass_manager_get_level(anvil_pass_manager_t *pm);

/* Enable/disable individual passes */
void anvil_pass_manager_enable(anvil_pass_manager_t *pm, anvil_pass_id_t pass);
void anvil_pass_manager_disable(anvil_pass_manager_t *pm, anvil_pass_id_t pass);
bool anvil_pass_manager_is_enabled(anvil_pass_manager_t *pm, anvil_pass_id_t pass);

/* Run passes */
bool anvil_pass_manager_run_func(anvil_pass_manager_t *pm, anvil_func_t *func);
bool anvil_pass_manager_run_module(anvil_pass_manager_t *pm, anvil_module_t *mod);

/* Register custom pass */
anvil_error_t anvil_pass_manager_register(anvil_pass_manager_t *pm, 
                                           const anvil_pass_info_t *pass);
```

### Context Integration API

```c
/* Set/get optimization level for context */
anvil_error_t anvil_ctx_set_opt_level(anvil_ctx_t *ctx, anvil_opt_level_t level);
anvil_opt_level_t anvil_ctx_get_opt_level(anvil_ctx_t *ctx);

/* Get pass manager for context */
anvil_pass_manager_t *anvil_ctx_get_pass_manager(anvil_ctx_t *ctx);

/* Optimize module */
anvil_error_t anvil_module_optimize(anvil_module_t *mod);
```

### Available Passes

| Pass ID | Name | Description | Min Level |
|---------|------|-------------|-----------|
| `ANVIL_PASS_CONST_FOLD` | Constant Folding | Evaluate constant expressions | O1 |
| `ANVIL_PASS_DCE` | Dead Code Elimination | Remove unused instructions | O1 |
| `ANVIL_PASS_COPY_PROP` | Copy Propagation | Replace uses of copied values | O1 |
| `ANVIL_PASS_SIMPLIFY_CFG` | CFG Simplification | Merge blocks, remove unreachable | O2 |
| `ANVIL_PASS_STRENGTH_REDUCE` | Strength Reduction | Replace expensive ops | O2 |
| `ANVIL_PASS_DEAD_STORE` | Dead Store Elimination | Remove overwritten stores | O2 |
| `ANVIL_PASS_LOAD_ELIM` | Load Elimination | Reuse loaded values | O2 |
| `ANVIL_PASS_COMMON_SUBEXPR` | CSE | Common subexpression elimination | O2 |
| `ANVIL_PASS_LOOP_UNROLL` | Loop Unrolling | Unroll small loops (experimental) | O3 |

### Built-in Pass Functions

```c
/* Can be called directly for custom pipelines */
bool anvil_pass_const_fold(anvil_func_t *func);
bool anvil_pass_dce(anvil_func_t *func);
bool anvil_pass_simplify_cfg(anvil_func_t *func);
bool anvil_pass_strength_reduce(anvil_func_t *func);
bool anvil_pass_copy_prop(anvil_func_t *func);
bool anvil_pass_dead_store(anvil_func_t *func);
bool anvil_pass_load_elim(anvil_func_t *func);
bool anvil_pass_loop_unroll(anvil_func_t *func);   // Experimental
bool anvil_pass_cse(anvil_func_t *func);
```

### Usage Example

```c
#include <anvil/anvil.h>
#include <anvil/anvil_opt.h>

int main(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_S390);
    
    /* Enable O2 optimization */
    anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
    
    anvil_module_t *mod = anvil_module_create(ctx, "test");
    
    /* ... build IR ... */
    
    /* Optimize before codegen */
    anvil_module_optimize(mod);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_module_codegen(mod, &output, &len);
    
    printf("%s", output);
    
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    
    return 0;
}
```

### Constant Folding Examples

The constant folding pass evaluates:

| Expression | Result |
|------------|--------|
| `3 + 5` | `8` |
| `x + 0` | `x` |
| `x * 1` | `x` |
| `x * 0` | `0` |
| `x - x` | `0` |
| `x & x` | `x` |
| `x ^ x` | `0` |

### Strength Reduction Examples

| Original | Optimized |
|----------|-----------|
| `x * 2` | `x << 1` |
| `x * 4` | `x << 2` |
| `x * 8` | `x << 3` |
| `x / 2` (unsigned) | `x >> 1` |
| `x % 8` (unsigned) | `x & 7` |

### Custom Pass Registration

```c
/* Define custom pass function */
bool my_custom_pass(anvil_func_t *func)
{
    bool changed = false;
    /* ... optimization logic ... */
    return changed;
}

/* Register with pass manager */
anvil_pass_info_t my_pass = {
    .id = ANVIL_PASS_COUNT,  /* Use next available ID */
    .name = "my-pass",
    .description = "My custom optimization",
    .run = my_custom_pass,
    .min_level = ANVIL_OPT_BASIC
};

anvil_pass_manager_t *pm = anvil_ctx_get_pass_manager(ctx);
anvil_pass_manager_register(pm, &my_pass);
```

## ARM64 Backend Details

The ARM64 backend supports both Linux (AAPCS64) and macOS (Darwin) ABIs.

### ABI Differences

| Feature | Linux | macOS (Darwin) |
|---------|-------|----------------|
| Symbol prefix | None | Underscore (`_`) |
| Global relocation | `:lo12:name` | `name@PAGEOFF` |
| Page relocation | `name` | `name@PAGE` |
| Object format | ELF | Mach-O |
| `.type` directive | Yes | No |

### Stack Frame Layout

```
Higher addresses
┌─────────────────────┐
│   Caller's frame    │
├─────────────────────┤ ← x29 (FP) after prologue
│   Saved x29, x30    │  (16 bytes, pre-indexed)
├─────────────────────┤
│   Local variables   │  (alloca slots)
│   SSA temporaries   │  (instruction results)
│   Spilled params    │  (x0-x7 saved here)
├─────────────────────┤ ← SP after prologue
Lower addresses
```

### Stack Slot Allocation

The backend calculates stack frame size based on:
- **ALLOCA instructions**: Size based on pointee type (arrays get full element count × element size)
- **Instruction results**: 8 bytes each for SSA temporaries
- **Function parameters**: 8 bytes each for spilled registers

```c
/* Example: int arr[5] allocates 20 bytes (5 × 4), aligned to 24 */
anvil_type_t *arr_type = anvil_type_array(ctx, anvil_type_i32(ctx), 5);
anvil_value_t *arr = anvil_build_alloca(ctx, arr_type, "arr");
```

### Global Variable Access

**Linux:**
```asm
adrp x9, counter
ldr w0, [x9, :lo12:counter]
```

**macOS:**
```asm
adrp x9, _counter@PAGE
ldr w0, [x9, _counter@PAGEOFF]
```

### Large Stack Offsets

For offsets > 255 bytes, the backend uses x16 as scratch:
```asm
sub x16, x29, #512
ldr x0, [x16]
```

### Very Large Stack Frames (>4095 bytes)

For stack frames exceeding 4095 bytes (ARM64 immediate limit), the backend uses a two-instruction sequence:

**Prologue:**
```asm
mov x16, #5920          ; Load large offset
sub sp, sp, x16         ; Allocate stack space
```

**Epilogue:**
```asm
mov x16, #5920          ; Load large offset
add sp, sp, x16         ; Deallocate stack space
```

**Stack access with large offsets:**
```asm
mov x16, #5000          ; Load offset
sub x16, x29, x16       ; Compute address
ldr x0, [x16]           ; Load from computed address
```

### String Pointer Arrays

Global arrays of string pointers are properly initialized with references to string constants:

```c
// C code
static const char *names[] = { "Red", "Green", "Blue" };
```

**Generated assembly:**
```asm
_names:
        .quad .LC0      ; Pointer to "Red"
        .quad .LC1      ; Pointer to "Green"
        .quad .LC2      ; Pointer to "Blue"

        .section __TEXT,__cstring,cstring_literals
.LC0:
        .asciz "Red"
.LC1:
        .asciz "Green"
.LC2:
        .asciz "Blue"
```

### Variadic Function Calls (Darwin/macOS)

On Darwin/macOS ARM64, variadic function arguments must be passed on the stack, not in registers. This is a key difference from Linux AAPCS64.

**Example: `printf("a=%d, b=%d\n", 1, 2)`**

```asm
// Allocate stack for variadic args (2 args × 8 bytes = 16, aligned)
sub sp, sp, #16

// Load format string (fixed argument) into x0
adrp x9, .LC0@PAGE
add x9, x9, .LC0@PAGEOFF
mov x0, x9

// Store variadic arguments on stack
mov x9, #1
str x9, [sp, #0]        // First variadic arg
mov x9, #2
str x9, [sp, #8]        // Second variadic arg

// Call printf
bl _printf

// Restore stack
add sp, sp, #16
```

The backend automatically detects variadic functions via `type->data.func.variadic` and generates the appropriate calling sequence.

### Global Array Initializers

Arrays with initializers are properly emitted with correct element values:

```c
// C code
static const uint16_t lookup[4] = { 10, 20, 30, 40 };
```

**Generated assembly:**
```asm
_lookup:
        .short 10
        .short 20
        .short 30
        .short 40
```

### Floating-Point Global Initializers

Float and double constants are emitted using their bit representation:

```c
// C code
static const float pi = 3.14159f;
static const double e = 2.71828;
```

**Generated assembly:**
```asm
_pi:
        .long 0x40490fd0    ; IEEE 754 bit pattern for 3.14159f

_e:
        .quad 0x4005bf0a8b145769  ; IEEE 754 bit pattern for 2.71828
```

### Sign-Extending Loads

For signed integer types smaller than 64 bits, the backend uses sign-extending load instructions:

| Type | Instruction | Description |
|------|-------------|-------------|
| `i8` (signed) | `ldrsb x0, [addr]` | Load signed byte, sign-extend to 64 bits |
| `i16` (signed) | `ldrsh x0, [addr]` | Load signed halfword, sign-extend to 64 bits |
| `i32` (signed) | `ldrsw x0, [addr]` | Load signed word, sign-extend to 64 bits |
| `u8` (unsigned) | `ldrb w0, [addr]` | Load unsigned byte, zero-extend |
| `u16` (unsigned) | `ldrh w0, [addr]` | Load unsigned halfword, zero-extend |
| `u32` (unsigned) | `ldr w0, [addr]` | Load unsigned word, zero-extend |

## Advanced Examples

ANVIL includes three advanced examples that demonstrate generating linkable libraries:

### Floating-Point Math Library (`examples/fp_math_lib/`)

Generates exportable FP functions that can be linked with C programs:

```bash
make -C examples/fp_math_lib test
```

Functions: `fp_add`, `fp_sub`, `fp_mul`, `fp_div`, `fp_neg`, `fp_abs`

### Dynamic Array Library (`examples/dynamic_array/`)

Demonstrates calling external C functions (`malloc`, `free`, `memcpy`):

```bash
make -C examples/dynamic_array test
```

Functions: `array_create`, `array_destroy`, `array_copy`, `array_sum`, `array_max`, `array_min`, `array_count_if`, `array_scale`

### Base64 Encoding Library (`examples/base64_lib/`)

Demonstrates complex bitwise operations and `select` for conditional values:

```bash
make -C examples/base64_lib test
```

Functions: `base64_encode`, `base64_encoded_len`

### Running All Advanced Examples

```bash
make test-examples-advanced
```

## Building and Linking

### Compilation

```bash
# Build library
make

# Build with debug symbols
make DEBUG=1

# Clean build
make clean && make

# Build advanced examples
make examples-advanced
```

### Linking

```bash
# Compile your program
gcc -I/path/to/anvil/include -c myprogram.c -o myprogram.o

# Link with ANVIL
gcc myprogram.o -L/path/to/anvil/lib -lanvil -o myprogram
```

### Installation

```bash
# Install to /usr/local (requires root)
sudo make install

# Install to custom prefix
make install PREFIX=/opt/anvil
```

## License

Unlicense
