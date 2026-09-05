# ARM64 Backend Refactoring Plan

## Status: Reference MachineIR Backend

ARM64 is the **reference implementation** of ANVIL's MachineIR pipeline. The
shared contract it established is now followed by the x86, x86-64, and PowerPC
backends as well:

```
lower source IR -> MachineIR
  -> verify-legal
  -> coalesce_copies            (anvil_mir_coalesce_copies)
  -> verify-legal
  -> linear-scan regalloc       (anvil_regalloc_linear_scan_classes)
  -> materialize_spills         (anvil_mir_materialize_spills)
  -> verify-legal
  -> emit assembly
```

Each target provides its own `anvil_<arch>_lower_func_to_mir`,
`anvil_<arch>_verify_mir_legal`, `anvil_<arch>_regalloc_mir`, and
`anvil_<arch>_emit_mir_abi`; the coalescing, linear-scan allocation, and
spill-materialization passes are target-independent and live in `src/machine/`
(`machine_ir.c`, `regalloc.c`). ARM64 remains the canonical example of the
contract — see the `x86_32_lower.c` / `x86_64_lower.c` and `asm/gas32.c` / `asm/gas64.c` components under `src/backend/x86/`
for the x86 implementations that mirror it.

## Current implementation notes

ARM64 no longer has a target-specific source-IR preparation or peephole
directory. The former passes could mutate observable loads and stores without
the IR mutation and alias contracts required to prove those rewrites correct,
so the backend exposes no `prepare_ir` hook. Source-level dead-store,
load-elimination, and control-flow transformations belong to the generic pass
manager in `src/opt/`, where their analyses and invalidation are shared and
tested.

Target lowering preserves every observable `LOAD` and `STORE`, then uses the
verified MachineIR pipeline above. MachineIR carries explicit register class,
width, CFG ownership, ABI live-ins, call bundles, spill slots, and all sixteen
floating-point comparison predicates. `i1` occupies one byte in memory and is
normalized to 0/1 at casts, loads, stores, comparisons, parameters, and returns.

Assembly-level improvements must be implemented after legal MachineIR lowering
or as a formally verified MachineIR transform. There is deliberately no dormant
ARM64 pass API advertising transformations that are not safe to run.

### Variadic Function Calls (Darwin/macOS)
- **Problem**: `printf` and other variadic functions were receiving incorrect arguments
- **Solution**: On Darwin/macOS, variadic arguments are now passed on the stack as required by AAPCS64
- **Implementation**: `arm64_emit_call()` detects variadic functions via `type->data.func.variadic` and allocates stack space for variadic args

### Global Variable Initializers
- **Array initializers**: Full support for emitting initialized arrays with `.byte`, `.short`, `.long`, `.quad` directives
- **Float/double initializers**: Floating-point constants emitted using bit representation via `memcpy` to preserve exact values
- **Float arrays**: Proper handling of float/double element types in array initializers

### Type-Aware Load/Store
- **Sign-extending loads**: `ldrsb`, `ldrsh`, `ldrsw` for signed types to preserve sign in 64-bit registers
- **Correct store sizes**: Store instructions now use source value type size, fixing corruption of adjacent array elements
- **Unsigned types**: Proper `is_unsigned` flag handling from MCC type system

### Multi-dimensional Array Access
- **Problem**: Stores to 2D array elements were corrupting adjacent elements
- **Root cause**: Store size was determined from pointer type (which could be `ptr<[N x T]>`) instead of value type
- **Solution**: `arm64_emit_store()` now uses source operand type for size determination

## Current Architecture (Working)

### Register Usage (MachineIR allocator)

The current backend (`arm64_mir.c`) lets the shared linear-scan allocator assign
physical registers from callee-saved-only pools; the older fixed-register scheme
below it has been removed.

- **x19-x28 / v8-v15**: allocatable pools handed to
  `anvil_regalloc_linear_scan_classes` (callee-saved only, so allocated values
  survive calls — the allocator has no call-clobber model).
- **x12-x15 / v16-v19**: scratch pools used by spill materialization.
- **x0-x7 / v0-v7**: ABI argument/return registers, modeled as fixed MachineIR
  vregs and copied into allocatable vregs before allocation.
- **x16**: scratch register for large offsets and indirect-call targets
  (legalized to `x16` -> `blr x16`).
- **x29**: Frame pointer (FP)
- **x30**: Link register (LR)
- **sp**: Stack pointer

### Stack Frame Layout
```
[Higher addresses]
+------------------+
| Saved LR (x30)   | <- x29 + 8
+------------------+
| Saved FP (x29)   | <- x29 (frame pointer)
+------------------+
| Local var 1      | <- x29 - 8
+------------------+
| Local var 2      | <- x29 - 16
+------------------+
| ...              |
+------------------+
| Spill slots      |
+------------------+ <- sp
[Lower addresses]
```

## Historical Notes

The items below describe limitations of the older direct ARM64 emitter. The
current ARM64 backend uses `arm64_mir.c` as the stable MachineIR reference path:
source IR is lowered to virtual registers, ABI constraints are modeled as fixed
registers, the shared linear-scan allocator assigns physical registers, spills
are materialized by MachineIR, and final assembly is emitted from allocated MIR.

## Superseded Limitations From The Old Direct Emitter

### 1. Inefficient Register Usage
- Only uses x0 for results, x9-x15 as temporaries
- Wastes callee-saved registers (x19-x28)
- Every SSA value is spilled to stack immediately

### 2. Stack Frame Optimization
- Stack size calculated in first pass but not optimized
- No consideration for value liveness
- All instruction results are saved even if never used again
- The old emitter had no register allocation and used a stack-only approach

### 3. Code Quality Issues
- Redundant load/store sequences
- No peephole optimization

### 4. Missing Features
- No callee-saved register preservation when needed
- Limited floating-point register usage

## Proposed Architecture (Historical — superseded)

> **Note:** The sections below were the original refactoring plan. They have been
> realized differently from the sketch: instead of an ARM64-private allocator with
> the `arm64_value_loc_t` / `arm64_reg_state_t` structures shown here, ARM64 now
> lowers to MachineIR and uses the **shared** linear-scan allocator
> (`anvil_regalloc_linear_scan_classes`) and spill passes in `src/machine/`. The
> data structures and per-phase plan are kept for historical context only and do
> not match the current code.

### Phase 1: Better Stack Frame Management
1. Pre-calculate exact stack requirements
2. Track which values actually need stack slots
3. Proper alignment for all types

### Phase 2: Simple Register Allocation
1. Use x19-x28 for frequently used values
2. Implement basic linear scan or graph coloring
3. Only spill when necessary

### Phase 3: Code Generation Improvements
1. Use correct register sizes (w vs x)
2. Combine load-use patterns
3. Better immediate handling

## Implementation Plan

### New Data Structures

```c
/* Value location tracking */
typedef enum {
    ARM64_LOC_NONE,      /* Not yet assigned */
    ARM64_LOC_REG,       /* In a register */
    ARM64_LOC_STACK,     /* On the stack */
    ARM64_LOC_CONST,     /* Constant value */
} arm64_loc_kind_t;

typedef struct {
    arm64_loc_kind_t kind;
    union {
        int reg;         /* Register number */
        int stack_off;   /* Stack offset from FP */
        int64_t imm;     /* Immediate value */
    };
    int size;            /* Size in bytes */
    bool dirty;          /* Needs writeback to stack */
} arm64_value_loc_t;

/* Register state */
typedef struct {
    anvil_value_t *value;  /* Current value in register, or NULL */
    bool callee_saved;     /* Is this a callee-saved register? */
    bool in_use;           /* Currently allocated */
} arm64_reg_state_t;

/* Enhanced backend state */
typedef struct {
    /* ... existing fields ... */
    
    /* Register allocation */
    arm64_reg_state_t gpr_state[32];
    arm64_reg_state_t fpr_state[32];
    
    /* Value locations */
    arm64_value_loc_t *value_locs;
    size_t num_value_locs;
    
    /* Callee-saved registers used */
    uint32_t used_callee_saved;
    
    /* Stack frame layout */
    int locals_size;       /* Size for local variables */
    int spill_size;        /* Size for register spills */
    int outgoing_args;     /* Size for outgoing call arguments */
    int total_frame_size;  /* Total aligned frame size */
} arm64_backend_t;
```

### Register Classes

```
Argument registers:     x0-x7   (caller-saved)
Temporary registers:    x9-x15  (caller-saved)
Intra-procedure:        x16-x17 (IP0, IP1 - scratch)
Platform register:      x18     (reserved)
Callee-saved:           x19-x28 (must preserve)
Frame pointer:          x29     (FP)
Link register:          x30     (LR)
Stack pointer:          sp      (x31)
```

### Code Generation Flow

1. **Analysis Pass**
   - Count instructions and values
   - Identify live ranges
   - Determine which values need stack slots
   - Calculate maximum call arguments

2. **Register Allocation Pass**
   - Assign callee-saved registers to long-lived values
   - Assign temporaries to short-lived values
   - Determine spill slots

3. **Prologue Generation**
   - Save used callee-saved registers
   - Allocate stack frame
   - Save incoming parameters if needed

4. **Code Generation**
   - Emit instructions using allocated registers
   - Handle spills/reloads as needed

5. **Epilogue Generation**
   - Restore callee-saved registers
   - Deallocate stack frame
   - Return

## ABI Differences: Linux vs Darwin

### Linux (AAPCS64)
- No symbol prefix
- `.type` and `.size` directives
- GOT/PLT for PIC
- `:lo12:` and `:got:` relocations

### Darwin (Apple ARM64)
- `_` symbol prefix
- No `.type`/`.size` directives
- `@PAGE` and `@PAGEOFF` relocations
- Different section names

## Testing Strategy

1. Compile examples with both ABIs
2. Assemble generated code
3. Link and run on actual hardware/emulator
4. Compare output with reference implementation
