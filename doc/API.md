# ANVIL API Reference

Complete API reference for the ANVIL library.

## Table of Contents

1. [Context API](#context-api)
2. [CPU Model API](#cpu-model-api)
3. [Module API](#module-api)
4. [Function API](#function-api)
5. [Block API](#block-api)
6. [Type API](#type-api)
7. [Value API](#value-api)
8. [IR Builder API](#ir-builder-api)
9. [Constants API](#constants-api)
10. [Global Variables API](#global-variables-api)
11. [Optimization API](#optimization-api)
12. [Enumerations](#enumerations)
13. [Structures](#structures)

## Context API

The context is the root object managing all ANVIL resources.

### anvil_ctx_create

```c
anvil_ctx_t *anvil_ctx_create(void);
```

Creates a new ANVIL context.

**Returns:** Pointer to new context, or NULL on failure.

**Example:**
```c
anvil_ctx_t *ctx = anvil_ctx_create();
if (!ctx) {
    fprintf(stderr, "Failed to create context\n");
    return 1;
}
```

### anvil_ctx_destroy

```c
void anvil_ctx_destroy(anvil_ctx_t *ctx);
```

Destroys a context and frees all associated resources.

**Parameters:**
- `ctx`: Context to destroy

**Note:** This does NOT free modules. Destroy modules first with `anvil_module_destroy`.

### anvil_ctx_set_target

```c
anvil_error_t anvil_ctx_set_target(anvil_ctx_t *ctx, anvil_arch_t arch);
```

Sets the target architecture for code generation.

**Parameters:**
- `ctx`: Context
- `arch`: Target architecture enum value

**Returns:** `ANVIL_OK` on success, error code on failure.

**Example:**
```c
anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64);
```

### anvil_ctx_get_target

```c
anvil_arch_t anvil_ctx_get_target(anvil_ctx_t *ctx);
```

Gets the current target architecture.

**Parameters:**
- `ctx`: Context

**Returns:** Current target architecture.

### anvil_set_insert_point

```c
void anvil_set_insert_point(anvil_ctx_t *ctx, anvil_block_t *block);
```

Sets the current insertion point for IR building.

**Parameters:**
- `ctx`: Context
- `block`: Block to insert instructions into

**Example:**
```c
anvil_block_t *entry = anvil_func_get_entry(func);
anvil_set_insert_point(ctx, entry);
// Now all anvil_build_* calls will insert into 'entry'
```

### anvil_get_insert_block

```c
anvil_block_t *anvil_get_insert_block(anvil_ctx_t *ctx);
```

Gets the current insertion block.

**Parameters:**
- `ctx`: Context

**Returns:** Current insertion block, or NULL if none set.

### anvil_ctx_set_abi

```c
anvil_error_t anvil_ctx_set_abi(anvil_ctx_t *ctx, anvil_abi_t abi);
```

Sets the OS ABI for platform-specific code generation.

**Parameters:**
- `ctx`: Context
- `abi`: ABI variant (`ANVIL_ABI_SYSV`, `ANVIL_ABI_DARWIN`, `ANVIL_ABI_MVS`)

**Returns:** `ANVIL_OK` on success, error code on failure.

**Example:**
```c
// For macOS ARM64 (Apple Silicon)
anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64);
anvil_ctx_set_abi(ctx, ANVIL_ABI_DARWIN);
```

**Note:** This affects symbol naming (underscore prefix on Darwin), assembly directives, and section names.

### anvil_ctx_get_abi

```c
anvil_abi_t anvil_ctx_get_abi(anvil_ctx_t *ctx);
```

Gets the current OS ABI.

**Parameters:**
- `ctx`: Context

**Returns:** Current ABI setting.

### anvil_ctx_set_fp_format

```c
anvil_error_t anvil_ctx_set_fp_format(anvil_ctx_t *ctx, anvil_fp_format_t fp_format);
```

Sets the floating-point format for architectures that support multiple formats.

**Parameters:**
- `ctx`: Context
- `fp_format`: FP format (`ANVIL_FP_IEEE754`, `ANVIL_FP_HFP`, `ANVIL_FP_HFP_IEEE`)

**Returns:** `ANVIL_OK` on success, error code on failure.

**Example:**
```c
// Use IEEE 754 on z/Architecture
anvil_ctx_set_target(ctx, ANVIL_ARCH_ZARCH);
anvil_ctx_set_fp_format(ctx, ANVIL_FP_IEEE754);
```

### anvil_ctx_get_fp_format

```c
anvil_fp_format_t anvil_ctx_get_fp_format(anvil_ctx_t *ctx);
```

Gets the current floating-point format.

**Parameters:**
- `ctx`: Context

**Returns:** Current FP format.

## CPU Model API

The CPU model system allows target-specific code generation by specifying the exact processor model. Each CPU model has a set of features (instruction set extensions) that can be queried and used to generate optimized code.

### anvil_ctx_set_cpu

```c
anvil_error_t anvil_ctx_set_cpu(anvil_ctx_t *ctx, anvil_cpu_model_t cpu);
```

Sets the CPU model for target-specific code generation.

**Parameters:**
- `ctx`: Context
- `cpu`: CPU model enum value

**Returns:** `ANVIL_OK` on success, error code on failure.

**Example:**
```c
// Generate code optimized for IBM POWER9
anvil_ctx_set_target(ctx, ANVIL_ARCH_PPC64);
anvil_ctx_set_cpu(ctx, ANVIL_CPU_PPC64_POWER9);

// Generate code for Apple M1
anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64);
anvil_ctx_set_cpu(ctx, ANVIL_CPU_ARM64_APPLE_M1);

// Generate code for IBM z15 mainframe
anvil_ctx_set_target(ctx, ANVIL_ARCH_ZARCH);
anvil_ctx_set_cpu(ctx, ANVIL_CPU_ZARCH_Z15);
```

### anvil_ctx_get_cpu

```c
anvil_cpu_model_t anvil_ctx_get_cpu(anvil_ctx_t *ctx);
```

Gets the current CPU model.

**Parameters:**
- `ctx`: Context

**Returns:** Current CPU model.

### anvil_ctx_get_cpu_features

```c
anvil_cpu_features_t anvil_ctx_get_cpu_features(anvil_ctx_t *ctx);
```

Gets the CPU features for the current CPU model (including any manual overrides).

**Parameters:**
- `ctx`: Context

**Returns:** Bitfield of active CPU features.

### anvil_ctx_has_feature

```c
bool anvil_ctx_has_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature);
```

Checks if a specific feature is available for the current CPU model.

**Parameters:**
- `ctx`: Context
- `feature`: Feature flag to check

**Returns:** `true` if feature is available, `false` otherwise.

**Example:**
```c
anvil_ctx_set_cpu(ctx, ANVIL_CPU_PPC64_POWER8);

if (anvil_ctx_has_feature(ctx, ANVIL_FEATURE_PPC_VSX)) {
    // Can use VSX vector instructions
}

if (anvil_ctx_has_feature(ctx, ANVIL_FEATURE_PPC_HTM)) {
    // Can use Hardware Transactional Memory
}
```

### anvil_ctx_enable_feature

```c
anvil_error_t anvil_ctx_enable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature);
```

Manually enables a specific feature (overrides CPU model defaults).

**Parameters:**
- `ctx`: Context
- `feature`: Feature flag to enable

**Returns:** `ANVIL_OK` on success.

**Example:**
```c
// Force enable HTM even if CPU model doesn't have it
anvil_ctx_enable_feature(ctx, ANVIL_FEATURE_PPC_HTM);
```

### anvil_ctx_disable_feature

```c
anvil_error_t anvil_ctx_disable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature);
```

Manually disables a specific feature (overrides CPU model defaults).

**Parameters:**
- `ctx`: Context
- `feature`: Feature flag to disable

**Returns:** `ANVIL_OK` on success.

**Example:**
```c
// Disable VSX to generate more compatible code
anvil_ctx_disable_feature(ctx, ANVIL_FEATURE_PPC_VSX);
```

### anvil_cpu_model_name

```c
const char *anvil_cpu_model_name(anvil_cpu_model_t cpu);
```

Gets the name of a CPU model as a string.

**Parameters:**
- `cpu`: CPU model enum value

**Returns:** CPU model name string.

### anvil_arch_default_cpu

```c
anvil_cpu_model_t anvil_arch_default_cpu(anvil_arch_t arch);
```

Gets the default CPU model for an architecture.

**Parameters:**
- `arch`: Architecture enum value

**Returns:** Default CPU model for the architecture.

### anvil_cpu_model_features

```c
anvil_cpu_features_t anvil_cpu_model_features(anvil_cpu_model_t cpu);
```

Gets the features for a specific CPU model (without context overrides).

**Parameters:**
- `cpu`: CPU model enum value

**Returns:** Bitfield of CPU features.

### Supported CPU Models

#### PowerPC 32-bit
- `ANVIL_CPU_PPC_G3` - PowerPC 750 (G3)
- `ANVIL_CPU_PPC_G4` - PowerPC 7400/7450 (G4) with AltiVec
- `ANVIL_CPU_PPC_G4E` - PowerPC 7450 (G4e) with AltiVec

#### PowerPC 64-bit
- `ANVIL_CPU_PPC64_970` - PowerPC 970 (G5)
- `ANVIL_CPU_PPC64_POWER4` - IBM POWER4
- `ANVIL_CPU_PPC64_POWER5` - IBM POWER5
- `ANVIL_CPU_PPC64_POWER6` - IBM POWER6
- `ANVIL_CPU_PPC64_POWER7` - IBM POWER7
- `ANVIL_CPU_PPC64_POWER8` - IBM POWER8
- `ANVIL_CPU_PPC64_POWER9` - IBM POWER9
- `ANVIL_CPU_PPC64_POWER10` - IBM POWER10

#### IBM Mainframe (z/Architecture)
- `ANVIL_CPU_ZARCH_Z900` - z900
- `ANVIL_CPU_ZARCH_Z9` - z9 EC/BC
- `ANVIL_CPU_ZARCH_Z10` - z10 EC/BC
- `ANVIL_CPU_ZARCH_Z196` - z196
- `ANVIL_CPU_ZARCH_ZEC12` - zEC12
- `ANVIL_CPU_ZARCH_Z13` - z13
- `ANVIL_CPU_ZARCH_Z14` - z14
- `ANVIL_CPU_ZARCH_Z15` - z15
- `ANVIL_CPU_ZARCH_Z16` - z16

#### ARM64
- `ANVIL_CPU_ARM64_GENERIC` - Generic ARMv8-A
- `ANVIL_CPU_ARM64_CORTEX_A53` - Cortex-A53
- `ANVIL_CPU_ARM64_CORTEX_A72` - Cortex-A72
- `ANVIL_CPU_ARM64_CORTEX_A76` - Cortex-A76
- `ANVIL_CPU_ARM64_NEOVERSE_N1` - Neoverse N1
- `ANVIL_CPU_ARM64_NEOVERSE_V1` - Neoverse V1
- `ANVIL_CPU_ARM64_APPLE_M1` - Apple M1
- `ANVIL_CPU_ARM64_APPLE_M2` - Apple M2
- `ANVIL_CPU_ARM64_APPLE_M3` - Apple M3

#### x86-64
- `ANVIL_CPU_X86_64_GENERIC` - Generic x86-64
- `ANVIL_CPU_X86_64_CORE2` - Intel Core 2
- `ANVIL_CPU_X86_64_NEHALEM` - Intel Nehalem
- `ANVIL_CPU_X86_64_SANDYBRIDGE` - Intel Sandy Bridge
- `ANVIL_CPU_X86_64_HASWELL` - Intel Haswell
- `ANVIL_CPU_X86_64_SKYLAKE` - Intel Skylake
- `ANVIL_CPU_X86_64_ICELAKE` - Intel Ice Lake
- `ANVIL_CPU_X86_64_ZEN` - AMD Zen
- `ANVIL_CPU_X86_64_ZEN3` - AMD Zen 3
- `ANVIL_CPU_X86_64_ZEN4` - AMD Zen 4

### CPU Feature Flags

#### PowerPC Features
- `ANVIL_FEATURE_PPC_ALTIVEC` - AltiVec/VMX SIMD
- `ANVIL_FEATURE_PPC_VSX` - VSX (Vector-Scalar)
- `ANVIL_FEATURE_PPC_DFP` - Decimal Floating Point
- `ANVIL_FEATURE_PPC_POPCNTD` - popcntd instruction
- `ANVIL_FEATURE_PPC_HTM` - Hardware Transactional Memory
- `ANVIL_FEATURE_PPC_MMA` - Matrix-Multiply Assist (POWER10)
- `ANVIL_FEATURE_PPC_PCREL` - PC-relative addressing (POWER10)

#### z/Architecture Features
- `ANVIL_FEATURE_ZARCH_DFP` - Decimal Floating Point
- `ANVIL_FEATURE_ZARCH_VECTOR` - Vector facility
- `ANVIL_FEATURE_ZARCH_VECTOR_ENH1` - Vector enhancements 1
- `ANVIL_FEATURE_ZARCH_VECTOR_ENH2` - Vector enhancements 2
- `ANVIL_FEATURE_ZARCH_NNPA` - Neural Network Processing Assist

#### ARM64 Features
- `ANVIL_FEATURE_ARM64_NEON` - NEON SIMD
- `ANVIL_FEATURE_ARM64_SVE` - Scalable Vector Extension
- `ANVIL_FEATURE_ARM64_SVE2` - SVE2
- `ANVIL_FEATURE_ARM64_ATOMICS` - LSE atomics
- `ANVIL_FEATURE_ARM64_DOTPROD` - Dot product instructions
- `ANVIL_FEATURE_ARM64_BF16` - BFloat16

#### x86/x86-64 Features
- `ANVIL_FEATURE_X86_SSE` - SSE
- `ANVIL_FEATURE_X86_SSE2` - SSE2
- `ANVIL_FEATURE_X86_AVX` - AVX
- `ANVIL_FEATURE_X86_AVX2` - AVX2
- `ANVIL_FEATURE_X86_AVX512F` - AVX-512 Foundation
- `ANVIL_FEATURE_X86_FMA` - FMA3

## Module API

Modules represent compilation units.

### anvil_module_create

```c
anvil_module_t *anvil_module_create(anvil_ctx_t *ctx, const char *name);
```

Creates a new module.

**Parameters:**
- `ctx`: Context
- `name`: Module name (used in generated code)

**Returns:** Pointer to new module, or NULL on failure.

**Example:**
```c
anvil_module_t *mod = anvil_module_create(ctx, "my_module");
```

### anvil_module_destroy

```c
void anvil_module_destroy(anvil_module_t *mod);
```

Destroys a module and all its contents (functions, globals).

**Parameters:**
- `mod`: Module to destroy

### anvil_module_codegen

```c
anvil_error_t anvil_module_codegen(anvil_module_t *mod, char **output, size_t *len);
```

Generates assembly code for the entire module.

**Parameters:**
- `mod`: Module to generate code for
- `output`: Pointer to receive allocated output string
- `len`: Pointer to receive output length (optional, can be NULL)

**Returns:** `ANVIL_OK` on success, error code on failure.

**Note:** Caller must free the output string with `free()`.

**Example:**
```c
char *asm_code = NULL;
size_t len = 0;
anvil_error_t err = anvil_module_codegen(mod, &asm_code, &len);
if (err == ANVIL_OK) {
    printf("%s", asm_code);
    free(asm_code);
}
```

### anvil_module_get_function

```c
anvil_func_t *anvil_module_get_function(anvil_module_t *mod, const char *name);
```

Finds a function by name.

**Parameters:**
- `mod`: Module
- `name`: Function name

**Returns:** Function pointer, or NULL if not found.

## Function API

Functions contain basic blocks and have signatures.

### anvil_func_create

```c
anvil_func_t *anvil_func_create(anvil_module_t *mod, const char *name,
                                 anvil_type_t *type, anvil_linkage_t linkage);
```

Creates a new function.

**Parameters:**
- `mod`: Parent module
- `name`: Function name
- `type`: Function type (created with `anvil_type_func`)
- `linkage`: Linkage type

**Returns:** Pointer to new function, or NULL on failure.

**Example:**
```c
anvil_type_t *i32 = anvil_type_i32(ctx);
anvil_type_t *params[] = { i32, i32 };
anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
anvil_func_t *func = anvil_func_create(mod, "add", func_type, ANVIL_LINK_EXTERNAL);
```

### anvil_func_get_entry

```c
anvil_block_t *anvil_func_get_entry(anvil_func_t *func);
```

Gets the entry basic block of a function.

**Parameters:**
- `func`: Function

**Returns:** Entry block (automatically created with the function).

### anvil_func_get_param

```c
anvil_value_t *anvil_func_get_param(anvil_func_t *func, size_t index);
```

Gets a function parameter by index.

**Parameters:**
- `func`: Function
- `index`: Parameter index (0-based)

**Returns:** Parameter value, or NULL if index out of range.

**Example:**
```c
anvil_value_t *a = anvil_func_get_param(func, 0);  // First parameter
anvil_value_t *b = anvil_func_get_param(func, 1);  // Second parameter
```

### anvil_func_get_param_count

```c
size_t anvil_func_get_param_count(anvil_func_t *func);
```

Gets the number of parameters.

**Parameters:**
- `func`: Function

**Returns:** Number of parameters.

### anvil_func_get_name

```c
const char *anvil_func_get_name(anvil_func_t *func);
```

Gets the function name.

**Parameters:**
- `func`: Function

**Returns:** Function name string.

## Block API

Basic blocks are sequences of instructions.

### anvil_block_create

```c
anvil_block_t *anvil_block_create(anvil_func_t *func, const char *name);
```

Creates a new basic block.

**Parameters:**
- `func`: Parent function
- `name`: Block name (used as label in generated code)

**Returns:** Pointer to new block, or NULL on failure.

**Example:**
```c
anvil_block_t *then_bb = anvil_block_create(func, "then");
anvil_block_t *else_bb = anvil_block_create(func, "else");
anvil_block_t *merge_bb = anvil_block_create(func, "merge");
```

### anvil_block_get_parent

```c
anvil_func_t *anvil_block_get_parent(anvil_block_t *block);
```

Gets the parent function of a block.

**Parameters:**
- `block`: Block

**Returns:** Parent function.

### anvil_block_get_name

```c
const char *anvil_block_get_name(anvil_block_t *block);
```

Gets the block name.

**Parameters:**
- `block`: Block

**Returns:** Block name string.

## Type API

Types define the shape of values.

### Primitive Types

```c
anvil_type_t *anvil_type_void(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_i1(anvil_ctx_t *ctx);    // Boolean
anvil_type_t *anvil_type_i8(anvil_ctx_t *ctx);    // Signed 8-bit
anvil_type_t *anvil_type_i16(anvil_ctx_t *ctx);   // Signed 16-bit
anvil_type_t *anvil_type_i32(anvil_ctx_t *ctx);   // Signed 32-bit
anvil_type_t *anvil_type_i64(anvil_ctx_t *ctx);   // Signed 64-bit
anvil_type_t *anvil_type_u8(anvil_ctx_t *ctx);    // Unsigned 8-bit
anvil_type_t *anvil_type_u16(anvil_ctx_t *ctx);   // Unsigned 16-bit
anvil_type_t *anvil_type_u32(anvil_ctx_t *ctx);   // Unsigned 32-bit
anvil_type_t *anvil_type_u64(anvil_ctx_t *ctx);   // Unsigned 64-bit
anvil_type_t *anvil_type_f32(anvil_ctx_t *ctx);   // 32-bit float
anvil_type_t *anvil_type_f64(anvil_ctx_t *ctx);   // 64-bit double
```

### anvil_type_ptr

```c
anvil_type_t *anvil_type_ptr(anvil_ctx_t *ctx, anvil_type_t *pointee);
```

Creates a pointer type.

**Parameters:**
- `ctx`: Context
- `pointee`: Type being pointed to

**Returns:** Pointer type.

**Example:**
```c
anvil_type_t *i32 = anvil_type_i32(ctx);
anvil_type_t *i32_ptr = anvil_type_ptr(ctx, i32);  // i32*
```

### anvil_type_array

```c
anvil_type_t *anvil_type_array(anvil_ctx_t *ctx, anvil_type_t *elem, size_t count);
```

Creates an array type.

**Parameters:**
- `ctx`: Context
- `elem`: Element type
- `count`: Number of elements

**Returns:** Array type.

**Example:**
```c
anvil_type_t *i32 = anvil_type_i32(ctx);
anvil_type_t *arr = anvil_type_array(ctx, i32, 10);  // [10 x i32]
```

### anvil_type_struct

```c
anvil_type_t *anvil_type_struct(anvil_ctx_t *ctx, const char *name,
                                 anvil_type_t **fields, size_t num_fields);
```

Creates a struct type.

**Parameters:**
- `ctx`: Context
- `name`: Struct name (can be NULL for anonymous)
- `fields`: Array of field types
- `num_fields`: Number of fields

**Returns:** Struct type.

**Example:**
```c
anvil_type_t *fields[] = {
    anvil_type_i32(ctx),  // x
    anvil_type_i32(ctx),  // y
};
anvil_type_t *point = anvil_type_struct(ctx, "Point", fields, 2);
```

### anvil_type_func

```c
anvil_type_t *anvil_type_func(anvil_ctx_t *ctx, anvil_type_t *ret,
                               anvil_type_t **params, size_t num_params,
                               bool variadic);
```

Creates a function type.

**Parameters:**
- `ctx`: Context
- `ret`: Return type
- `params`: Array of parameter types
- `num_params`: Number of parameters
- `variadic`: True if function accepts variable arguments

**Returns:** Function type.

**Example:**
```c
anvil_type_t *i32 = anvil_type_i32(ctx);
anvil_type_t *params[] = { i32, i32 };
anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
// int(int, int)
```

### Type Query Functions

```c
anvil_type_kind_t anvil_type_get_kind(anvil_type_t *type);
size_t anvil_type_get_size(anvil_type_t *type);
size_t anvil_type_get_align(anvil_type_t *type);
bool anvil_type_is_integer(anvil_type_t *type);
bool anvil_type_is_float(anvil_type_t *type);
bool anvil_type_is_pointer(anvil_type_t *type);
bool anvil_type_is_aggregate(anvil_type_t *type);
```

## IR Builder API

The IR builder creates instructions.

### Arithmetic Operations

```c
anvil_value_t *anvil_build_add(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
```
Integer or float addition.

```c
anvil_value_t *anvil_build_sub(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
```
Integer or float subtraction.

```c
anvil_value_t *anvil_build_mul(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
```
Integer or float multiplication.

```c
anvil_value_t *anvil_build_sdiv(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                 anvil_value_t *rhs, const char *name);
```
Signed integer division.

```c
anvil_value_t *anvil_build_udiv(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                 anvil_value_t *rhs, const char *name);
```
Unsigned integer division.

```c
anvil_value_t *anvil_build_smod(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                 anvil_value_t *rhs, const char *name);
```
Signed integer modulo (remainder).

```c
anvil_value_t *anvil_build_umod(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                 anvil_value_t *rhs, const char *name);
```
Unsigned integer modulo.

```c
anvil_value_t *anvil_build_neg(anvil_ctx_t *ctx, anvil_value_t *val,
                                const char *name);
```
Integer or float negation.

### Bitwise Operations

```c
anvil_value_t *anvil_build_and(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
```
Bitwise AND.

```c
anvil_value_t *anvil_build_or(anvil_ctx_t *ctx, anvil_value_t *lhs,
                               anvil_value_t *rhs, const char *name);
```
Bitwise OR.

```c
anvil_value_t *anvil_build_xor(anvil_ctx_t *ctx, anvil_value_t *lhs,
                                anvil_value_t *rhs, const char *name);
```
Bitwise XOR.

```c
anvil_value_t *anvil_build_not(anvil_ctx_t *ctx, anvil_value_t *val,
                                const char *name);
```
Bitwise NOT (complement).

```c
anvil_value_t *anvil_build_shl(anvil_ctx_t *ctx, anvil_value_t *val,
                                anvil_value_t *amt, const char *name);
```
Shift left.

```c
anvil_value_t *anvil_build_shr(anvil_ctx_t *ctx, anvil_value_t *val,
                                anvil_value_t *amt, const char *name);
```
Logical shift right (zero-fill).

```c
anvil_value_t *anvil_build_sar(anvil_ctx_t *ctx, anvil_value_t *val,
                                anvil_value_t *amt, const char *name);
```
Arithmetic shift right (sign-extend).

### Comparison Operations

All comparison operations return an `i1` (boolean) value.

```c
// Signed comparisons
anvil_value_t *anvil_build_cmp_eq(ctx, lhs, rhs, name);   // Equal
anvil_value_t *anvil_build_cmp_ne(ctx, lhs, rhs, name);   // Not equal
anvil_value_t *anvil_build_cmp_lt(ctx, lhs, rhs, name);   // Less than
anvil_value_t *anvil_build_cmp_le(ctx, lhs, rhs, name);   // Less or equal
anvil_value_t *anvil_build_cmp_gt(ctx, lhs, rhs, name);   // Greater than
anvil_value_t *anvil_build_cmp_ge(ctx, lhs, rhs, name);   // Greater or equal

// Unsigned comparisons
anvil_value_t *anvil_build_cmp_ult(ctx, lhs, rhs, name);  // Unsigned less than
anvil_value_t *anvil_build_cmp_ule(ctx, lhs, rhs, name);  // Unsigned less or equal
anvil_value_t *anvil_build_cmp_ugt(ctx, lhs, rhs, name);  // Unsigned greater than
anvil_value_t *anvil_build_cmp_uge(ctx, lhs, rhs, name);  // Unsigned greater or equal
```

### Memory Operations

```c
anvil_value_t *anvil_build_alloca(anvil_ctx_t *ctx, anvil_type_t *type,
                                   const char *name);
```
Allocates space on the stack. Returns a pointer to the allocated space.

```c
anvil_value_t *anvil_build_load(anvil_ctx_t *ctx, anvil_value_t *ptr,
                                 const char *name);
```
Loads a value from memory.

```c
anvil_value_t *anvil_build_store(anvil_ctx_t *ctx, anvil_value_t *val,
                                  anvil_value_t *ptr);
```
Stores a value to memory. Returns NULL (store has no result).

```c
anvil_value_t *anvil_build_gep(anvil_ctx_t *ctx, anvil_value_t *ptr,
                                anvil_value_t **indices, size_t num_indices,
                                const char *name);
```
Get Element Pointer. Computes address of a sub-element.

**Example:**
```c
// Allocate an i32
anvil_value_t *ptr = anvil_build_alloca(ctx, anvil_type_i32(ctx), "x");

// Store 42 to it
anvil_value_t *val = anvil_const_i32(ctx, 42);
anvil_build_store(ctx, val, ptr);

// Load it back
anvil_value_t *loaded = anvil_build_load(ctx, ptr, "loaded");
```

### Control Flow Operations

```c
anvil_value_t *anvil_build_br(anvil_ctx_t *ctx, anvil_block_t *dest);
```
Unconditional branch to a block.

```c
anvil_value_t *anvil_build_br_cond(anvil_ctx_t *ctx, anvil_value_t *cond,
                                    anvil_block_t *then_block,
                                    anvil_block_t *else_block);
```
Conditional branch. If `cond` is true, branches to `then_block`, otherwise to `else_block`.

```c
anvil_value_t *anvil_build_call(anvil_ctx_t *ctx, anvil_value_t *func,
                                 anvil_value_t **args, size_t num_args,
                                 const char *name);
```
Calls a function.

```c
anvil_value_t *anvil_build_ret(anvil_ctx_t *ctx, anvil_value_t *val);
```
Returns a value from the function.

```c
anvil_value_t *anvil_build_ret_void(anvil_ctx_t *ctx);
```
Returns void from the function.

**Example:**
```c
// if (a > b) { return a; } else { return b; }
anvil_block_t *then_bb = anvil_block_create(func, "then");
anvil_block_t *else_bb = anvil_block_create(func, "else");

anvil_value_t *cmp = anvil_build_cmp_gt(ctx, a, b, "cmp");
anvil_build_br_cond(ctx, cmp, then_bb, else_bb);

anvil_set_insert_point(ctx, then_bb);
anvil_build_ret(ctx, a);

anvil_set_insert_point(ctx, else_bb);
anvil_build_ret(ctx, b);
```

### Type Conversion Operations

```c
anvil_value_t *anvil_build_trunc(anvil_ctx_t *ctx, anvil_value_t *val,
                                  anvil_type_t *type, const char *name);
```
Truncates to a smaller integer type.

```c
anvil_value_t *anvil_build_zext(anvil_ctx_t *ctx, anvil_value_t *val,
                                 anvil_type_t *type, const char *name);
```
Zero-extends to a larger integer type.

```c
anvil_value_t *anvil_build_sext(anvil_ctx_t *ctx, anvil_value_t *val,
                                 anvil_type_t *type, const char *name);
```
Sign-extends to a larger integer type.

```c
anvil_value_t *anvil_build_bitcast(anvil_ctx_t *ctx, anvil_value_t *val,
                                    anvil_type_t *type, const char *name);
```
Reinterprets bits as a different type (same size required).

```c
anvil_value_t *anvil_build_ptrtoint(anvil_ctx_t *ctx, anvil_value_t *val,
                                     anvil_type_t *type, const char *name);
```
Converts pointer to integer.

```c
anvil_value_t *anvil_build_inttoptr(anvil_ctx_t *ctx, anvil_value_t *val,
                                     anvil_type_t *type, const char *name);
```
Converts integer to pointer.

### PHI and Select

```c
anvil_value_t *anvil_build_phi(anvil_ctx_t *ctx, anvil_type_t *type,
                                const char *name);
```
Creates a PHI node for SSA form.

```c
void anvil_phi_add_incoming(anvil_value_t *phi, anvil_value_t *val,
                             anvil_block_t *block);
```
Adds an incoming value to a PHI node.

```c
anvil_value_t *anvil_build_select(anvil_ctx_t *ctx, anvil_value_t *cond,
                                   anvil_value_t *then_val,
                                   anvil_value_t *else_val, const char *name);
```
Selects between two values based on a condition (like ternary operator).

**Example:**
```c
// PHI node for loop variable
anvil_set_insert_point(ctx, loop_header);
anvil_value_t *i = anvil_build_phi(ctx, anvil_type_i32(ctx), "i");
anvil_phi_add_incoming(i, anvil_const_i32(ctx, 0), entry_block);
anvil_phi_add_incoming(i, i_next, loop_body);
```

## Constants API

```c
anvil_value_t *anvil_const_i8(anvil_ctx_t *ctx, int8_t val);
anvil_value_t *anvil_const_i16(anvil_ctx_t *ctx, int16_t val);
anvil_value_t *anvil_const_i32(anvil_ctx_t *ctx, int32_t val);
anvil_value_t *anvil_const_i64(anvil_ctx_t *ctx, int64_t val);
anvil_value_t *anvil_const_u8(anvil_ctx_t *ctx, uint8_t val);
anvil_value_t *anvil_const_u16(anvil_ctx_t *ctx, uint16_t val);
anvil_value_t *anvil_const_u32(anvil_ctx_t *ctx, uint32_t val);
anvil_value_t *anvil_const_u64(anvil_ctx_t *ctx, uint64_t val);
anvil_value_t *anvil_const_f32(anvil_ctx_t *ctx, float val);
anvil_value_t *anvil_const_f64(anvil_ctx_t *ctx, double val);
anvil_value_t *anvil_const_null(anvil_ctx_t *ctx, anvil_type_t *ptr_type);
anvil_value_t *anvil_const_string(anvil_ctx_t *ctx, const char *str);
```

**Example:**
```c
anvil_value_t *forty_two = anvil_const_i32(ctx, 42);
anvil_value_t *pi = anvil_const_f64(ctx, 3.14159265359);
anvil_value_t *null_ptr = anvil_const_null(ctx, anvil_type_ptr(ctx, anvil_type_i8(ctx)));
```

## Global Variables API

### anvil_module_add_global

```c
anvil_value_t *anvil_module_add_global(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type, anvil_linkage_t linkage);
```

Creates a global variable in the module.

**Parameters:**
- `mod`: Parent module
- `name`: Variable name
- `type`: Variable type
- `linkage`: Linkage type (ANVIL_LINK_INTERNAL or ANVIL_LINK_EXTERNAL)

**Returns:** Value representing the global variable (can be used with load/store).

**Example:**
```c
// Create a global integer counter
anvil_value_t *counter = anvil_module_add_global(mod, "counter", 
    anvil_type_i32(ctx), ANVIL_LINK_INTERNAL);

// Load from global
anvil_value_t *val = anvil_build_load(ctx, anvil_type_i32(ctx), counter, "val");

// Store to global
anvil_value_t *new_val = anvil_build_add(ctx, val, anvil_const_i32(ctx, 1), "inc");
anvil_build_store(ctx, new_val, counter);
```

### anvil_module_add_extern

```c
anvil_value_t *anvil_module_add_extern(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type);
```

Declares an external global variable (defined elsewhere).

**Parameters:**
- `mod`: Parent module
- `name`: Variable name
- `type`: Variable type

**Returns:** Value representing the external global.

### Mainframe Backend Support

Global variables are fully supported on all mainframe backends (S/370, S/370-XA, S/390, z/Architecture):

| Type | HLASM Storage | Size |
|------|---------------|------|
| i8/u8 | `DS C` | 1 byte |
| i16/u16 | `DS H` | 2 bytes |
| i32/u32/ptr | `DS F` | 4 bytes |
| i64/u64 | `DS FD` | 8 bytes |
| f32 | `DS E` | 4 bytes |
| f64 | `DS D` | 8 bytes |

**Generated Code Example (S/370):**
```hlasm
*        Global variables (static)
COUNTER  DS    F                  Global variable

*        In function code:
         L     R15,COUNTER            Load from global
         ...
         ST    R2,COUNTER            Store to global
```

**Generated Code Example (z/Architecture):**
```hlasm
*        Global variables (static)
COUNTER  DS    F                  Global variable

*        In function code:
         LGRL  R15,COUNTER            Load from global (relative long)
         ...
         STGRL R2,COUNTER            Store to global (relative long)
```

## Enumerations

### anvil_arch_t

```c
typedef enum {
    ANVIL_ARCH_UNKNOWN = 0,
    ANVIL_ARCH_X86,         // x86 32-bit
    ANVIL_ARCH_X86_64,      // x86-64
    ANVIL_ARCH_S370,        // IBM S/370 (24-bit)
    ANVIL_ARCH_S370_XA,     // IBM S/370-XA (31-bit)
    ANVIL_ARCH_S390,        // IBM S/390 (31-bit)
    ANVIL_ARCH_ZARCH,       // IBM z/Architecture (64-bit)
    ANVIL_ARCH_PPC32,       // PowerPC 32-bit
    ANVIL_ARCH_PPC64,       // PowerPC 64-bit BE
    ANVIL_ARCH_PPC64LE,     // PowerPC 64-bit LE
    ANVIL_ARCH_ARM64,       // ARM64/AArch64
    ANVIL_ARCH_COUNT
} anvil_arch_t;
```

### anvil_error_t

```c
typedef enum {
    ANVIL_OK = 0,           // Success
    ANVIL_ERR_NOMEM,        // Out of memory
    ANVIL_ERR_INVALID_ARG,  // Invalid argument
    ANVIL_ERR_NOT_FOUND,    // Resource not found
    ANVIL_ERR_TYPE_MISMATCH,// Type mismatch
    ANVIL_ERR_NO_BACKEND,   // No backend for target architecture
    ANVIL_ERR_CODEGEN       // Code generation error
} anvil_error_t;
```

### anvil_linkage_t

```c
typedef enum {
    ANVIL_LINK_INTERNAL,    // Static/internal linkage (not exported)
    ANVIL_LINK_EXTERNAL,    // External linkage (exported)
    ANVIL_LINK_WEAK         // Weak linkage
} anvil_linkage_t;
```

### anvil_type_kind_t

```c
typedef enum {
    ANVIL_TYPE_VOID,
    ANVIL_TYPE_INT,
    ANVIL_TYPE_FLOAT,
    ANVIL_TYPE_PTR,
    ANVIL_TYPE_ARRAY,
    ANVIL_TYPE_STRUCT,
    ANVIL_TYPE_FUNC
} anvil_type_kind_t;
```

### anvil_op_t

```c
typedef enum {
    // Arithmetic
    ANVIL_OP_ADD,
    ANVIL_OP_SUB,
    ANVIL_OP_MUL,
    ANVIL_OP_SDIV,
    ANVIL_OP_UDIV,
    ANVIL_OP_SMOD,
    ANVIL_OP_UMOD,
    ANVIL_OP_NEG,
    
    // Bitwise
    ANVIL_OP_AND,
    ANVIL_OP_OR,
    ANVIL_OP_XOR,
    ANVIL_OP_NOT,
    ANVIL_OP_SHL,
    ANVIL_OP_SHR,
    ANVIL_OP_SAR,
    
    // Comparison
    ANVIL_OP_CMP_EQ,
    ANVIL_OP_CMP_NE,
    ANVIL_OP_CMP_LT,
    ANVIL_OP_CMP_LE,
    ANVIL_OP_CMP_GT,
    ANVIL_OP_CMP_GE,
    ANVIL_OP_CMP_ULT,
    ANVIL_OP_CMP_ULE,
    ANVIL_OP_CMP_UGT,
    ANVIL_OP_CMP_UGE,
    
    // Memory
    ANVIL_OP_ALLOCA,
    ANVIL_OP_LOAD,
    ANVIL_OP_STORE,
    ANVIL_OP_GEP,
    
    // Control flow
    ANVIL_OP_BR,
    ANVIL_OP_BR_COND,
    ANVIL_OP_CALL,
    ANVIL_OP_RET,
    
    // Conversion
    ANVIL_OP_TRUNC,
    ANVIL_OP_ZEXT,
    ANVIL_OP_SEXT,
    ANVIL_OP_BITCAST,
    ANVIL_OP_PTRTOINT,
    ANVIL_OP_INTTOPTR,
    
    // Misc
    ANVIL_OP_PHI,
    ANVIL_OP_SELECT
} anvil_op_t;
```

## Optimization API

The optimization API provides IR optimization passes. Include `<anvil/anvil_opt.h>`.

### anvil_ctx_set_opt_level

```c
anvil_error_t anvil_ctx_set_opt_level(anvil_ctx_t *ctx, anvil_opt_level_t level);
```

Sets the optimization level for the context.

**Parameters:**
- `ctx`: Context
- `level`: Optimization level (`ANVIL_OPT_NONE`, `ANVIL_OPT_BASIC`, `ANVIL_OPT_STANDARD`, `ANVIL_OPT_AGGRESSIVE`)

**Returns:** `ANVIL_OK` on success.

### anvil_ctx_get_opt_level

```c
anvil_opt_level_t anvil_ctx_get_opt_level(anvil_ctx_t *ctx);
```

Gets the current optimization level.

**Returns:** Current optimization level.

### anvil_ctx_get_pass_manager

```c
anvil_pass_manager_t *anvil_ctx_get_pass_manager(anvil_ctx_t *ctx);
```

Gets or creates the pass manager for the context.

**Returns:** Pass manager pointer.

### anvil_module_optimize

```c
anvil_error_t anvil_module_optimize(anvil_module_t *mod);
```

Runs all enabled optimization passes on the module.

**Parameters:**
- `mod`: Module to optimize

**Returns:** `ANVIL_OK` on success.

**Example:**
```c
anvil_ctx_set_opt_level(ctx, ANVIL_OPT_STANDARD);
anvil_module_optimize(mod);
anvil_module_codegen(mod, &output, &len);
```

### anvil_pass_manager_create

```c
anvil_pass_manager_t *anvil_pass_manager_create(anvil_ctx_t *ctx);
```

Creates a new pass manager.

**Returns:** Pass manager pointer, or NULL on failure.

### anvil_pass_manager_destroy

```c
void anvil_pass_manager_destroy(anvil_pass_manager_t *pm);
```

Destroys a pass manager.

### anvil_pass_manager_set_level

```c
void anvil_pass_manager_set_level(anvil_pass_manager_t *pm, anvil_opt_level_t level);
```

Sets optimization level, enabling/disabling passes accordingly.

### anvil_pass_manager_enable

```c
void anvil_pass_manager_enable(anvil_pass_manager_t *pm, anvil_pass_id_t pass);
```

Enables a specific optimization pass.

### anvil_pass_manager_disable

```c
void anvil_pass_manager_disable(anvil_pass_manager_t *pm, anvil_pass_id_t pass);
```

Disables a specific optimization pass.

### anvil_pass_manager_run_module

```c
bool anvil_pass_manager_run_module(anvil_pass_manager_t *pm, anvil_module_t *mod);
```

Runs all enabled passes on a module.

**Returns:** `true` if any changes were made.

### anvil_pass_manager_run_func

```c
bool anvil_pass_manager_run_func(anvil_pass_manager_t *pm, anvil_func_t *func);
```

Runs all enabled passes on a function.

**Returns:** `true` if any changes were made.

### Built-in Pass Functions

```c
bool anvil_pass_const_fold(anvil_func_t *func);    // Constant folding
bool anvil_pass_dce(anvil_func_t *func);           // Dead code elimination
bool anvil_pass_simplify_cfg(anvil_func_t *func);  // CFG simplification
bool anvil_pass_strength_reduce(anvil_func_t *func); // Strength reduction
bool anvil_pass_copy_prop(anvil_func_t *func);     // Copy propagation
bool anvil_pass_dead_store(anvil_func_t *func);    // Dead store elimination
bool anvil_pass_load_elim(anvil_func_t *func);     // Redundant load elimination
bool anvil_pass_loop_unroll(anvil_func_t *func);   // Loop unrolling (experimental)
bool anvil_pass_cse(anvil_func_t *func);           // Common subexpression elimination
```

## Structures

### anvil_arch_info_t

```c
typedef struct {
    anvil_arch_t arch;
    const char *name;
    int ptr_size;           // Pointer size in bytes
    int addr_bits;          // Address bits (24, 31, 32, 64)
    int word_size;          // Native word size in bytes
    int num_gpr;            // Number of general purpose registers
    int num_fpr;            // Number of floating point registers
    anvil_endian_t endian;  // ANVIL_ENDIAN_LITTLE or ANVIL_ENDIAN_BIG
    anvil_stack_dir_t stack_dir; // ANVIL_STACK_DOWN or ANVIL_STACK_UP
    bool has_condition_codes;
    bool has_delay_slots;
} anvil_arch_info_t;
```

### anvil_backend_ops_t

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

**Fields:**
| Field | Description |
|-------|-------------|
| `name` | Human-readable backend name |
| `arch` | Architecture enum value |
| `init` | Initialize backend state |
| `cleanup` | Free all backend resources |
| `reset` | Clear cached pointers to IR values (prevents dangling pointers) |
| `codegen_module` | Generate assembly for entire module |
| `codegen_func` | Generate assembly for single function |
| `get_arch_info` | Return architecture information |

**Note:** The `reset` function is called by `anvil_ctx_destroy()` before destroying modules. This ensures that any cached pointers to `anvil_value_t` in backend data structures (like stack slots or string tables) are cleared before the IR values are freed.
