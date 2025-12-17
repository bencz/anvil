# ARM64 Backend Refactoring Plan

## Recent Improvements (Implemented)

The ARM64 backend has received significant improvements for correctness, robustness, and optimization:

### Architecture-Specific Optimizations
New optimization pass infrastructure in `src/backend/arm64/opt/`:
- **Pass Manager**: `arm64_opt.c` coordinates all optimization passes
- **Peephole Optimizations**: Redundant store elimination, load-store same address removal
- **Dead Store Elimination**: Remove stores that are immediately overwritten
- **Redundant Load Elimination**: Reuse values already loaded from same address
- **Branch Optimization**: Combine cmp+cset+cbnz into cmp+b.cond, use cbz/cbnz/tbz/tbnz
- **Immediate Optimization**: Use immediate forms of instructions when possible

### Conditional Branch Optimization
The `arm64_emit_br_cond()` function now detects when the condition is a comparison result and emits `cmp` + `b.cond` directly instead of loading the boolean result:

**Before:**
```asm
cmp x9, x10
cset x0, le
strb w0, [stack]
ldrsb x9, [stack]
cbnz x9, .body
```

**After:**
```asm
cmp x9, x10
b.le .body
```

Supported condition codes: `eq`, `ne`, `lt`, `le`, `gt`, `ge`, `lo`, `ls`, `hi`, `hs` (unsigned).

### IR Preparation Phase
New `prepare_ir` callback in backend interface:
- Called automatically by `anvil_module_codegen()` before code generation
- Analyzes all functions (detect leaf functions, calculate stack layout)
- Runs architecture-specific optimizations via `arm64_opt_module()`

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

### Register Usage
- **x0**: Primary result register
- **x9-x15**: Temporary registers for operand loading
- **x16**: Scratch register for large offsets (>255 bytes)
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

## Future Improvements (Planned)

### 1. Inefficient Register Usage
- Only uses x0 for results, x9-x15 as temporaries
- Wastes callee-saved registers (x19-x28)
- Every SSA value is spilled to stack immediately

### 2. Stack Frame Optimization
- Stack size calculated in first pass but not optimized
- No consideration for value liveness
- All instruction results are saved even if never used again
- No register allocation - pure stack-based approach

### 3. Code Quality Issues
- Redundant load/store sequences
- No peephole optimization

### 4. Missing Features
- No callee-saved register preservation when needed
- Limited floating-point register usage

## Proposed Architecture

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
