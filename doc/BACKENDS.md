# ANVIL Backend Development Guide

This document explains how backends work and how to implement new ones.

## Backend Architecture

### Overview

A backend translates ANVIL IR to target-specific assembly code. Each backend is a self-contained module that implements the `anvil_backend_ops_t` interface.

```
┌─────────────────────────────────────────────────────────────────┐
│                          ANVIL IR                                │
│  (Modules, Functions, Blocks, Instructions, Values, Types)      │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Backend Interface                           │
│                   (anvil_backend_ops_t)                         │
└─────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│  x86 Backend  │    │ S/370 Backend │    │ Your Backend  │
└───────────────┘    └───────────────┘    └───────────────┘
        │                     │                     │
        ▼                     ▼                     ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│  GAS/NASM     │    │    HLASM      │    │  Your ASM     │
│  Assembly     │    │   Assembly    │    │   Format      │
└───────────────┘    └───────────────┘    └───────────────┘
```

### Backend Interface

```c
typedef struct {
    const char *name;              // Human-readable name
    anvil_arch_t arch;             // Architecture enum value
    
    // Initialize backend state
    anvil_error_t (*init)(anvil_backend_t *be, anvil_ctx_t *ctx);
    
    // Free backend resources
    void (*cleanup)(anvil_backend_t *be);
    
    // Generate code for entire module
    anvil_error_t (*codegen_module)(anvil_backend_t *be, anvil_module_t *mod,
                                     char **output, size_t *len);
    
    // Generate code for single function (optional)
    anvil_error_t (*codegen_func)(anvil_backend_t *be, anvil_func_t *func,
                                   char **output, size_t *len);
    
    // Return architecture information
    const anvil_arch_info_t *(*get_arch_info)(anvil_backend_t *be);
} anvil_backend_ops_t;
```

### Architecture Information

```c
typedef struct {
    anvil_arch_t arch;             // Architecture enum
    const char *name;              // Architecture name
    int ptr_size;                  // Pointer size in bytes
    int addr_bits;                 // Address bits (24, 31, 32, 64)
    int word_size;                 // Native word size
    int num_gpr;                   // General purpose registers
    int num_fpr;                   // Floating point registers
    anvil_endian_t endian;         // ANVIL_ENDIAN_LITTLE or ANVIL_ENDIAN_BIG
    anvil_stack_dir_t stack_dir;   // ANVIL_STACK_DOWN or ANVIL_STACK_UP
    bool has_condition_codes;      // Has condition code register
    bool has_delay_slots;          // Has branch delay slots
} anvil_arch_info_t;
```

## Implementing a New Backend

### Step 1: Create Backend File

Create `src/backend/<arch>/<arch>.c`:

```c
#include "anvil/anvil_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Register names for your architecture
static const char *myarch_reg_names[] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    // ...
};

// Register indices
#define MYARCH_R0   0
#define MYARCH_R1   1
// ...

// Backend private data
typedef struct {
    anvil_strbuf_t code;           // Output buffer
    anvil_strbuf_t data;           // Data section buffer
    int label_counter;             // For generating unique labels
    // Add your backend-specific state here
} myarch_backend_t;
```

### Step 2: Define Architecture Info

```c
static const anvil_arch_info_t myarch_arch_info = {
    .arch = ANVIL_ARCH_MYARCH,
    .name = "MyArch",
    .ptr_size = 4,                 // 4 bytes for 32-bit
    .addr_bits = 32,
    .word_size = 4,
    .num_gpr = 16,
    .num_fpr = 8,
    .endian = ANVIL_ENDIAN_LITTLE,
    .stack_dir = ANVIL_STACK_DOWN,
    .has_condition_codes = true,
    .has_delay_slots = false
};
```

### Step 3: Implement Init and Cleanup

```c
static anvil_error_t myarch_init(anvil_backend_t *be, anvil_ctx_t *ctx)
{
    (void)ctx;  // May be unused
    
    myarch_backend_t *priv = calloc(1, sizeof(myarch_backend_t));
    if (!priv) return ANVIL_ERR_NOMEM;
    
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    priv->label_counter = 0;
    
    be->priv = priv;
    return ANVIL_OK;
}

static void myarch_cleanup(anvil_backend_t *be)
{
    if (!be || !be->priv) return;
    
    myarch_backend_t *priv = be->priv;
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    free(priv);
    be->priv = NULL;
}

static const anvil_arch_info_t *myarch_get_arch_info(anvil_backend_t *be)
{
    (void)be;
    return &myarch_arch_info;
}
```

### Step 4: Implement Code Emission Helpers

```c
// Emit file header
static void myarch_emit_header(myarch_backend_t *be, const char *module_name)
{
    anvil_strbuf_appendf(&be->code, "; Module: %s\n", module_name);
    anvil_strbuf_append(&be->code, "; Generated by ANVIL\n\n");
    anvil_strbuf_append(&be->code, ".text\n");
}

// Emit function prologue
static void myarch_emit_prologue(myarch_backend_t *be, anvil_func_t *func)
{
    anvil_strbuf_appendf(&be->code, ".global %s\n", func->name);
    anvil_strbuf_appendf(&be->code, "%s:\n", func->name);
    
    // Save frame pointer, allocate stack, etc.
    anvil_strbuf_append(&be->code, "    push rbp\n");
    anvil_strbuf_append(&be->code, "    mov rbp, rsp\n");
    // ...
}

// Emit function epilogue
static void myarch_emit_epilogue(myarch_backend_t *be)
{
    anvil_strbuf_append(&be->code, "    mov rsp, rbp\n");
    anvil_strbuf_append(&be->code, "    pop rbp\n");
    anvil_strbuf_append(&be->code, "    ret\n");
}

// Load a value into a register
static void myarch_emit_load_value(myarch_backend_t *be, anvil_value_t *val, int reg)
{
    if (!val) return;
    
    switch (val->kind) {
        case ANVIL_VAL_CONST_INT:
            anvil_strbuf_appendf(&be->code, "    mov %s, %lld\n",
                myarch_reg_names[reg], (long long)val->data.i);
            break;
            
        case ANVIL_VAL_PARAM:
            // Load parameter based on calling convention
            // ...
            break;
            
        case ANVIL_VAL_INSTR:
            // Result of previous instruction (may already be in a register)
            // ...
            break;
            
        // Handle other value kinds...
    }
}
```

### Step 5: Implement Instruction Emission

```c
static void myarch_emit_instr(myarch_backend_t *be, anvil_instr_t *instr)
{
    if (!instr) return;
    
    switch (instr->op) {
        case ANVIL_OP_ADD:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            myarch_emit_load_value(be, instr->operands[1], MYARCH_R1);
            anvil_strbuf_append(&be->code, "    add R0, R1\n");
            // Result is in R0
            break;
            
        case ANVIL_OP_SUB:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            myarch_emit_load_value(be, instr->operands[1], MYARCH_R1);
            anvil_strbuf_append(&be->code, "    sub R0, R1\n");
            break;
            
        case ANVIL_OP_MUL:
            // Handle multiplication
            break;
            
        case ANVIL_OP_SDIV:
        case ANVIL_OP_UDIV:
            // Handle division
            break;
            
        case ANVIL_OP_AND:
        case ANVIL_OP_OR:
        case ANVIL_OP_XOR:
        case ANVIL_OP_NOT:
            // Handle bitwise operations
            break;
            
        case ANVIL_OP_SHL:
        case ANVIL_OP_SHR:
        case ANVIL_OP_SAR:
            // Handle shifts
            break;
            
        case ANVIL_OP_CMP_EQ:
        case ANVIL_OP_CMP_NE:
        case ANVIL_OP_CMP_LT:
        case ANVIL_OP_CMP_LE:
        case ANVIL_OP_CMP_GT:
        case ANVIL_OP_CMP_GE:
            // Handle comparisons
            break;
            
        case ANVIL_OP_LOAD:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            anvil_strbuf_append(&be->code, "    load R0, [R0]\n");
            break;
            
        case ANVIL_OP_STORE:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);  // value
            myarch_emit_load_value(be, instr->operands[1], MYARCH_R1);  // address
            anvil_strbuf_append(&be->code, "    store [R1], R0\n");
            break;
            
        case ANVIL_OP_BR:
            anvil_strbuf_appendf(&be->code, "    jmp %s\n",
                instr->true_block->name);
            break;
            
        case ANVIL_OP_BR_COND:
            myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
            anvil_strbuf_append(&be->code, "    test R0, R0\n");
            anvil_strbuf_appendf(&be->code, "    jnz %s\n",
                instr->true_block->name);
            anvil_strbuf_appendf(&be->code, "    jmp %s\n",
                instr->false_block->name);
            break;
            
        case ANVIL_OP_RET:
            if (instr->num_operands > 0) {
                myarch_emit_load_value(be, instr->operands[0], MYARCH_R0);
                // Move to return register if needed
            }
            myarch_emit_epilogue(be);
            break;
            
        case ANVIL_OP_CALL:
            // Handle function calls
            break;
            
        default:
            anvil_strbuf_appendf(&be->code, "    ; Unimplemented: op %d\n",
                instr->op);
            break;
    }
}
```

### Step 6: Implement Block and Function Emission

```c
static void myarch_emit_block(myarch_backend_t *be, anvil_block_t *block)
{
    if (!block) return;
    
    // Emit block label
    anvil_strbuf_appendf(&be->code, "%s:\n", block->name);
    
    // Emit all instructions
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        myarch_emit_instr(be, instr);
    }
}

static void myarch_emit_func(myarch_backend_t *be, anvil_func_t *func)
{
    if (!func) return;
    
    myarch_emit_prologue(be, func);
    
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        myarch_emit_block(be, block);
    }
    
    anvil_strbuf_append(&be->code, "\n");
}
```

### Step 7: Implement Module Code Generation

```c
static anvil_error_t myarch_codegen_module(anvil_backend_t *be,
                                            anvil_module_t *mod,
                                            char **output, size_t *len)
{
    if (!be || !mod || !output) return ANVIL_ERR_INVALID_ARG;
    
    myarch_backend_t *priv = be->priv;
    
    // Reset buffers
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_destroy(&priv->data);
    anvil_strbuf_init(&priv->code);
    anvil_strbuf_init(&priv->data);
    
    // Emit header
    myarch_emit_header(priv, mod->name);
    
    // Emit all functions
    for (anvil_func_t *func = mod->funcs; func; func = func->next) {
        myarch_emit_func(priv, func);
    }
    
    // Emit data section
    if (mod->num_globals > 0) {
        anvil_strbuf_append(&priv->code, "\n.data\n");
        for (anvil_global_t *g = mod->globals; g; g = g->next) {
            anvil_strbuf_appendf(&priv->code, "%s: .long 0\n",
                g->value->name);
        }
    }
    
    // Return output
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}

static anvil_error_t myarch_codegen_func(anvil_backend_t *be,
                                          anvil_func_t *func,
                                          char **output, size_t *len)
{
    if (!be || !func || !output) return ANVIL_ERR_INVALID_ARG;
    
    myarch_backend_t *priv = be->priv;
    
    anvil_strbuf_destroy(&priv->code);
    anvil_strbuf_init(&priv->code);
    
    myarch_emit_func(priv, func);
    
    *output = anvil_strbuf_detach(&priv->code, len);
    return ANVIL_OK;
}
```

### Step 8: Export Backend Operations

```c
const anvil_backend_ops_t anvil_backend_myarch = {
    .name = "MyArch",
    .arch = ANVIL_ARCH_MYARCH,
    .init = myarch_init,
    .cleanup = myarch_cleanup,
    .codegen_module = myarch_codegen_module,
    .codegen_func = myarch_codegen_func,
    .get_arch_info = myarch_get_arch_info
};
```

### Step 9: Register Backend

Add to `include/anvil/anvil.h`:

```c
typedef enum {
    ANVIL_ARCH_UNKNOWN = 0,
    ANVIL_ARCH_X86,
    ANVIL_ARCH_X86_64,
    ANVIL_ARCH_S370,
    ANVIL_ARCH_S370_XA,
    ANVIL_ARCH_S390,
    ANVIL_ARCH_ZARCH,
    ANVIL_ARCH_MYARCH,      // Add your architecture
    ANVIL_ARCH_COUNT
} anvil_arch_t;
```

Add to `src/core/backend.c`:

```c
extern const anvil_backend_ops_t anvil_backend_myarch;

static const anvil_backend_ops_t *backends[] = {
    &anvil_backend_x86,
    &anvil_backend_x86_64,
    &anvil_backend_s370,
    &anvil_backend_s370_xa,
    &anvil_backend_s390,
    &anvil_backend_zarch,
    &anvil_backend_myarch,  // Add your backend
};
```

Add to `Makefile`:

```makefile
BACKEND_SRCS = \
    src/backend/x86/x86.c \
    src/backend/x86_64/x86_64.c \
    src/backend/s370/s370.c \
    src/backend/s390/s390.c \
    src/backend/zarch/zarch.c \
    src/backend/myarch/myarch.c  # Add your backend
```

## Calling Conventions

### Handling Parameters

Different architectures pass parameters differently:

**x86 (CDECL):**
- All parameters on stack
- Caller cleans up stack

**x86-64 (System V):**
- First 6 integer args: RDI, RSI, RDX, RCX, R8, R9
- First 8 float args: XMM0-XMM7
- Rest on stack

**ARM64 (AAPCS64):**
- First 8 integer args: X0-X7
- First 8 float args: D0-D7
- Return value: X0 (integer), D0 (float)
- Frame pointer: X29
- Link register: X30
- Stack pointer: SP (16-byte aligned)

**IBM Mainframe (MVS/GCCMVS):**
- R1 points to parameter list
- Each entry is address of parameter
- High bit of last entry is set (VL bit)
- VL bit is NOT cleared when loading parameters (allows full 31/64-bit addressing)

### GCCMVS Conventions for Mainframes

ANVIL generates code compatible with GCCMVS conventions:

| Convention | Implementation |
|------------|----------------|
| **CSECT** | Blank (no module name prefix) |
| **AMODE/RMODE** | `AMODE ANY`, `RMODE ANY` |
| **Function Names** | UPPERCASE (e.g., `FACTORIAL`, `SUM_ARRAY`) |
| **Block Labels** | `FUNCNAME$BLOCKNAME` format |
| **Stack Allocation** | Direct stack offset from R13 (no GETMAIN/FREEMAIN) |
| **VL Bit** | NOT cleared (allows full addressing) |

**Why Stack Allocation Instead of GETMAIN:**
- **Performance**: `LA R2,72(,R13)` is faster than `GETMAIN R,LV=size`
- **Simplicity**: No need to save/restore R15 around FREEMAIN in epilogue
- **Compatibility**: Matches GCCMVS output for easier interoperability

**Prologue (Stack-Based):**
```hlasm
FACTORIAL DS    0H
         STM   R14,R12,12(R13)    Save caller's registers
         LR    R12,R15            Copy entry point to base reg
         USING FACTORIAL,R12      Establish addressability
         LR    R11,R1             Save parameter list pointer
*        Set up save area chain (stack allocation)
         LA    R2,72(,R13)        R2 -> our save area
         ST    R13,4(,R2)         Chain: new->prev = caller's
         ST    R2,8(,R13)         Chain: caller->next = new
         LR    R13,R2             R13 -> our save area
```

**Epilogue (Stack-Based):**
```hlasm
*        Function epilogue
         L     R13,4(,R13)        Restore caller's SA pointer
         L     R14,12(,R13)       Restore return address
         LM    R0,R12,20(,R13)    Restore R0-R12
         BR    R14                Return to caller
```

### Handling Return Values

**x86/x86-64:**
- Integer: EAX/RAX
- Float: XMM0 or ST(0)

**ARM64:**
- Integer: X0
- Float: D0 (double) or S0 (float)

**IBM Mainframe:**
- R15 contains return value

## Register Allocation

ANVIL currently uses a simple register allocation strategy:

1. Load operands into fixed registers
2. Perform operation
3. Result is in a fixed register

For more sophisticated backends, consider:

1. **Linear Scan**: Fast, good for JIT
2. **Graph Coloring**: Better code quality
3. **PBQP**: Optimal for small functions

## Handling Special Cases

### Division

Many architectures have special requirements for division:

**x86:**
```asm
; Signed division: EDX:EAX / operand
mov eax, dividend
cdq                 ; Sign-extend EAX into EDX
idiv divisor        ; Result in EAX, remainder in EDX
```

**S/370:**
```asm
; Division uses even-odd register pair
L     R3,dividend
SRDA  R2,32         ; Sign-extend R3 into R2:R3
D     R2,divisor    ; Quotient in R3, remainder in R2
```

### Comparisons

**x86:**
```asm
cmp eax, ebx
sete al             ; Set AL to 1 if equal
movzx eax, al       ; Zero-extend to full register
```

**ARM64:**
```asm
cmp x9, x10         ; Compare registers
cset x0, eq         ; Set X0 to 1 if equal, 0 otherwise
```

**S/370:**
```asm
CR    R2,R3         ; Compare registers
LA    R15,1         ; Assume true
BE    *+6           ; Skip if equal
SR    R15,R15       ; Set false
```

### Function Calls

**x86-64:**
```asm
; Call printf("Hello %d", 42)
lea rdi, [rel fmt]  ; First arg: format string
mov esi, 42         ; Second arg: integer
xor eax, eax        ; No vector args
call printf
```

**ARM64:**
```asm
// Call printf("Hello %d", 42)
adrp x0, fmt            // Load page address of fmt
add x0, x0, :lo12:fmt   // Add page offset
mov w1, #42             // Second arg: integer
bl printf               // Branch with link
```

**S/370:**
```asm
; Build parameter list in dynamic area
ST    R0,72(,R13)   ; Store first param address
ST    R1,76(,R13)   ; Store second param address
OI    76(R13),X'80' ; Mark last param
LA    R1,72(,R13)   ; R1 -> param list
L     R15,=V(FUNC)  ; Load function address
BALR  R14,R15       ; Call
```

## Testing Your Backend

### Unit Tests

Create test cases for each instruction type:

```c
void test_add(void) {
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_MYARCH);
    
    anvil_module_t *mod = anvil_module_create(ctx, "test");
    // Create add function...
    
    char *output = NULL;
    anvil_module_codegen(mod, &output, NULL);
    
    // Verify output contains expected instructions
    assert(strstr(output, "add") != NULL);
    
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}
```

### Integration Tests

Test complete programs:

```c
void test_factorial(void) {
    // Generate factorial function
    // Assemble output
    // Link and run
    // Verify result
}
```

## Debugging Tips

1. **Print IR**: Add debug output to see what IR you're processing
2. **Check Operand Types**: Many bugs come from type mismatches
3. **Verify Offsets**: Stack and memory offsets are common error sources
4. **Test Incrementally**: Start with simple functions, add complexity gradually
5. **Compare with Reference**: Generate code with a known compiler and compare

## Performance Considerations

1. **Minimize Memory Operations**: Keep values in registers when possible
2. **Use Efficient Instructions**: Some operations have faster alternatives
3. **Align Data**: Misaligned access is slow on most architectures
4. **Branch Prediction**: Arrange code to help branch predictors
5. **Instruction Scheduling**: Order instructions to avoid pipeline stalls

## Platform-Specific Code Generation

### OS ABI Support

ANVIL supports multiple OS ABIs through the `anvil_ctx_set_abi()` function. This affects:

- **Symbol naming**: Darwin uses underscore prefix (`_main` vs `main`)
- **Assembly directives**: `.type` (ELF) vs `.p2align` (Mach-O)
- **Section names**: `.text` (ELF) vs `__TEXT,__text` (Mach-O)
- **Relocation syntax**: `:lo12:` (ELF) vs `@PAGE/@PAGEOFF` (Mach-O)

### ARM64 macOS (Apple Silicon) Example

```c
anvil_ctx_t *ctx = anvil_ctx_create();
anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64);
anvil_ctx_set_abi(ctx, ANVIL_ABI_DARWIN);  // Enable macOS mode

// ... build IR ...

// Generated code will have:
// - Underscore prefix on symbols (_main, _printf)
// - Mach-O section directives
// - @PAGE/@PAGEOFF relocations
```

**Linux ARM64:**
```asm
        .globl main
        .type main, %function
main:
        adrp x0, .LC0
        add x0, x0, :lo12:.LC0
        bl printf
        .size main, .-main
```

**macOS ARM64:**
```asm
        .globl _main
        .p2align 2
_main:
        adrp x0, L_.str@PAGE
        add x0, x0, L_.str@PAGEOFF
        bl _printf
```

### Implementing ABI Support in Backends

When implementing a backend that needs ABI-specific code:

```c
static void emit_function(my_backend_t *be, anvil_func_t *func)
{
    bool is_darwin = be->ctx && be->ctx->abi == ANVIL_ABI_DARWIN;
    const char *prefix = is_darwin ? "_" : "";
    
    if (is_darwin) {
        // Mach-O directives
        emit(".globl %s%s\n", prefix, func->name);
        emit(".p2align 2\n");
    } else {
        // ELF directives
        emit(".globl %s\n", func->name);
        emit(".type %s, %%function\n", func->name);
    }
    
    emit("%s%s:\n", prefix, func->name);
    // ... emit function body ...
}
```
