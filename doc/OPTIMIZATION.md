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

A pass is enabled at a level when `level >= pass.min_level`, so each higher
level is a strict superset of the one below it.

| Level | Constant | Passes enabled (cumulative) |
|-------|----------|-----------------------------|
| O0 | `ANVIL_OPT_NONE` | None (default) |
| Og | `ANVIL_OPT_DEBUG` | copy_prop, store_load_prop |
| O1 | `ANVIL_OPT_BASIC` | Og + const_fold, dce |
| O2 | `ANVIL_OPT_STANDARD` | O1 + simplify_cfg, strength_reduce, dead_store, load_elim, cse |
| O3 | `ANVIL_OPT_AGGRESSIVE` | Currently the same verified pass set as O2 |

O3 currently produces the same result as O2; no unimplemented pass is selected
or exposed.

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

### Copy Propagation (`ANVIL_PASS_COPY_PROP`) - Og+

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

### Store-Load Propagation (`ANVIL_PASS_STORE_LOAD_PROP`) - Og+

Replaces loads that immediately follow stores to the same address with the stored value.

**Example:**

```c
// Before
*p = x;
y = *p;  // Load from same address
z = y + 1;

// After
*p = x;
z = x + 1;  // Load eliminated, y replaced with x
```

**Benefits:**
- Eliminates redundant memory accesses
- Enables further optimizations (constant propagation, CSE)

**Limitations:**
- Only analyzes within a single basic block
- Requires store and load to be adjacent (no intervening instructions)

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

**Supported Operations** (from `src/opt/cse.c`):
- Arithmetic: `ADD`, `SUB`, `MUL`, `SDIV`, `UDIV`, `SMOD`, `UMOD`
- Bitwise: `AND`, `OR`, `XOR`, `SHL`, `SHR`, `SAR`
- Comparisons: `CMP_EQ`, `CMP_NE`, `CMP_LT`, `CMP_LE`, `CMP_GT`, `CMP_GE`,
  `CMP_ULT`, `CMP_ULE`, `CMP_UGT`, `CMP_UGE`

Commutative operations recognized for normalization: `ADD`, `MUL`, `AND`, `OR`,
`XOR`, `CMP_EQ`, `CMP_NE`.

### Loop Unrolling - Design requirement, not exposed

Unrolls small loops with known trip counts to reduce branch overhead.

**Status:** not part of the current API or pipeline. It will only be exposed
after LoopInfo, canonicalization, trip-count proof, remainder generation and
the semantic tests below are implemented.

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

The pass is aware of all source IR terminators: `br`, `br_cond`, and `switch`.
Switch defaults and case targets are included in reachability analysis,
predecessor cache reconstruction, and branch-target rewrites when an empty block
is bypassed. This keeps switch-heavy CFGs valid after simplification.

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
anvil_error_t anvil_pass_manager_set_level(anvil_pass_manager_t *pm,
                                            anvil_opt_level_t level);
anvil_opt_level_t anvil_pass_manager_get_level(anvil_pass_manager_t *pm);

/* Enable/disable individual passes */
anvil_error_t anvil_pass_manager_enable(anvil_pass_manager_t *pm,
                                         anvil_pass_id_t pass);
anvil_error_t anvil_pass_manager_disable(anvil_pass_manager_t *pm,
                                          anvil_pass_id_t pass);
bool anvil_pass_manager_is_enabled(anvil_pass_manager_t *pm, anvil_pass_id_t pass);

/* Run passes: unchanged, changed, or error (also stored on the context) */
anvil_pass_result_t anvil_pass_manager_run_func(anvil_pass_manager_t *pm,
                                                 anvil_func_t *func);
anvil_pass_result_t anvil_pass_manager_run_module(anvil_pass_manager_t *pm,
                                                   anvil_module_t *mod);

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
anvil_pass_result_t anvil_pass_const_fold(anvil_func_t *func);
anvil_pass_result_t anvil_pass_dce(anvil_func_t *func);
anvil_pass_result_t anvil_pass_simplify_cfg(anvil_func_t *func);
anvil_pass_result_t anvil_pass_strength_reduce(anvil_func_t *func);
anvil_pass_result_t anvil_pass_copy_prop(anvil_func_t *func);
anvil_pass_result_t anvil_pass_dead_store(anvil_func_t *func);
anvil_pass_result_t anvil_pass_load_elim(anvil_func_t *func);
anvil_pass_result_t anvil_pass_cse(anvil_func_t *func);
anvil_pass_result_t anvil_pass_store_load_prop(anvil_func_t *func);
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

typedef anvil_pass_result_t (*anvil_pass_func_t)(anvil_func_t *func);
```

## Usage Examples

### Basic Usage

```c
#include <anvil/anvil.h>
#include <anvil/anvil_opt.h>

int main(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_S390);
    
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
anvil_pass_result_t my_peephole_pass(anvil_func_t *func)
{
    bool changed = false;
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
            /* ... custom optimization logic ... */
        }
    }
    
    return changed ? ANVIL_PASS_RUN_CHANGED : ANVIL_PASS_RUN_UNCHANGED;
}

/* Register with pass manager */
anvil_pass_info_t my_pass = {
    .id = ANVIL_PASS_CUSTOM,
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

The pass manager runs enabled built-in passes in an explicit order (defined by
`pass_exec_order[]` in `src/opt/opt.c`), independent of the enum/registration
order. Opportunity-creating passes run first and DCE sweeps up at the end of each
fixpoint iteration:

1. Copy Propagation (`copy_prop`) — exposes constants
2. Constant Folding (`const_fold`)
3. Common Subexpression Elimination (`cse`)
4. Strength Reduction (`strength_reduce`)
5. Store-Load Propagation (`store_load_prop`)
6. Dead Store Elimination (`dead_store`)
7. Redundant Load Elimination (`load_elim`)
8. CFG Simplification (`simplify_cfg`)
9. Dead Code Elimination (`dce`)
10. Custom passes (in registration order)

Only passes that are enabled for the current optimization level (or enabled
individually) actually run; the order above is the sequence within each fixpoint
iteration.

### Fixpoint Iteration

The pass manager verifies the current function immediately after every built-in or custom
pass, including passes that report no change. It iterates until no pass reports
changes. The fixpoint bound defaults to 10 and can be changed with
`anvil_pass_manager_set_iteration_limit()`. If the pipeline still changes IR at
the configured bound, execution returns `ANVIL_PASS_RUN_ERROR`; reaching the
bound is never reported as successful.
Custom passes run only when the current level reaches their `min_level`.
Manually enabled built-in passes still run at O0.

### Thread Safety

The pass manager is **not** thread-safe. Each thread should have its own context and pass manager.

## Source Files

| File | Description |
|------|-------------|
| `include/anvil/anvil_opt.h` | Public API header |
| `src/opt/opt.c` | Pass manager implementation |
| `src/opt/const_fold.c` | Constant folding pass |
| `src/opt/dce.c` | Dead code elimination pass |
| `src/opt/simplify_cfg.c` | CFG simplification pass, including `switch` reachability/target rewrites |
| `src/opt/strength_reduce.c` | Strength reduction |
| `src/opt/copy_prop.c` | Copy propagation |
| `src/opt/store_load_prop.c` | Store-load propagation |
| `src/opt/dead_store.c` | Dead store elimination |
| `src/opt/load_elim.c` | Redundant load elimination |
| `src/opt/ctx_opt.c` | Context integration |
| `src/opt/cse.c` | Common subexpression elimination |

## Not Implemented

The following are **not** present in the current optimizer. All implemented
passes are local (single-basic-block) except CFG simplification and DCE, which
work across the function. There is no SSA-construction / promotion step, so the
"SSA" properties of the IR depend on how the front-end builds it:

- **mem2reg / register promotion (SSA construction)** — alloca/load/store are not
  promoted to SSA values; only the local memory passes (store-load propagation,
  dead store, redundant load) clean up obvious cases.
- **Global Value Numbering (GVN)** — only local CSE exists.
- **Loop-Invariant Code Motion (LICM)**
- **Loop unrolling**
- **Inlining**
- **Tail call optimization**
