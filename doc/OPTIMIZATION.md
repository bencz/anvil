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
| O2 | `ANVIL_OPT_STANDARD` | O1 + simplify_cfg, strength_reduce, dead_store, load_elim, cse, switch-table, inline (small bodies, internal or external and not preemptible, and internal single call sites), loop-vectorize; unrolling |
| O3 | `ANVIL_OPT_AGGRESSIVE` | O2 + larger inlining budgets, vectorize (SLP) |

The two vectorizers create vector operations only where the backend's
`vector_operation_cost` hook accepts them for the CPU model.  Floating-point
vectors follow `anvil_func_set_fp_vectorization`, on by default (MCC:
`-fno-fp-vectorize` turns it off): lane-wise arithmetic gives the same results
as the scalar loop, the policy exists for exception-flag and denormal
behaviour.  Reductions (a vectorized sum) also need
`anvil_func_set_fp_reassociation`, off by default.  Inlining copies a callee
only into a caller with the same floating-point policies, so the inlined code
keeps its own.

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

`src/opt/dce.c`. Mark and sweep: the instructions that must run mark the
definitions they read, transitively, and every unmarked instruction is
removed. Cycles die too: a PHI that only its own update reads, such as a
loop-carried sum nobody uses after the loop.

**Rules:**
- Roots: instructions without a result (stores, branches, returns), calls with
  effects, divisions (they may trap), floating-point operations, atomics, and
  loads, unless the load is not volatile and its whole range lies inside a
  static alloca or a global that exists (not a weak declaration), so it
  cannot fault
- NOP instructions (left by other passes) are always removed
- Every allocation precedes the sweep, which allocates nothing

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
| `x / 8` (signed) | `(x + ((x >> 31) & 7)) >> 3` | Signed, power of 2 (bias rounds toward zero) |
| `x / 10` (unsigned) | `umulh(x, 0xCCCCCCCD) >> 3` | Any other constant, where the target prices `UMULH` |
| `x / 7` (signed) | `q = smulh(x, m) + x; q >>= 2; q += q >>u 31` | Any other constant (`|d| >= 2`), where the target prices `SMULH` |
| `x % d` | `x - (x / d) * d` | As the division |
| `a * b + c` (FP) | `fma(a, b, c)` | `anvil_func_set_fp_contraction` and a target FMA |
| `lo <= x && x <= hi` | `(x - lo) u<= hi - lo` | Constants `lo <= hi` (an i1 `and` or the select forms) |
| `ext(a) == ext(b)`, `sext(c) != C` | `a == b`, `c != C` | Extensions of one narrow type; a constant the narrow type represents |
| `sext(a) < sext(b)` / `zext(a) u< zext(b)` | `a < b` / `a u< b` | The extension's kind matches the order |
| `zext(a) < zext(b)` (signed) | `a u< b` | Unsigned narrow values |

Division by a constant uses the Granlund-Montgomery reciprocal with the magic
numbers of Hacker's Delight (10-1 signed, 10-2 unsigned with the "add"
fix-up) at the operand width. It needs the high half of a product
(`UMULH`/`SMULH`), so it applies only where the backend's cost hook prices
that operation for the type (`ANVIL_VECTOR_SHAPE_SCALAR`): 32 and 64 bits on
x86-64, ARM64 and 64-bit PowerPC, 32 bits on i386 and ppc32; SystemZ keeps
the division.

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

`src/opt/load_elim.c`. A load takes the value of a load or store of the same
address (`anvil_memory_alias` says MUST) earlier in its block or in a
dominating one, when every path between them preserves it: no volatile
access, no store that may alias, no call that may write memory or be
observable. Nothing writes a read-only global (`anvil_memory_read_only`,
through any address arithmetic), so stores and calls never invalidate a load
of one.

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

### Loop-Invariant Code Motion (`ANVIL_PASS_LICM`) - O2

`src/opt/licm.c`. An instruction in a loop whose operands are all defined
outside it moves to the loop's preheader (created when one is missing and
something can move) when it is speculatable (`anvil_opt_speculatable`):
integer arithmetic, compares, selects, casts, address arithmetic, rotates and
shifts by a constant below the width. Divisions, loads, calls and
floating-point operations stay in the loop. An address `gep p, a + b` with
`p` and `a` invariant splits into `gep (gep p, a), b`, so the invariant part
leaves the loop.

### Complete Unrolling (`ANVIL_PASS_UNROLL`) - O2

`src/opt/unroll.c`. A loop of one header and one body block becomes straight
code when its trip count is proven and small. The header has PHIs,
pure work (`anvil_opt_pure`), a compare and the branch; the
body ends with the back edge. Its counter starts at a constant, is compared
with a constant, and steps by a constant. The step may be direct
(`i + c`, `i - c`) or done on the counter widened and truncated back
(`trunc(zext(i) + c)`, `trunc(sext(i) - c)`: a narrow C counter promoted to
`int`), which is the same modulo its width.

Limits: at most 8 iterations and 32 instructions of header work and body
together, and the copies must stay within 128 instructions. Each iteration copies the header's work and
then the body's. After the last iteration the header's work runs once more,
on the final counter, for the values the loop's readers take after it. Every
copy is made before the loop is replaced. Larger counted loops are unrolled
partially at run time (`ANVIL_PASS_RUNTIME_UNROLL`).

### Loop Strength Reduction (`ANVIL_PASS_LOOP_STRENGTH`) - O2+

`src/opt/loop_strength.c`. In a loop whose induction variable steps by one,
an address `gep T base, i, ...` with a loop-invariant base and a `T` whose
size no scaled index absorbs (not 1, 2, 4 or 8) becomes an access through a
pointer that starts at `gep T base, start` and advances one `T` in the
latch, so no multiply runs per iteration. Indices narrower than a pointer
are extended by their signedness; the rewrite needs the header's `i < n`
(signed or unsigned as `i` is) to show the index cannot wrap in the body.
A narrow IV stepped by a constant or by a loop-invariant no-wrap amount is
widened first. After the pipeline (`anvil_opt_loop_strength_late`) streams
also replace indexes that fold where the target's accesses advance their
base and either nothing else reads the IV (it is rebuilt after the loop
from a stream) or only the loop test does on a target with counter branches
(`anvil_arch_info_t.counter_branches`: the IV is left a pure counter, PowerPC
CTR, SystemZ BRCTG); counted loops then count to zero. Pointers advancing in
lockstep (two or more header PHIs stepped by the same bytes, read in the
loop only as addresses or through a constant displacement the target folds)
become `p0 + i` over one byte index where an index folds into the access and
no access advances its own base (x86, the mainframes): one add per iteration
for all of them.

### Loop Tables (`ANVIL_PASS_LOOP_TABLE`) - O2

`src/opt/loop_table.c`. A loop (after complete unrolling, also O2) that
computes from constants one integer per iteration and stores it to
base[iteration], with no loads, calls or other stores, runs at compile time;
its values become a read-only table the store copies from.

### Runtime Unrolling (`ANVIL_PASS_RUNTIME_UNROLL`) - O2

`src/opt/runtime_unroll.c`. A loop of a header and a body counting up to
zero (after loop strength reduction) with a small body and no calls runs,
where `anvil_arch_info_t.small_loop_unroll` asks for it (PowerPC), a loop of
U body copies per test first (its own counter, values stepped by a constant
c as v + k*c, addresses as (base + v) + k*c), then the original loop for the
remaining iterations.

### Loop Vectorization (`ANVIL_PASS_LOOP_VECTORIZE`) - O2

`src/opt/loop_vectorize.c`. An innermost counted loop

```
header: %i = phi [%start, preheader], [%i + 1, body]
        br (%i < %n | %i u< %n | %i != %n), body, exit
body:   straight-line lane-wise arithmetic over base[..., %i] accesses
```

gets a vector copy (`vec.header`/`vec.body`, 16-byte vectors of the element
type) that runs while a whole vector of iterations remains; the original loop,
entered with the vector index, runs the rest. Accesses are GEPs whose last
index is the loop index (or its extension) and whose other operands are
invariant, so consecutive iterations are adjacent. Invariant operands become
one `splat` in the preheader. Streams that are stored through and whose
distance is unknown are checked once in the preheader: when two first-iteration
addresses are closer than one vector (and not equal), the vector loop runs zero
iterations. The original header is marked `vectorized` so later runs leave the
remainder loop alone. Every operation's legality and cost come from
`vector_operation_cost`; the pass knows no target.

A loop with accumulators whose body is one small block runs two copies of the
vector body per iteration (interleaving), each with its own accumulators,
combined after the loop: the next copy's additions do not wait for the
previous one's. It does so when every accumulator starts from its operation's
identity (not min/max) and the copies' accumulators and temporaries fit the
target's FP/vector registers (`anvil_arch_info_t.num_fpr`); the vector trip
count is then a multiple of twice the lanes.

An *open search* has no trip test: its header reads one stream and leaves on
a comparison of the element, as in a scan for a terminator or a delimiter in
any language:

```
header: %i = phi [%start, preheader], [%i + 1, latch]
        %x = load base[%i]; %c = cmp %x, %k
        br %c, latch, exit
latch:  br header
```

A loop walking a pointer instead (`p = phi [base, p + 1]`, read through `p`)
first becomes `base + i`, where the target compares vectors of the element.
The vector loop has no end; it runs until a vector holds a leaving lane, then
resumes the scalar loop at that vector, which finds the exact element. It
reads only aligned vectors, from the one holding `base[start]`: an aligned
vector lies within one protection block (`access_fault_granularity` at least
the vector size), and that block holds an element the scalar loop would read,
so the vector loop faults only where the scalar one would. The first vector's
lanes before the start are masked off (lane numbers, a constant vector,
compared with their count); the mask is a loop-carried value, all lanes after
the first vector. The vector loop runs when the start is element-aligned and
`start - off` does not wrap the index, otherwise the scalar loop runs alone.
Only one read-only stream, no stores and no accumulators.

### Known Bits (`ANVIL_PASS_KNOWN_BITS`) - O2

`src/opt/known_bits.c`. For every integer value, it records which bits are
known zero and which are known one. The facts come from AND, OR, XOR, NOT,
ADD and SUB, truncations and extensions, shifts by constants, compares, PHIs
and selects, and bitcasts between integers of one width. They are solved to a
fixpoint over a worklist. A value whose bits are all known becomes a constant.
For a value that is not fully known:
- a signed operation whose operand has its sign bit known zero becomes its
  unsigned form: `sext` → `zext`, `sar` → `shr`, `sdiv` by 2^k → `shr` by
  k, `smod` by 2^k → `and` with 2^k − 1;
- an AND whose constant clears only bits already known zero is replaced by
  its operand.

C code that promotes a byte or a masked value to `int` and divides, reduces
or shifts it then needs no sign fix-up.

### Scalar Replacement of Aggregates (`ANVIL_PASS_SROA`) - O2

`src/opt/sroa.c`. A struct or array allocated in the entry block splits into
one scalar allocation per accessed field, which mem2reg then promotes. Every
use of its address must be a constant-index GEP or STRUCT_GEP, a pointer
BITCAST (same offset), or a non-volatile scalar load or store. Any other use
(an escape, an aggregate access, a variable index) keeps the object whole.
Accesses at one offset share a slice when their types are equal, or when both
are integers of the same size (not `i1`). A copy of a struct in words then
reads the fields that its code writes. Such an access whose type is not its
slice's goes through a value BITCAST: a load becomes a load of the slice type
plus a bitcast, a store stores the bitcast value. Partly overlapping accesses
keep the object whole. Every instruction and allocation precedes the rewrite.

### Switch Tables (`ANVIL_PASS_SWITCH_TABLE`) - O2

`src/opt/switch_table.c`. A switch of at least four cases that only chooses
values becomes a table lookup. Either every destination forwards to one merge
block whose PHIs take a constant from each, or every destination returns a
constant (a classifying function). The case values must span at most 4096
values with a density of at least 40%. The selector minus the lowest case
indexes one read-only table per chosen value. Holes, and the entry past the
range where an out-of-range index is clamped, hold the default's value, so
no branch remains. A default that is code keeps its own path:
`off u< span ? lookup : default`. Every allocation precedes the rewrite, which
then removes the forwarding blocks with their PHI entries. Other switches
lower as sorted runs and a balanced tree of compares (see
[BACKENDS.md](BACKENDS.md)).

### SLP Vectorization (`ANVIL_PASS_VECTORIZE`) - O3

`src/opt/vectorize.c`. In one block, as many stores of one element type as a
16-byte vector holds, to consecutive addresses of one root (a *seed*), become
one vector store when their values form a tree the target computes lane by
lane. A bundle (one scalar per lane) is a splat (the same value in every
lane, defined before the seed), a vector load (loads of consecutive addresses
of one root), or one operation (integer add/sub/mul/and/or/xor, FP
add/sub/mul/div) on every lane whose operand bundles are again bundles.
Addresses are a root plus a constant offset (`anvil_memory_offset`: GEPs with
constant indices, struct fields, bitcasts; a variable index makes the root).
The tree is computed at the seed's first store. Loads and stores moved there
must not pass an access that may overlap them (`anvil_memory_alias`), a call
with memory effects or a volatile access. Scalars read outside the tree stay,
and a bundle built for an earlier seed of the block is reused. A vector load
is refused where the function stores narrower values into its range through
another path: the load could not take their data forwarded. The cost compares
the vector operations, loads and splats with the scalar instructions and
stores that go away.

### CFG Simplification (`ANVIL_PASS_SIMPLIFY_CFG`)

Simplifies the control flow graph.

**Transformations:**

1. **Constant Branch Folding**: Converts conditional branches with constant conditions to unconditional branches
2. **Empty Block Removal**: Removes blocks that only contain an unconditional branch; a predecessor that already
   reaches the destination gets a second edge unless the destination's PHIs would need two values from it
3. **Block Merging**: Merges a block with its single successor if the successor has only one predecessor
4. **Unreachable Code Removal**: Removes blocks not reachable from the entry block
5. **Jump Threading**: a branch on a PHI of constants (or a compare of one with a constant) sends each predecessor
   bringing a constant straight to the chosen successor; a branch on a condition computed elsewhere sends each
   predecessor whose edge implies the condition (it branches on it, or is reached through single-predecessor blocks
   from one that does) straight to its successor. Pure instructions in the join move into both successors first.
   This keeps a loop that exits on a test from carrying the test's result as a value.
6. **If-conversion**: two-way branches over small speculatable sides (`anvil_opt_speculatable`, shared with LICM: no
   trap, memory or FP-environment effect, and no shift whose count could reach the width, which is undefined) become
   selects

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
| `src/opt/known_bits.c` | Known bits |
| `src/opt/sroa.c` | Scalar replacement of aggregates |

## Not Implemented

- **Tail calls to other functions** (sibling calls): a recursive tail call
  becomes a loop (`ANVIL_PASS_TAIL_RECURSION`), but a call in tail position to
  another function keeps its call and return.
