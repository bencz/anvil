# ARM64 Backend Refactoring Plan

## Current Issues

### 1. Inefficient Register Usage
- Only uses x0 for results, x9-x15 as temporaries
- Wastes callee-saved registers (x19-x28)
- Every SSA value is spilled to stack immediately

### 2. Stack Frame Problems
- Stack size calculated in first pass but not optimized
- No consideration for value liveness
- All instruction results are saved even if never used again
- No register allocation - pure stack-based approach

### 3. Code Quality Issues
- Redundant load/store sequences
- No peephole optimization
- Type-size mismatches (using x registers for i32 values)

### 4. Missing Features
- No callee-saved register preservation when needed
- No proper handling of large immediates in all cases
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
