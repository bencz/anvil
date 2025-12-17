# MCC AST Optimization System

This document describes the AST-level optimization system in MCC, which performs source-level transformations before code generation.

## Overview

MCC includes a modular AST optimization pass manager that runs after semantic analysis and before code generation. The optimizer applies various transformations to simplify and improve the AST, reducing the work needed by the ANVIL backend optimizer.

```
Source → Preprocessor → Lexer → Parser → Sema → AST Optimizer → Codegen → Assembly
                                                     ↑
                                              (this module)
```

## Optimization Levels

MCC supports five optimization levels, controlled by the `-O` flag:

| Flag | Level | Description |
|------|-------|-------------|
| `-O0` | None | Minimal optimizations (always-on passes only) |
| `-Og` | Debug | Debug-friendly optimizations |
| `-O1` | Basic | Basic optimizations |
| `-O2` | Standard | Standard optimizations |
| `-O3` | Aggressive | Aggressive optimizations |

### Pass Activation by Level

| Pass | O0 | Og | O1 | O2 | O3 | Description |
|------|:--:|:--:|:--:|:--:|:--:|-------------|
| `normalize` | ✓ | ✓ | ✓ | ✓ | ✓ | Canonicalize AST structure |
| `trivial_const` | ✓ | ✓ | ✓ | ✓ | ✓ | Trivial constant simplifications |
| `identity_ops` | ✓ | ✓ | ✓ | ✓ | ✓ | Identity operation removal (x+0, x*1) |
| `double_neg` | ✓ | ✓ | ✓ | ✓ | ✓ | Double negation removal (--x, ~~x) |
| `copy_prop` | | ✓ | ✓ | ✓ | ✓ | Copy propagation |
| `store_load_prop` | | ✓ | ✓ | ✓ | ✓ | Store-load propagation |
| `unreachable_after_return` | | ✓ | ✓ | ✓ | ✓ | Remove code after return |
| `const_fold` | | | ✓ | ✓ | ✓ | Constant folding |
| `const_prop` | | | ✓ | ✓ | ✓ | Constant propagation |
| `dce` | | | ✓ | ✓ | ✓ | Dead code elimination |
| `dead_store` | | | ✓ | ✓ | ✓ | Dead store elimination |
| `strength_red` | | | ✓ | ✓ | ✓ | Strength reduction |
| `algebraic` | | | ✓ | ✓ | ✓ | Algebraic simplifications |
| `branch_simp` | | | ✓ | ✓ | ✓ | Branch simplification |
| `cse` | | | | ✓ | ✓ | Common subexpression elimination |
| `licm` | | | | ✓ | ✓ | Loop-invariant code motion |
| `loop_simp` | | | | ✓ | ✓ | Loop simplification |
| `tail_call` | | | | ✓ | ✓ | Tail call optimization |
| `inline_small` | | | | ✓ | ✓ | Small function inlining |
| `loop_unroll` | | | | | ✓ | Loop unrolling |
| `inline_aggr` | | | | | ✓ | Aggressive inlining |
| `vectorize` | | | | | ✓ | Vectorization hints |

## Architecture

### Source Files

| File | Description |
|------|-------------|
| `include/ast_opt.h` | Public API and pass ID definitions |
| `src/opt/opt_internal.h` | Internal header for pass implementations |
| `src/opt/ast_opt.c` | Pass manager and initialization |
| `src/opt/opt_helpers.c` | Helper functions (eval, traversal, etc.) |
| `src/opt/opt_const.c` | Constant-related passes |
| `src/opt/opt_simplify.c` | Simplification passes (strength reduction, algebraic) |
| `src/opt/opt_dead.c` | Dead code elimination passes |
| `src/opt/opt_propagate.c` | Propagation passes (constant, copy) |
| `src/opt/opt_cse.c` | Common subexpression elimination |
| `src/opt/opt_loop.c` | Loop optimizations (simplification, unrolling, LICM) |
| `src/opt/opt_inline.c` | Function inlining and tail call optimization |
| `src/opt/opt_stubs.c` | Stub implementations (vectorize only) |

### Key Data Structures

```c
/* Pass identifier */
typedef enum {
    MCC_OPT_PASS_NORMALIZE,
    MCC_OPT_PASS_TRIVIAL_CONST,
    MCC_OPT_PASS_IDENTITY_OPS,
    /* ... */
    MCC_OPT_PASS_COUNT
} mcc_opt_pass_id_t;

/* Pass set (bitset for enabled passes) */
typedef struct {
    uint64_t bits[MCC_OPT_PASSES_WORDS];
} mcc_opt_passes_t;

/* AST optimizer state */
typedef struct mcc_ast_opt {
    mcc_context_t *ctx;
    mcc_sema_t *sema;           /* For semantic info access */
    mcc_opt_level_t level;
    mcc_opt_passes_t enabled;
    bool verbose;
} mcc_ast_opt_t;
```

### Pass Function Signature

Each optimization pass has the following signature:

```c
typedef int (*mcc_opt_pass_fn)(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
```

The function returns the number of changes made to the AST. The pass manager iterates until no pass makes any changes (fixpoint).

## Pass Descriptions

### O0 Passes (Always On)

#### `normalize`
Canonicalizes the AST structure:
- Reorders commutative operations to put constants on the right
- Normalizes comparison directions

#### `trivial_const`
Simplifies trivial constant expressions:
- `0 + x` → `x`
- `x - 0` → `x`
- `1 * x` → `x`
- `x / 1` → `x`
- `0 / x` → `0` (if x ≠ 0)

#### `identity_ops`
Removes identity operations:
- `x + 0` → `x`
- `x * 1` → `x`
- `x | 0` → `x`
- `x & ~0` → `x`
- `x ^ 0` → `x`
- `x << 0` → `x`
- `x >> 0` → `x`

#### `double_neg`
Removes double negations:
- `--x` → `x` (unary minus)
- `~~x` → `x` (bitwise not)
- `!!x` → `x` (logical not, when used as boolean)

### Og Passes (Debug-Friendly)

#### `copy_prop`
Propagates variable copies:
```c
int x = a;
int y = x + 1;  // becomes: int y = a + 1;
```

Uses symbol pointers for unique identification, correctly handling variable shadowing.

#### `store_load_prop`
Eliminates redundant loads after stores (largely covered by `copy_prop`).

#### `unreachable_after_return`
Removes statements after `return`, `break`, `continue`, or `goto`:
```c
return 0;
x = 5;  // removed - unreachable
```

### O1 Passes (Basic)

#### `const_fold`
Evaluates constant expressions at compile time:
```c
int x = 3 + 5 * 2;  // becomes: int x = 13;
```

Handles:
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`
- Logical: `&&`, `||`, `!`
- Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=`

#### `const_prop`
Propagates known constant values through the code:
```c
int x = 5;
int y = x + 3;  // becomes: int y = 5 + 3;
return y;       // becomes: return 8; (after const_fold)
```

Uses symbol pointers for tracking, correctly handling:
- Variable shadowing in nested scopes
- Multiple functions with same-named variables
- Invalidation at control flow joins

#### `dce` (Dead Code Elimination)
Removes code with no effect:
- Empty statements
- Expression statements with no side effects
- Empty compound statements

#### `dead_store`
Detects stores to variables that are never read (analysis only).

#### `strength_red` (Strength Reduction)
Replaces expensive operations with cheaper ones:
- `x * 2` → `x << 1`
- `x * 4` → `x << 2`
- `x / 2^n` → `x >> n` (for unsigned types only)
- `x % 2^n` → `x & (2^n - 1)` (for unsigned types only)

Uses semantic information (symbol types) to verify unsigned types before applying division/modulo optimizations.

#### `algebraic`
Applies algebraic identities:
- `x - x` → `0`
- `x ^ x` → `0`
- `x & x` → `x`
- `x | x` → `x`

#### `branch_simp`
Simplifies branches with constant conditions:
- `if (1) { A } else { B }` → `A`
- `if (0) { A } else { B }` → `B`
- `while (0) { A }` → (removed)
- `cond ? A : B` with constant `cond` → `A` or `B`

### O2 Passes (Standard)

#### `cse` (Common Subexpression Elimination)
Identifies redundant computations within basic blocks:
```c
int a = x + y;
int b = x + y;  // Detected as common subexpression
```

Features:
- Tracks expressions by structural equality
- Uses symbol pointers for variable comparison
- Invalidates on function calls and control flow
- Only considers expressions without side effects

#### `licm` (Loop-Invariant Code Motion)
Stub - LICM is better implemented at IR level where SSA form is available.

#### `loop_simp` (Loop Simplification)
Simplifies loop structures:
- `while(0) { body }` → removed
- `do { body } while(0)` → `body` (single execution)
- `for(init; 0; incr) { body }` → `init` (condition always false)

#### `tail_call` (Tail Call Detection)
Detects tail calls for backend optimization:
- Identifies `return f(args);` patterns
- Detects recursive tail calls
- Marks calls for potential jump optimization by backend

#### `inline_small` (Small Function Inlining)
Identifies small functions suitable for inlining:
- Maximum 5 statements
- Maximum 4 parameters
- No recursion
- No loops in body
- No goto/labels

### O3 Passes (Aggressive)

#### `loop_unroll` (Loop Unrolling)
Detects loops suitable for unrolling:
- Pattern: `for (i = 0; i < N; i++)` where N ≤ 8
- Body with ≤ 10 statements
- No break/continue in body
- Unit increment only

#### `inline_aggr` (Aggressive Inlining)
More aggressive inlining with higher thresholds:
- Maximum 15 statements
- Maximum 8 parameters

#### `vectorize`
Stub - ANVIL does not currently support vectorization.

## Integration

### Pipeline Integration

The AST optimizer is integrated into `main.c` after semantic analysis:

```c
/* After semantic analysis */
mcc_sema_analyze(sema, ast);

/* AST Optimization */
mcc_ast_opt_t *ast_opt = mcc_ast_opt_create(ctx);
mcc_ast_opt_set_level(ast_opt, ctx->options.opt_level);
mcc_ast_opt_set_sema(ast_opt, sema);
mcc_ast_opt_set_verbose(ast_opt, ctx->options.verbose);
mcc_ast_opt_run(ast_opt, ast);
mcc_ast_opt_destroy(ast_opt);

/* Code generation */
mcc_codegen_generate(cg, ast);
```

### Verbose Output

With `-v` flag, the optimizer prints detailed information:

```
AST Optimization (level 2):
Iteration 1:
  [run] normalize
  [run] trivial_const
  [run] identity_ops
  [run] double_neg
  [run] copy_prop
  [run] store_load_prop
  [run] unreachable_after_return
  [run] const_fold
    -> 2 change(s)
  [run] const_prop
    -> 5 change(s)
  [run] dce
  [run] dead_store
  [run] strength_red
    -> 1 change(s)
  [run] algebraic
  [run] branch_simp
Iteration 2:
  ...
Optimization complete: 8 total changes in 2 iterations
```

## API Reference

### Public API (`include/ast_opt.h`)

```c
/* Create/destroy optimizer */
mcc_ast_opt_t *mcc_ast_opt_create(mcc_context_t *ctx);
void mcc_ast_opt_destroy(mcc_ast_opt_t *opt);

/* Configuration */
void mcc_ast_opt_set_level(mcc_ast_opt_t *opt, mcc_opt_level_t level);
void mcc_ast_opt_set_sema(mcc_ast_opt_t *opt, mcc_sema_t *sema);
void mcc_ast_opt_set_verbose(mcc_ast_opt_t *opt, bool verbose);

/* Pass control */
void mcc_ast_opt_enable_pass(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass);
void mcc_ast_opt_disable_pass(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass);
bool mcc_ast_opt_pass_enabled(mcc_ast_opt_t *opt, mcc_opt_pass_id_t pass);

/* Run optimization */
int mcc_ast_opt_run(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
```

### Helper Functions (`src/opt/opt_internal.h`)

```c
/* Constant expression evaluation */
bool opt_is_const_expr(mcc_ast_node_t *expr);
bool opt_eval_const_int(mcc_ast_node_t *expr, int64_t *result);
bool opt_eval_const_float(mcc_ast_node_t *expr, double *result);

/* AST node creation */
mcc_ast_node_t *opt_make_int_lit(mcc_ast_opt_t *opt, int64_t value, mcc_location_t loc);
mcc_ast_node_t *opt_make_float_lit(mcc_ast_opt_t *opt, double value, mcc_location_t loc);

/* Side effect analysis */
bool opt_has_side_effects(mcc_ast_node_t *expr);

/* Expression comparison */
bool opt_exprs_equal(mcc_ast_node_t *a, mcc_ast_node_t *b);

/* AST traversal */
typedef bool (*mcc_opt_visitor_fn)(mcc_ast_opt_t *opt, mcc_ast_node_t *node, void *data);
void opt_visit_preorder(mcc_ast_opt_t *opt, mcc_ast_node_t *ast, mcc_opt_visitor_fn fn, void *data);
void opt_visit_postorder(mcc_ast_opt_t *opt, mcc_ast_node_t *ast, mcc_opt_visitor_fn fn, void *data);
```

## Adding a New Pass

1. **Define pass ID** in `include/ast_opt.h`:
   ```c
   typedef enum {
       /* ... existing passes ... */
       MCC_OPT_PASS_MY_NEW_PASS,
       MCC_OPT_PASS_COUNT
   } mcc_opt_pass_id_t;
   ```

2. **Declare pass function** in `src/opt/opt_internal.h`:
   ```c
   int opt_pass_my_new_pass(mcc_ast_opt_t *opt, mcc_ast_node_t *ast);
   ```

3. **Register pass** in `src/opt/ast_opt.c`:
   ```c
   static const mcc_opt_pass_info_t pass_info[] = {
       /* ... existing passes ... */
       [MCC_OPT_PASS_MY_NEW_PASS] = { "my_new_pass", MCC_OPT_BASIC },
   };
   
   static mcc_opt_pass_fn pass_funcs[] = {
       /* ... existing passes ... */
       [MCC_OPT_PASS_MY_NEW_PASS] = opt_pass_my_new_pass,
   };
   ```

4. **Enable pass** in appropriate level initializer:
   ```c
   static void init_o1_passes(mcc_opt_passes_t *passes) {
       init_og_passes(passes);
       /* ... existing passes ... */
       MCC_OPT_PASSES_SET(passes, MCC_OPT_PASS_MY_NEW_PASS);
   }
   ```

5. **Implement pass** in appropriate source file:
   ```c
   int opt_pass_my_new_pass(mcc_ast_opt_t *opt, mcc_ast_node_t *ast)
   {
       int changes = 0;
       /* Implementation */
       return changes;
   }
   ```

6. **Update Makefile** (`mk/rules.mk`):
   ```makefile
   $(OBJDIR)/opt_my_file.o: $(OPT_INTERNAL_H) $(INCDIR)/mcc.h
   ```

## Limitations

- **VAR_DECL tracking**: Variables declared with initializers (`int x = 5;`) cannot be tracked directly because `VAR_DECL` nodes don't have symbol pointers. Tracking works for assignments (`x = 5;`).

- **Compound assignments**: Compound assignments (`+=`, `-=`, `*=`, etc.) invalidate the variable's tracked value since the result depends on the current value. Only simple assignments (`=`) with constant RHS are tracked.

- **Control flow**: The optimizer uses a simplified dataflow analysis that invalidates all tracked values at control flow joins (if/else, loops). More sophisticated analysis would improve optimization opportunities.

- **Detection vs transformation**: Some passes (CSE, tail_call, inline, loop_unroll) currently detect optimization opportunities but don't perform full AST transformations. Full implementation would require AST cloning and variable substitution infrastructure.

- **LICM stub**: Loop-invariant code motion is a stub because it's better implemented at IR level with SSA form.

- **Vectorization stub**: ANVIL does not currently support vectorization instructions.

## Examples

### Constant Propagation and Folding

Input:
```c
int main(void) {
    int x;
    x = 5;
    int y;
    y = x + 3;
    return y;
}
```

After optimization (`-O1`):
```c
int main(void) {
    int x;
    x = 5;
    int y;
    y = 8;      // const_prop: x→5, const_fold: 5+3→8
    return 8;   // const_prop: y→8
}
```

### Copy Propagation

Input:
```c
int main(void) {
    int a;
    a = 10;
    int b;
    b = a;
    int c;
    c = b;
    return c;
}
```

After optimization (`-Og`):
```c
int main(void) {
    int a;
    a = 10;
    int b;
    b = a;
    int c;
    c = a;      // copy_prop: b→a
    return a;   // copy_prop: c→a
}
```

### Branch Simplification

Input:
```c
int main(void) {
    if (1) {
        return 42;
    } else {
        return 0;
    }
}
```

After optimization (`-O1`):
```c
int main(void) {
    return 42;  // branch_simp: if(1) → then branch
}
```

### Strength Reduction (Unsigned)

Input:
```c
unsigned int test(unsigned int x) {
    unsigned int a;
    a = x / 8;
    unsigned int b;
    b = x % 16;
    return a + b;
}
```

After optimization (`-O1`):
```c
unsigned int test(unsigned int x) {
    unsigned int a;
    a = x >> 3;   // strength_red: x/8 → x>>3 (unsigned)
    unsigned int b;
    b = x & 15;   // strength_red: x%16 → x&15 (unsigned)
    return a + b;
}
```

### Loop Simplification

Input:
```c
int main(void) {
    int x = 0;
    while (0) { x = 1; }      // Never executes
    do { x = 5; } while (0);  // Executes once
    return x;
}
```

After optimization (`-O2`):
```c
int main(void) {
    int x = 0;
    // while(0) removed
    x = 5;        // do-while(0) → single execution
    return x;
}
```
