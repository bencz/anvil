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
# Clone and build
git clone <repository>
cd anvil
make

# Build with debug symbols
make DEBUG=1

# Run tests
make test

# Clean build
make clean && make
```

### Project Structure

```
anvil/
├── include/anvil/
│   ├── anvil.h              # Public API
│   └── anvil_internal.h     # Internal structures
├── src/
│   ├── core/                # Core library
│   │   ├── context.c        # Context management
│   │   ├── types.c          # Type system
│   │   ├── module.c         # Module management
│   │   ├── function.c       # Function management
│   │   ├── value.c          # Values and instructions
│   │   ├── builder.c        # IR builder
│   │   ├── strbuf.c         # String buffer
│   │   ├── backend.c        # Backend registry
│   │   └── memory.c         # Memory utilities
│   └── backend/             # Target backends
│       ├── x86/x86.c
│       ├── x86_64/x86_64.c
│       ├── s370/s370.c
│       ├── s370_xa/s370_xa.c
│       ├── s390/s390.c
│       └── zarch/zarch.c
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

### Testing a Backend

```c
// Create a test function
void test_backend_add(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_YOUR_ARCH);
    
    anvil_module_t *mod = anvil_module_create(ctx, "test");
    
    // Create: int add(int a, int b) { return a + b; }
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
    
    anvil_func_t *func = anvil_func_create(mod, "add", func_type,
                                            ANVIL_LINK_EXTERNAL);
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *a = anvil_func_get_param(func, 0);
    anvil_value_t *b = anvil_func_get_param(func, 1);
    anvil_value_t *result = anvil_build_add(ctx, a, b, "result");
    anvil_build_ret(ctx, result);
    
    // Generate and verify
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    assert(err == ANVIL_OK);
    assert(output != NULL);
    
    // Check for expected instructions
    printf("Generated:\n%s\n", output);
    
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}
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

1. **Register Allocation**: Currently uses fixed registers
2. **Optimization Passes**: No IR optimization yet
3. **Binary Output**: Currently text assembly only
4. **Debug Info**: No DWARF support
5. **More Architectures**: ARM, RISC-V, etc.
6. **String/Array Support**: Limited aggregate type support

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
