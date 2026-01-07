# Contributing to ANVIL

## Code Style

- C99 standard
- 4-space indentation
- Opening braces on same line
- Snake_case for functions and variables
- PascalCase for types (with Anvil prefix)
- UPPER_CASE for constants and macros

## Adding New Optimizations

### IR Optimization Pass

1. Add function declaration to `src/opt/ir_opt.h`:
```c
bool anvil_opt_my_pass(AnvilFunc* func, AnvilOptStats* stats);
```

2. Implement in `src/opt/ir_opt.c`:
```c
bool anvil_opt_my_pass(AnvilFunc* func, AnvilOptStats* stats) {
    bool changed = false;
    
    for (AnvilBlock* block = func->entry; block; block = block->next) {
        for (AnvilInst* inst = block->first; inst; inst = inst->next) {
            // Your optimization logic here
            if (/* can optimize */) {
                // Apply optimization
                changed = true;
                if (stats) stats->instructions_removed++;
            }
        }
    }
    
    return changed;
}
```

3. Add to `anvil_opt_run_all()`:
```c
if (opt_level >= 2) {
    changed |= anvil_opt_my_pass(func, stats);
}
```

### MIR Optimization Pass

Similar process in `src/opt/mir_opt.c`.

## Adding a New ABI

1. Create `src/backend/<arch>/abi/newabi.c`:

```c
#include "newabi.h"
#include "../regs.h"

static const int newabi_arg_regs_int[] = { REG_A, REG_B, REG_C, REG_D };
static const int newabi_arg_regs_float[] = { REG_F0, REG_F1, REG_F2, REG_F3 };
static const int newabi_callee_saved[] = { REG_S0, REG_S1, REG_S2 };

static void newabi_classify_argument(const AnvilABI* abi, const AnvilTargetInfo* target,
                                      AnvilType* type, int arg_index, AnvilArgInfo* out) {
    // Classification logic
}

static void newabi_classify_return(const AnvilABI* abi, const AnvilTargetInfo* target,
                                    AnvilType* type, AnvilArgInfo* out) {
    // Return value classification
}

static void newabi_compute_frame_layout(const AnvilABI* abi, const AnvilTargetInfo* target,
                                         AnvilMFunc* func, AnvilFrameLayout* out) {
    // Frame layout computation
}

const AnvilABI arch_newabi = {
    .name = "newabi",
    
    .arg_regs_int = newabi_arg_regs_int,
    .num_arg_regs_int = 4,
    .arg_regs_float = newabi_arg_regs_float,
    .num_arg_regs_float = 4,
    
    .ret_reg_int_lo = REG_A,
    .ret_reg_int_hi = REG_B,
    .ret_reg_float = REG_F0,
    
    .callee_saved_regs = newabi_callee_saved,
    .num_callee_saved = 3,
    
    .stack_alignment = 16,
    .red_zone_size = 0,
    
    .uses_underscore_prefix = false,
    
    .classify_argument = newabi_classify_argument,
    .classify_return = newabi_classify_return,
    .compute_frame_layout = newabi_compute_frame_layout,
};
```

2. Create header `src/backend/<arch>/abi/newabi.h`:
```c
#ifndef ANVIL_ARCH_NEWABI_H
#define ANVIL_ARCH_NEWABI_H

#include "../../backend.h"

extern const AnvilABI arch_newabi;

void newabi_classify_argument(const AnvilABI* abi, const AnvilTargetInfo* target,
                               AnvilType* type, int arg_index, AnvilArgInfo* out);
void newabi_classify_return(const AnvilABI* abi, const AnvilTargetInfo* target,
                             AnvilType* type, AnvilArgInfo* out);

#endif
```

3. Add to backend's `get_abi()` function:
```c
static const AnvilABI* arch_get_abi(int os, const char* abi_name) {
    if (abi_name && strcmp(abi_name, "newabi") == 0) return &arch_newabi;
    // ...
}
```

4. Add to CMakeLists.txt.

## Testing

### Unit Tests
Located in `tests/unit/`. Run with:
```bash
./build/test_arena
./build/test_types
```

### Integration Tests
Create test programs in `examples/` that:
1. Build IR using the API
2. Compile to assembly
3. Assemble and link with clang
4. Run and verify output

Example:
```bash
./build/test_compile  # Generates /tmp/sum.s
clang -c /tmp/sum.s -o /tmp/sum.o
clang examples/test_asm.c /tmp/sum.o -o /tmp/test
/tmp/test  # Should print "sum(10, 32) = 42"
```

## Debugging Tips

1. **Dump IR**: Use `anvil_module_dump_ir(mod, stdout)` to see IR
2. **Check MIR**: Add debug prints in `anvil_lower_module()`
3. **Verify regalloc**: Print vreg-to-preg mappings after allocation
4. **Compare with clang**: Generate reference assembly with `clang -S`

## Pull Request Checklist

- [ ] Code follows style guidelines
- [ ] No `#ifdef` for target-specific code (use vtables)
- [ ] All tests pass
- [ ] New features have tests
- [ ] Documentation updated
- [ ] No memory leaks (use arena allocation)
