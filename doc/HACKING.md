# ANVIL Hacking Guide

This guide is for developers who want to contribute to ANVIL or understand its internals.

## Getting Started

### Prerequisites

- C compiler (GCC or Clang)
- Make
- Basic understanding of compiler design
- Familiarity with at least one target architecture

### Building

```bash
# Clone and build (builds the static lib + simple examples)
git clone <repository>
cd anvil
make                 # == make lib examples

# Just the library
make lib             # produces lib/libanvil.a

# Build and run the regression test suite
make tests

# Runtime examples: generate asm, assemble/link, and execute it
make examples-runtime
make test-examples

# Advanced examples (fp_math_lib, dynamic_array, base64_lib)
make examples-advanced
make test-examples-advanced

# Clean build
make clean && make
```

**Build flags:** `CFLAGS` in the `Makefile` is

```make
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -I./include -g -O2
```

The `-D_GNU_SOURCE` define is **required**: ANVIL compiles with `-std=c11`, under
which `strdup` (and a few other POSIX functions) are not declared by default. If you
add a custom build invocation or a separate compile step, keep `-D_GNU_SOURCE` or you
will get implicit-declaration warnings/errors for `strdup`.

**Make targets:**

| Target | Description |
|--------|-------------|
| `all` | `lib` + `examples` (default) |
| `lib` | Build `lib/libanvil.a` |
| `examples` | Build the simple example programs under `build/examples/` |
| `tests` | Build and run the regression tests (see Testing) |
| `examples-runtime` / `test-examples` | Build/run the executable examples in `examples/basic_runtime` |
| `examples-advanced` / `test-examples-advanced` | Build/run `fp_math_lib`, `dynamic_array`, `base64_lib` |
| `install` | Install headers + lib to `/usr/local` |
| `clean` | Remove `build/` and `lib/` |

### Project Structure

```
anvil/
├── include/anvil/
│   ├── anvil.h              # Public API
│   ├── anvil_internal.h     # Internal structures
│   ├── anvil_cpu.h          # CPU models + feature flags
│   ├── anvil_debug.h        # IR dump/print API
│   ├── anvil_opt.h          # Optimization API
│   ├── anvil_machine.h      # MachineIR + register allocation API
│   ├── anvil_arm64_mir.h    # ARM64 MIR lower/verify/regalloc/emit entry points
│   ├── anvil_x86_64_mir.h   # x86-64 MIR entry points
│   ├── anvil_x86_mir.h      # x86 MIR entry points
│   ├── anvil_ppc_mir.h      # Shared PPC MIR entry points
│   └── anvil_mainframe_mir.h# Shared mainframe MIR entry points
├── src/
│   ├── core/                # Core library
│   │   ├── context.c        # Context management
│   │   ├── cpu_table.c      # CPU model/feature table
│   │   ├── types.c          # Type system
│   │   ├── module.c         # Module management
│   │   ├── function.c       # Function management
│   │   ├── value.c          # Values and instructions
│   │   ├── builder.c        # IR builder
│   │   ├── strbuf.c         # String buffer
│   │   ├── backend.c        # Backend registry
│   │   ├── ir_dump.c        # IR dump/print
│   │   └── verify.c         # Source-IR verifier
│   ├── machine/             # Target-independent MachineIR infrastructure
│   │   ├── machine_ir.c     # MachineIR builder/queries
│   │   └── regalloc.c       # Linear-scan register allocation + spilling
│   ├── opt/                 # IR optimization passes
│   └── backend/             # Target backends
│       ├── common/                   # Shared target-independent backend utilities
│       │   ├── anvil_slot_map.h
│       │   └── gnu_data.c, gnu_data.h # GAS data emission with symbol callback
│       ├── x86/                      # x86 family (32-bit and 64-bit)
│       │   ├── x86_32_lower.c, x86_64_lower.c
│       │   ├── x86_32_legal.c, x86_64_legal.c
│       │   ├── x86_32_codegen.c, x86_64_codegen.c
│       │   ├── x86_32_helpers.c, x86_64_helpers.c
│       │   ├── x86_32_internal.h, x86_64_internal.h
│       │   ├── targets/              # Target identity and vtables
│       │   ├── abi/                  # Calling conventions and varargs
│       │   └── asm/                  # GAS emission per execution mode
│       ├── systemz/                  # S/370 lineage, distinct ISA descriptors
│       │   ├── systemz_lower.c, systemz_legal.c, systemz_codegen.c
│       │   ├── systemz_target.c, systemz_internal.h
│       │   ├── targets/              # S/370, XA, S/390, z/Architecture
│       │   ├── abi/                  # MVS-oriented arena linkage policies
│       │   └── asm/                  # HLASM instructions/data/symbols
│       ├── ppc/                      # PowerPC family
│       │   ├── ppc_lower.c, ppc_legal.c, ppc_emit.c, ppc_codegen.c
│       │   ├── ppc_target.c, ppc_internal.h
│       │   ├── targets/              # PPC32, PPC64 BE, PPC64 LE
│       │   └── abi/                  # ELF32, ELFv1, ELFv2
│       └── arm64/                     # Reference MachineIR backend
│           ├── arm64.c               #   thin backend_ops driver
│           ├── arm64_mir.c           #   source IR -> MachineIR -> asm
│           ├── arm64_helpers.c
│           ├── arm64_internal.h
│           └── opt/                  # ARM64-specific peephole/branch passes
├── tests/                   # Regression tests (see Testing)
├── examples/                # Example programs
├── doc/                     # Documentation
├── Makefile
└── README.md
```

## Code Style

### Naming Conventions

```c
// Public API: anvil_ prefix, snake_case
anvil_ctx_t *anvil_ctx_create(void);
anvil_error_t anvil_module_codegen(...);

// Internal functions: module prefix, snake_case
static void s370_emit_prologue(...);
static anvil_error_t x86_codegen_module(...);

// Types: anvil_ prefix, _t suffix
typedef struct anvil_ctx anvil_ctx_t;
typedef enum anvil_arch anvil_arch_t;

// Constants: ANVIL_ prefix, UPPER_CASE
#define ANVIL_OK 0
#define ANVIL_ERR_NOMEM 1

// Local variables: snake_case
int label_counter;
anvil_func_t *current_func;
```

### Formatting

- 4-space indentation (no tabs)
- Opening brace on same line
- Space after keywords (if, for, while)
- No space after function names
- 80-100 character line limit

```c
// Good
if (condition) {
    do_something();
}

for (int i = 0; i < count; i++) {
    process(items[i]);
}

// Bad
if(condition){
    do_something();
}
```

### Comments

```c
// Single-line comments for brief notes

/* Multi-line comments for
 * longer explanations */

/**
 * Function documentation
 * @param ctx Context
 * @return Error code
 */
anvil_error_t anvil_func(anvil_ctx_t *ctx);
```

## Core Components

### Adding a New IR Operation

1. Add to `anvil_op_t` enum in `anvil.h`:

```c
typedef enum {
    // ...existing ops...
    ANVIL_OP_NEW_OP,
    // ...
} anvil_op_t;
```

2. Add builder function declaration in `anvil.h`:

```c
anvil_value_t *anvil_build_new_op(anvil_ctx_t *ctx,
                                   anvil_value_t *operand,
                                   const char *name);
```

3. Implement in `builder.c`:

```c
anvil_value_t *anvil_build_new_op(anvil_ctx_t *ctx,
                                   anvil_value_t *operand,
                                   const char *name)
{
    if (!ctx || !operand) return NULL;
    
    anvil_value_t *operands[] = { operand };
    return anvil_build_instr(ctx, ANVIL_OP_NEW_OP, operands, 1,
                              operand->type, name);
}
```

4. Add handling in each backend's `emit_instr` function:

```c
case ANVIL_OP_NEW_OP:
    // Emit target-specific code
    break;
```

### Adding a New Type

1. Add to `anvil_type_kind_t` if needed:

```c
typedef enum {
    // ...existing kinds...
    ANVIL_TYPE_NEW_KIND,
} anvil_type_kind_t;
```

2. Add creation function in `types.c`:

```c
anvil_type_t *anvil_type_new_kind(anvil_ctx_t *ctx, /* params */)
{
    anvil_type_t *type = anvil_type_create(ctx, ANVIL_TYPE_NEW_KIND);
    if (!type) return NULL;
    
    // Initialize type-specific fields
    type->size = /* calculate size */;
    type->align = /* calculate alignment */;
    
    return type;
}
```

3. Declare in `anvil.h`:

```c
anvil_type_t *anvil_type_new_kind(anvil_ctx_t *ctx, /* params */);
```

### Adding Error Handling

1. Add error code to `anvil_error_t`:

```c
typedef enum {
    ANVIL_OK = 0,
    // ...existing errors...
    ANVIL_ERR_NEW_ERROR,
} anvil_error_t;
```

2. Use consistently:

```c
if (bad_condition) {
    return ANVIL_ERR_NEW_ERROR;
}
```

## Backend Development

### Adding a New Backend (recommended approach)

New backends should lower through **MachineIR**, following the **ARM64 reference
backend** (`src/backend/arm64/`). The x86-64 and x86 modes
(`src/backend/x86/`) backends are concrete, non-RISC examples of the same pattern.
Do not write a monolithic `emit_instr` switch directly over source IR; instead split
the backend into:

| File | Role |
|------|------|
| `<arch>/<arch>.c` | Thin `anvil_backend_ops_t` driver. Implements `init`/`cleanup`/`reset`/`codegen_*` by calling the MIR pipeline below. |
| `<arch>/<arch>_mir.c` | The real work: `anvil_<arch>_lower_func_to_mir`, `anvil_<arch>_verify_mir_legal`, `anvil_<arch>_regalloc_mir`, and `anvil_<arch>_emit_mir[_abi]`. |
| `<arch>/<arch>_internal.h` | Internal structs/descriptor tables shared across the backend's `.c` files. |
| `<arch>/<arch>_helpers.c` | Small shared helpers (register names, descriptor lookup, etc.). |
| `include/anvil/anvil_<arch>_mir.h` | Public-ish entry points for the four pipeline stages (used by regression tests and the driver). |

**The MachineIR pipeline** (see `anvil_<arch>_regalloc_mir` in any reference
backend) runs, per function:

```
anvil_<arch>_lower_func_to_mir(func)      // source IR -> MachineIR (anvil_machine.h)
  -> anvil_<arch>_verify_mir_legal(...)   // reject illegal/un-lowered MIR
  -> anvil_mir_coalesce_copies(...)       // remove redundant COPY chains
  -> anvil_<arch>_verify_mir_legal(...)
  -> anvil_regalloc_linear_scan_classes(mir, configs, n)   // per-class allocation
  -> anvil_mir_materialize_spills(mir, scratch_configs, n) // insert reload/spill code
  -> anvil_<arch>_verify_mir_legal(...)
// then anvil_<arch>_emit_mir[_abi](mir, ...) emits target assembly
```

All of these are declared in `include/anvil/anvil_machine.h`. The driver in
`<arch>.c` simply calls `lower -> regalloc -> emit` and routes the result through
`codegen_func`/`codegen_module`.

**Register-pool / regalloc considerations** (these are the easiest things to get
wrong):

- The **allocatable pools** passed to `anvil_regalloc_linear_scan_classes` must be
  **callee-saved registers only**. The allocator has *no call-clobber model* — it does
  not know a `CALL` destroys caller-saved registers, so any value it places in a
  caller-saved register that is live across a call would be corrupted. Reference
  backends therefore hand the allocator only callee-saved GPR/FPR pools (see the
  `alloc_gpr_regs`/`alloc_fpr_regs` arrays in the per-ABI descriptor tables).
- The **scratch pools** passed to `anvil_mir_materialize_spills` must be **disjoint
  from any fixed-register assignments** (ABI argument/return registers, etc.) and
  **large enough for the worst-case number of simultaneous spill temporaries in a
  single instruction**; otherwise spill materialization runs out of scratch registers.
- The **ABI is expressed as fixed-register vregs plus a descriptor table**: argument,
  return, and clobbered registers are modeled by creating vregs pinned to physical
  registers (`anvil_mir_set_fixed_reg`), and per-ABI facts (which regs are
  allocatable/scratch/fixed, decoration, stack alignment) live in a descriptor table
  selected by `anvil_abi_t` / `anvil_cc_t`. The x86_64 backend selects SysV/Darwin/
  Win64 descriptors this way, and the x86 backend selects cdecl/stdcall/fastcall
  descriptors. The effective convention is immutable in
  `func->type->data.func.cc`; each CALL copies it into `instr->call_cc`, so an
  indirect VTable call cannot inherit the caller's ABI accidentally.

For the full design rationale and instruction-level details, see
[`BACKENDS.md`](BACKENDS.md) and [`ARM64_REFACTOR.md`](ARM64_REFACTOR.md) rather than
duplicating backend internals here.

### Backend Checklist

When implementing a new backend, ensure you handle:

- [ ] All arithmetic operations (add, sub, mul, div, mod, neg)
- [ ] All bitwise operations (and, or, xor, not, shl, shr, sar)
- [ ] All comparison operations (eq, ne, lt, le, gt, ge, unsigned variants)
- [ ] Memory operations (alloca, load, store, gep)
- [ ] Control flow (br, br_cond, call, ret)
- [ ] Type conversions (trunc, zext, sext, bitcast, ptrtoint, inttoptr)
- [ ] PHI nodes and select
- [ ] Function prologue/epilogue
- [ ] Parameter passing
- [ ] Return values
- [ ] Global variables
- [ ] Constants (integers, floats, null, strings)

### Testing

There are two complementary kinds of tests.

**1. MIR-lowering regression tests** (`tests/*_mir_lowering_regression.c`). These are
the primary way to test a MachineIR backend. Each is a standalone program that builds
source IR, drives the backend's MIR pipeline, and asserts on the generated assembly
text (or on MIR-level properties). Existing examples:

```
tests/arm64_mir_lowering_regression.c
tests/x86_64_mir_lowering_regression.c
tests/x86_mir_lowering_regression.c
tests/ppc_mir_lowering_regression.c
tests/mainframe_mir_lowering_regression.c
```

The full test list (also including `core_arm64_regression`, `ir_verifier_regression`,
`optimizer_regression`, and `machine_regalloc_regression`) is wired into the `TESTS`
variable in the `Makefile`. To add a new backend's regression test:

1. Create `tests/<arch>_mir_lowering_regression.c` following the existing files.
2. Add `$(BUILD_DIR)/tests/<arch>_mir_lowering_regression` to the `TESTS` list in the
   `Makefile`. The generic rule `$(BUILD_DIR)/tests/%: tests/%.c $(LIB_PATH)` builds
   and links each test against `lib/libanvil.a`.
3. Run the suite:

```bash
make tests          # builds and runs every test in TESTS, stops on first failure
```

A minimal test skeleton:

```c
#include <anvil/anvil.h>
#include <assert.h>
#include <string.h>

int main(void) {
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(ANVIL_ARCH_YOUR_ARCH);
    anvil_module_t *mod = anvil_module_create(ctx, "test");

    /* Build: int add(int a, int b) { return a + b; } */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *fty = anvil_type_func(ctx, i32, params, 2, false);
    anvil_func_t *fn = anvil_func_create(mod, "add", fty, ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *r = anvil_build_add(ctx,
        anvil_func_get_param(fn, 0), anvil_func_get_param(fn, 1), "r");
    anvil_build_ret(ctx, r);

    char *asm_out = NULL; size_t len = 0;
    assert(anvil_module_codegen(mod, &asm_out, &len) == ANVIL_OK);
    assert(asm_out && strstr(asm_out, "add"));  /* assert on expected instructions */

    free(asm_out);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    return 0;
}
```

**2. Execution tests** (`examples/basic_runtime`, via `test-examples`). These generate
assembly, assemble and link it with a C driver, run the result, and check its exit
status/output — i.e. they confirm the emitted code actually *runs* correctly on the
host. Build/run them with:

```bash
make test-examples            # examples/basic_runtime
make test-examples-advanced   # fp_math_lib, dynamic_array, base64_lib
```

### Debugging Backend Issues

1. **Print IR before codegen:**

```c
void debug_print_func(anvil_func_t *func)
{
    printf("Function: %s\n", func->name);
    for (anvil_block_t *bb = func->blocks; bb; bb = bb->next) {
        printf("  Block: %s\n", bb->name);
        for (anvil_instr_t *i = bb->first; i; i = i->next) {
            printf("    Op: %d, Operands: %zu\n", i->op, i->num_operands);
        }
    }
}
```

2. **Add verbose output to backend:**

```c
static void emit_instr(backend_t *be, anvil_instr_t *instr)
{
    #ifdef DEBUG
    anvil_strbuf_appendf(&be->code, "; DEBUG: op=%d operands=%zu\n",
                          instr->op, instr->num_operands);
    #endif
    
    switch (instr->op) {
        // ...
    }
}
```

3. **Verify generated assembly:**

```bash
# For x86
as -o test.o test.s && objdump -d test.o

# For mainframe (if you have HLASM)
hlasm test.asm
```

## Common Pitfalls

### Memory Management

```c
// WRONG: Forgetting to free
char *output = NULL;
anvil_module_codegen(mod, &output, NULL);
// output is leaked!

// CORRECT: Always free output
char *output = NULL;
anvil_module_codegen(mod, &output, NULL);
// ... use output ...
free(output);
```

### Backend Cleanup and Reset

Backends must implement proper cleanup to avoid memory leaks and dangling pointers:

```c
// cleanup: Free all backend resources
static void myarch_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    myarch_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    free(priv->stack_slots);  // Free arrays
    free(priv->strings);
    free(priv);
    be->priv = NULL;
}

// reset: Clear cached IR pointers (called before module destruction)
static void myarch_reset(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    myarch_backend_t *priv = be->priv;
    
    // Clear stack_slots (contain pointers to anvil_value_t)
    priv->num_stack_slots = 0;
    
    // Clear string table (contain pointers to string data)
    priv->num_strings = 0;
    
    // Reset counters
    priv->label_counter = 0;
}
```

**Why `reset` is important:**
- Backends cache pointers to `anvil_value_t` in stack slots and string tables
- When `anvil_ctx_destroy()` is called, modules are destroyed (freeing IR values)
- If backend still holds pointers to freed values → **dangling pointers**
- `reset` is called BEFORE module destruction to clear these cached pointers

### Null Checks

```c
// WRONG: No null check
void process(anvil_value_t *val)
{
    printf("Type: %d\n", val->type->kind);  // Crash if val is NULL
}

// CORRECT: Check for null
void process(anvil_value_t *val)
{
    if (!val) return;
    printf("Type: %d\n", val->type->kind);
}
```

### Type Mismatches

```c
// WRONG: Assuming types match
anvil_value_t *result = anvil_build_add(ctx, i32_val, i64_val, "result");
// Undefined behavior!

// CORRECT: Ensure types match
anvil_value_t *extended = anvil_build_zext(ctx, i32_val,
                                            anvil_type_i64(ctx), "ext");
anvil_value_t *result = anvil_build_add(ctx, extended, i64_val, "result");
```

### Backend Register Clobbering

```c
// WRONG: Using same register for both operands
emit_load_value(be, instr->operands[0], R0);
emit_load_value(be, instr->operands[1], R0);  // Clobbers first operand!
emit("ADD R0, R0");  // Wrong result

// CORRECT: Use different registers
emit_load_value(be, instr->operands[0], R0);
emit_load_value(be, instr->operands[1], R1);
emit("ADD R0, R1");
```

### Mainframe-Specific Issues

```c
// WRONG: Using LM to restore all registers including R15
emit("LM R14,R12,12(R13)");  // Overwrites return value in R15!

// CORRECT: Restore R14 and R0-R12 separately
emit("L R14,12(,R13)");      // Restore return address only
emit("LM R0,R12,20(,R13)");  // Restore R0-R12, skip R14 and R15

// WRONG: z/Architecture STMG at offset 8
emit("STMG R14,R12,8(R13)");  // Corrupts save area chain!

// CORRECT: z/Architecture STMG at offset 24
emit("STMG R14,R12,24(R13)");  // F4SA format
```

## Performance Optimization

### String Buffer Usage

```c
// WRONG: Many small appends
for (int i = 0; i < 1000; i++) {
    anvil_strbuf_appendf(&sb, "%d", i);
}

// BETTER: Batch when possible
char buf[8192];
int pos = 0;
for (int i = 0; i < 1000; i++) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%d", i);
}
anvil_strbuf_append(&sb, buf);
```

### Avoid Redundant Loads

```c
// WRONG: Loading same value multiple times
emit_load_value(be, val, R0);
emit("STORE [addr1], R0");
emit_load_value(be, val, R0);  // Redundant!
emit("STORE [addr2], R0");

// CORRECT: Reuse loaded value
emit_load_value(be, val, R0);
emit("STORE [addr1], R0");
emit("STORE [addr2], R0");  // R0 still has the value
```

## Contributing

### Submitting Changes

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Add tests for new functionality
5. Ensure all tests pass
6. Submit a pull request

### Code Review Checklist

- [ ] Code follows style guidelines
- [ ] All functions have proper null checks
- [ ] Memory is properly managed (no leaks)
- [ ] New features have tests
- [ ] Documentation is updated
- [ ] Commit messages are clear

### Reporting Bugs

Include:
1. ANVIL version
2. Target architecture
3. Minimal reproducing code
4. Expected vs actual output
5. Any error messages

## Future Work

Areas that need development:

1. **Register Allocation**: A target-independent linear-scan allocator now exists
   (`src/machine/regalloc.c`, used by the MachineIR backends). It has no call-clobber
   model yet, so allocatable pools must be callee-saved (see Backend Development).
2. **Binary Output**: The current public output API supports text assembly only.
3. **Debug Info**: No DWARF support.
4. **More Architectures**: RISC-V, etc. (x86, x86-64, ARM64, PowerPC, and the
   mainframe family are implemented).
5. **Aggregate Support**: Struct/array handling is still limited on some backends.

## Resources

### Compiler Design

- "Engineering a Compiler" by Cooper & Torczon
- "Modern Compiler Implementation" by Appel
- LLVM documentation (llvm.org)

### x86/x86-64

- Intel Software Developer Manuals
- AMD64 Architecture Programmer's Manual
- System V ABI specification

### IBM Mainframe

- z/Architecture Principles of Operation (SA22-7832)
- z/OS MVS Programming: Assembler Services Guide
- HLASM Language Reference

### Online Resources

- Godbolt Compiler Explorer (godbolt.org)
- OSDev Wiki (wiki.osdev.org)
- IBM Documentation (ibm.com/docs)
