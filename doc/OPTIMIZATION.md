# ANVIL IR Optimization

This document describes the IR optimization infrastructure in ANVIL.

## Overview

ANVIL includes a configurable optimization pass infrastructure that operates on the IR before code generation. Optimizations can be enabled or disabled individually, or controlled via optimization levels.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      User Application                        │
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
│                      Pass Manager                            │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐       │
│  │ConstFold │ │   DCE    │ │ Strength │ │SimplifyCFG│       │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      Backend Registry                        │
└─────────────────────────────────────────────────────────────┘
```

## Optimization Levels

| Level | Constant | Description |
|-------|----------|-------------|
| O0 | `ANVIL_OPT_NONE` | No optimization (default) |
| O1 | `ANVIL_OPT_BASIC` | Constant folding, DCE, copy propagation |
| O2 | `ANVIL_OPT_STANDARD` | O1 + CFG simplification, strength reduction, memory opts, CSE |
| O3 | `ANVIL_OPT_AGGRESSIVE` | O2 + loop unrolling (experimental) |

## Available Passes

### Constant Folding (`ANVIL_PASS_CONST_FOLD`)

Evaluates constant expressions at compile time.

**Transformations:**

| Before | After |
|--------|-------|
| `add 3, 5` | `8` |
| `mul 4, 8` | `32` |
| `sub 10, 10` | `0` |
| `and 0xFF, 0x0F` | `0x0F` |

**Algebraic Identities:**

| Pattern | Result |
|---------|--------|
| `x + 0` | `x` |
| `x - 0` | `x` |
| `x * 0` | `0` |
| `x * 1` | `x` |
| `x / 1` | `x` |
| `x % 1` | `0` |
| `x & 0` | `0` |
| `x & -1` | `x` |
| `x & x` | `x` |
| `x \| 0` | `x` |
| `x \| x` | `x` |
| `x ^ 0` | `x` |
| `x ^ x` | `0` |
| `x - x` | `0` |
| `x << 0` | `x` |
| `x >> 0` | `x` |

**Comparison Folding:**

| Pattern | Result |
|---------|--------|
| `x == x` | `true` |
| `x != x` | `false` |
| `x < x` | `false` |
| `x <= x` | `true` |
| `x > x` | `false` |
| `x >= x` | `true` |

### Dead Code Elimination (`ANVIL_PASS_DCE`)

Removes instructions whose results are never used.

**Rules:**
- Instructions with side effects (store, call, branch, ret) are never removed
- NOP instructions (left by other passes) are always removed
- Iterates until no more dead code is found

**Example:**

```
Before:
  %1 = add %a, %b      ; used
  %2 = mul %a, %c      ; NOT used
  %3 = sub %1, %d      ; used
  ret %3

After:
  %1 = add %a, %b
  %3 = sub %1, %d
  ret %3
```

### Strength Reduction (`ANVIL_PASS_STRENGTH_REDUCE`)

Replaces expensive operations with cheaper equivalents.

**Transformations:**

| Before | After | Condition |
|--------|-------|-----------|
| `x * 2` | `x << 1` | Power of 2 |
| `x * 4` | `x << 2` | Power of 2 |
| `x * 8` | `x << 3` | Power of 2 |
| `x * 16` | `x << 4` | Power of 2 |
| `x / 2` | `x >> 1` | Unsigned, power of 2 |
| `x / 4` | `x >> 2` | Unsigned, power of 2 |
| `x % 2` | `x & 1` | Unsigned, power of 2 |
| `x % 8` | `x & 7` | Unsigned, power of 2 |

**Note:** Signed division/modulo by power of 2 is not optimized due to rounding differences for negative numbers.

### Copy Propagation (`ANVIL_PASS_COPY_PROP`)

Replaces uses of copied values with the original value, enabling further optimizations.

**Transformations:**

| Before | After |
|--------|-------|
| `y = x + 0; z = y + 1` | `y = x + 0; z = x + 1` |
| `y = x * 1; z = y * 2` | `y = x * 1; z = x * 2` |
| `y = x & -1; z = y` | `y = x & -1; z = x` |

**Recognized Copy Patterns:**
- `x + 0`, `x - 0`, `x | 0`, `x ^ 0` → copy of x
- `x * 1`, `x / 1` → copy of x
- `x & -1` → copy of x
- `x << 0`, `x >> 0` → copy of x

The dead copy instructions can then be removed by DCE.

### Dead Store Elimination (`ANVIL_PASS_DEAD_STORE`)

Removes store instructions that are overwritten before being read.

**Example:**

```c
// Before
*p = 1;  // Dead store
*p = 2;

// After
*p = 2;
```

**Limitations:**
- Only analyzes within a single basic block
- Conservative with function calls (assumes they may read memory)
- Does not track aliasing across different pointers

### Redundant Load Elimination (`ANVIL_PASS_LOAD_ELIM`)

Eliminates redundant loads from the same memory location when the value hasn't changed.

**Example:**

```c
// Before
x = *p;
y = *p;  // Redundant load
z = x + y;

// After
x = *p;
z = x + x;
```

**Limitations:**
- Only analyzes within a single basic block
- Conservative with stores (any store may invalidate cached loads)
- Different allocas are known not to alias

### Common Subexpression Elimination (`ANVIL_PASS_COMMON_SUBEXPR`)

Identifies and eliminates redundant computations by reusing previously computed values.

**Example:**

```c
// Before
a = x + y;
b = x + y;  // Same computation
result = a * b;

// After
a = x + y;
result = a * a;
```

**Features:**
- Tracks binary arithmetic and bitwise operations
- Recognizes commutative operations (`x + y` == `y + x`)
- Local CSE within basic blocks
- Invalidates expressions on stores and calls

**Supported Operations:**
- Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`
- Bitwise: `AND`, `OR`, `XOR`, `SHL`, `SHR`, `SAR`
- Comparisons: `EQ`, `NE`, `LT`, `LE`, `GT`, `GE`

### Loop Unrolling (`ANVIL_PASS_LOOP_UNROLL`) - Experimental

Unrolls small loops with known trip counts to reduce branch overhead.

**Status:** Experimental - currently disabled by default. The infrastructure is in place but loop pattern detection needs refinement.

**Supported Loop Patterns:**
- Simple counted loops with constant bounds
- Loops with single back-edge
- Loops without complex control flow (no nested loops, no early exits)

**Unrolling Strategies:**

| Strategy | Condition | Description |
|----------|-----------|-------------|
| Full Unroll | Trip count ≤ 8 | Completely eliminates loop structure |
| Partial Unroll | Trip count > 8 or unknown | Duplicates body 2-4x, keeps loop |

**Benefits:**
- Reduces branch overhead
- Enables instruction scheduling
- Allows further optimizations (constant folding, CSE)

**Limitations:**
- Only simple loops are analyzed
- Maximum body size: 32 instructions
- Nested loops not supported
- PHI node pattern detection needs work

### CFG Simplification (`ANVIL_PASS_SIMPLIFY_CFG`)

Simplifies the control flow graph.

**Transformations:**

1. **Constant Branch Folding**: Converts conditional branches with constant conditions to unconditional branches
2. **Empty Block Removal**: Removes blocks that only contain an unconditional branch
3. **Block Merging**: Merges a block with its single successor if the successor has only one predecessor
4. **Unreachable Code Removal**: Removes blocks not reachable from the entry block

**Example:**

```
Before:
  entry:
    br_cond true, then, else
  then:
    br merge
  else:
    br merge
  merge:
    ...

After:
  entry:
    br then
  then:
    ...
```

## API Reference

### Pass Manager

```c
/* Create/destroy pass manager */
anvil_pass_manager_t *anvil_pass_manager_create(anvil_ctx_t *ctx);
void anvil_pass_manager_destroy(anvil_pass_manager_t *pm);

/* Set optimization level (enables/disables passes accordingly) */
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

### Context Integration

```c
/* Set/get optimization level for context */
anvil_error_t anvil_ctx_set_opt_level(anvil_ctx_t *ctx, anvil_opt_level_t level);
anvil_opt_level_t anvil_ctx_get_opt_level(anvil_ctx_t *ctx);

/* Get pass manager for context (creates if needed) */
anvil_pass_manager_t *anvil_ctx_get_pass_manager(anvil_ctx_t *ctx);

/* Optimize module before codegen */
anvil_error_t anvil_module_optimize(anvil_module_t *mod);
```

### Built-in Pass Functions

```c
/* Can be called directly for custom pipelines */
bool anvil_pass_const_fold(anvil_func_t *func);
bool anvil_pass_dce(anvil_func_t *func);
bool anvil_pass_simplify_cfg(anvil_func_t *func);
bool anvil_pass_strength_reduce(anvil_func_t *func);
```

### Pass Information Structure

```c
typedef struct {
    anvil_pass_id_t id;
    const char *name;
    const char *description;
    anvil_pass_func_t run;
    anvil_opt_level_t min_level;
} anvil_pass_info_t;

typedef bool (*anvil_pass_func_t)(anvil_func_t *func);
```

## Usage Examples

### Basic Usage

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

### Fine-Grained Control

```c
/* Get pass manager */
anvil_pass_manager_t *pm = anvil_ctx_get_pass_manager(ctx);

/* Enable specific passes */
anvil_pass_manager_enable(pm, ANVIL_PASS_CONST_FOLD);
anvil_pass_manager_enable(pm, ANVIL_PASS_STRENGTH_REDUCE);

/* Disable specific passes */
anvil_pass_manager_disable(pm, ANVIL_PASS_DCE);

/* Run on module */
anvil_pass_manager_run_module(pm, mod);
```

### Custom Pass

```c
/* Define custom pass function */
bool my_peephole_pass(anvil_func_t *func)
{
    bool changed = false;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            /* ... custom optimization logic ... */
        }
    }
    
    return changed;
}

/* Register with pass manager */
anvil_pass_info_t my_pass = {
    .id = ANVIL_PASS_COUNT,  /* Use next available ID */
    .name = "peephole",
    .description = "Custom peephole optimizations",
    .run = my_peephole_pass,
    .min_level = ANVIL_OPT_STANDARD
};

anvil_pass_manager_t *pm = anvil_ctx_get_pass_manager(ctx);
anvil_pass_manager_register(pm, &my_pass);
```

## Generated Code Examples

### Constant Folding (S/390)

**Before optimization:**
```hlasm
TEST_CONST_FOLD$ENTRY DS    0H
         LA    R2,3            Load constant 3
         AHI   R2,5            Add 5
         LR    R15,R2          Result in R15
```

**After optimization:**
```hlasm
TEST_CONST_FOLD$ENTRY DS    0H
         LA    R15,8           Load constant 8 directly
```

### Strength Reduction (S/390)

**Before optimization:**
```hlasm
TEST_STRENGTH$ENTRY DS    0H
         L     R2,0(,R11)      Load param
         L     R2,0(,R2)
         LA    R3,8            Load constant 8
         MSR   R2,R3           Multiply (expensive)
         LR    R15,R2
```

**After optimization:**
```hlasm
TEST_STRENGTH$ENTRY DS    0H
         L     R2,0(,R11)      Load param
         L     R2,0(,R2)
         LA    R3,3            Load shift amount
         SLL   R2,0(R3)        Shift left by 3 (x * 8 = x << 3)
         LR    R15,R2
```

## Implementation Details

### Pass Execution Order

Passes are executed in the following order:
1. Constant Folding
2. Dead Code Elimination
3. CFG Simplification
4. Strength Reduction
5. Custom passes (in registration order)

### Fixpoint Iteration

The pass manager runs all enabled passes in a loop until no pass reports any changes, or a maximum iteration count (10) is reached. This allows passes to enable further optimizations in subsequent passes.

### Thread Safety

The pass manager is **not** thread-safe. Each thread should have its own context and pass manager.

## Source Files

| File | Description |
|------|-------------|
| `include/anvil/anvil_opt.h` | Public API header |
| `src/opt/opt.c` | Pass manager implementation |
| `src/opt/const_fold.c` | Constant folding pass |
| `src/opt/dce.c` | Dead code elimination pass |
| `src/opt/simplify_cfg.c` | CFG simplification pass |
| `src/opt/strength_reduce.c` | Strength reduction |
| `src/opt/copy_prop.c` | Copy propagation |
| `src/opt/dead_store.c` | Dead store elimination |
| `src/opt/load_elim.c` | Redundant load elimination |
| `src/opt/loop_unroll.c` | Loop unrolling |
| `src/opt/ctx_opt.c` | Context integration |
| `src/opt/cse.c` | Common subexpression elimination |

## Future Work

- Loop-invariant code motion (LICM)
- Inlining
- Tail call optimization
- Register promotion (mem2reg)
