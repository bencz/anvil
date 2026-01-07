# ANVIL - A Nimble Versatile Intermediate Language

ANVIL is a lightweight code generation library designed as a simpler alternative to LLVM. It provides a clean C API for generating machine code for multiple architectures.

## Features

- **Multi-target support**: x86_64 (Linux/Windows), ARM64 (Linux/macOS), PPC64 Big Endian (Linux)
- **Clean C API**: Simple, intuitive API for building IR
- **No #ifdef for targets**: All backends compile together, selection at runtime
- **Arena-based allocation**: Fast, efficient memory management
- **Multiple ABIs**: System V AMD64, Win64, AAPCS64, Apple ARM64, PPC64 ELFv2
- **Optimized code generation**: Leaf function detection, peephole optimizations, minimal prologue/epilogue
- **Vtable-based architecture**: Easy to extend with new targets and ABIs

## Building

```bash
mkdir build && cd build
cmake ..
make
```

### Build Options

- `ANVIL_BUILD_EXAMPLES=ON/OFF` - Build example programs (default: ON)
- `ANVIL_BUILD_TESTS=ON/OFF` - Build test programs (default: ON)

## Quick Start

```c
#include <anvil.h>
#include <stdio.h>

int main(void) {
    anvil_init();
    
    // Create a module
    AnvilModule* mod = anvil_module_new("example");
    
    // Create a function that returns 42
    AnvilFunc* fn = anvil_func_new(mod, "get_answer", anvil_type_i32());
    AnvilValue* val = anvil_const_i32(fn, 42);
    anvil_ret(fn, val);
    
    // Compile for native target
    AnvilTarget target = anvil_target_native();
    AnvilCompileResult result = anvil_compile(mod, target, ANVIL_OPT_STANDARD);
    
    if (result.errors) {
        fprintf(stderr, "Error: %s\n", result.errors);
    } else {
        printf("%s", result.code);
    }
    
    anvil_result_free(&result);
    anvil_module_free(mod);
    anvil_shutdown();
    
    return 0;
}
```

## Supported Targets

| Architecture | OS | ABI | Endianness |
|-------------|-----|-----|------------|
| x86_64 | Linux | System V AMD64 | Little |
| x86_64 | Windows | Win64 | Little |
| ARM64 | Linux | AAPCS64 | Little |
| ARM64 | macOS | Apple ARM64 | Little |
| PPC64 | Linux | ELFv2 | Big |

## API Overview

### Types

```c
AnvilType* anvil_type_void(void);
AnvilType* anvil_type_i8(void);
AnvilType* anvil_type_i16(void);
AnvilType* anvil_type_i32(void);
AnvilType* anvil_type_i64(void);
AnvilType* anvil_type_f32(void);
AnvilType* anvil_type_f64(void);
AnvilType* anvil_type_ptr(AnvilModule* mod, AnvilType* pointee);
AnvilType* anvil_type_array(AnvilModule* mod, AnvilType* elem, int count);
AnvilType* anvil_type_struct(AnvilModule* mod, const char* name);
```

### Functions

```c
AnvilFunc* anvil_func_new(AnvilModule* mod, const char* name, AnvilType* ret_type);
AnvilVar* anvil_func_add_param(AnvilFunc* fn, const char* name, AnvilType* type);
AnvilVar* anvil_func_add_local(AnvilFunc* fn, const char* name, AnvilType* type);
```

### Operations

```c
// Arithmetic
AnvilValue* anvil_add(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_sub(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_mul(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_div(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);

// Comparisons
AnvilValue* anvil_eq(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);
AnvilValue* anvil_lt(AnvilFunc* fn, AnvilValue* lhs, AnvilValue* rhs);

// Memory
AnvilValue* anvil_load(AnvilFunc* fn, AnvilVar* var);
void anvil_store(AnvilFunc* fn, AnvilVar* var, AnvilValue* val);

// Control flow
void anvil_ret(AnvilFunc* fn, AnvilValue* val);
AnvilIf* anvil_if_begin(AnvilFunc* fn, AnvilValue* cond);
AnvilLoop* anvil_while_begin(AnvilFunc* fn, AnvilValue* cond);
AnvilLoop* anvil_for_begin(AnvilFunc* fn, AnvilVar* var, AnvilValue* start, 
                            AnvilValue* end, AnvilValue* step);
```

### Compilation

```c
AnvilTarget anvil_target_native(void);
AnvilTarget anvil_target_from_triple(const char* triple);
AnvilCompileResult anvil_compile(AnvilModule* mod, AnvilTarget target, AnvilOptLevel opt);
```

## Examples

See the `examples/` directory for complete examples:

- `hello.c` - Simple function returning a constant
- `fibonacci.c` - Recursive Fibonacci implementation
- `sum_array.c` - Array iteration with for loop
- `multi_target.c` - Cross-compilation to multiple targets
- `isel_test.c` - Instruction selection and strength reduction optimizations

## Architecture

```
anvil/
├── include/
│   └── anvil.h              # Public API
├── src/
│   ├── core/                # Core utilities
│   │   ├── arena.c/h        # Arena allocator
│   │   ├── vec.c/h          # Dynamic vector
│   │   ├── hash.c/h         # Hash table
│   │   ├── str.c/h          # String utilities
│   │   └── error.c/h        # Error handling
│   ├── ir/                  # Intermediate Representation
│   │   ├── types.c/h        # Type system
│   │   ├── value.c/h        # Values and constants
│   │   ├── inst.c/h         # Instructions
│   │   ├── func.c/h         # Functions and blocks
│   │   ├── builder.c/h      # IR builder
│   │   └── module.c/h       # Module management
│   ├── mir/                 # Machine IR
│   │   ├── mir.c/h          # MIR structures
│   │   ├── lower.c/h        # IR to MIR lowering
│   │   ├── cfg.c/h          # Control flow graph
│   │   ├── liveness.c/h     # Liveness analysis
│   │   └── regalloc.c/h     # Register allocation
│   ├── opt/                 # Generic optimizations
│   │   ├── ir_opt.c/h       # IR-level optimizations
│   │   └── mir_opt.c/h      # MIR-level optimizations
│   ├── backend/             # Code generation backends
│   │   ├── backend.c/h      # Backend interface
│   │   ├── x86_64/          # x86_64 backend
│   │   │   ├── opt/         # Target-specific optimizations
│   │   │   └── abi/         # ABI implementations (sysv, win64)
│   │   ├── arm64/           # ARM64 backend
│   │   │   ├── opt/         # Target-specific optimizations
│   │   │   └── abi/         # ABI implementations (aapcs64, apple)
│   │   └── ppc64/           # PPC64 Big Endian backend
│   │       ├── opt/         # Target-specific optimizations
│   │       └── abi/         # ABI implementations (elfv2)
│   └── api.c                # Public API implementation
├── examples/                # Example programs
└── tests/                   # Unit tests
```

## Compilation Pipeline

```
IR → IR Opt → MIR Lowering → MIR Analyze → MIR Opt → ISel → Vectorize → Regalloc → Schedule → Peephole → Emit
```

1. **IR Optimization**: Constant folding, dead code elimination
2. **MIR Lowering**: Convert IR to machine-level IR with ABI awareness
3. **MIR Analysis**: Detect leaf functions, compute frame requirements
4. **MIR Optimization**: Strength reduction (mul→shift), copy propagation, move chain elimination
5. **Instruction Selection**: Target-specific instruction patterns (LEA, combined ops)
6. **Vectorization**: Auto-vectorization (at aggressive optimization level)
7. **Register Allocation**: Linear scan with ABI-aware preallocation
8. **Instruction Scheduling**: Reorder instructions for better pipeline utilization
9. **Target Peephole**: Architecture-specific optimizations
10. **Emit**: Generate assembly output

## Strength Reduction Optimizations

ANVIL automatically optimizes common patterns:

| Pattern | Optimization |
|---------|-------------|
| `x * 2` | `x + x` or `x << 1` |
| `x * 4` | `x << 2` |
| `x * 8` | `x << 3` |
| `x / 4` | `x >> 2` (unsigned) |
| `x % 16` | `x & 15` |
