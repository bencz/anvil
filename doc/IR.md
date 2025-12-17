# ANVIL Intermediate Representation Specification

This document specifies the ANVIL IR (Intermediate Representation).

## Table of Contents

1. [Overview](#overview)
2. [SSA Form](#ssa-form)
3. [Module Structure](#module-structure)
4. [Types](#types)
5. [Values](#values)
6. [Instructions](#instructions)
7. [Floating-Point Instructions](#floating-point-instructions)
8. [Well-Formedness Rules](#well-formedness-rules)
9. [Example IR](#example-ir)
10. [IR Debug/Dump API](#ir-debugdump-api)
11. [Lowering to Assembly](#lowering-to-assembly)

## Overview

ANVIL IR is a low-level, typed, SSA-based intermediate representation designed for code generation. It is similar in spirit to LLVM IR but simpler and more focused on practical code generation for multiple architectures.

**Key Characteristics:**
- **SSA Form**: Static Single Assignment for clean dataflow
- **Typed**: All values have explicit types
- **Low-level**: Close to machine operations
- **Portable**: Architecture-independent representation
- **Extensible**: Easy to add new operations and types

## SSA Form

ANVIL IR uses Static Single Assignment (SSA) form:

1. Every value is assigned exactly once
2. Every use of a value refers to exactly one definition
3. PHI nodes are used to merge values from different control flow paths

**Benefits:**
- Simplifies dataflow analysis
- Enables many optimizations
- Makes register allocation easier

## Module Structure

```
Module
├── Name
├── Functions[]
│   ├── Name
│   ├── Type (return type, parameter types)
│   ├── Linkage
│   └── Blocks[]
│       ├── Name (label)
│       └── Instructions[]
│           ├── Operation
│           ├── Operands[]
│           └── Result (optional)
└── Globals[]
    ├── Name
    ├── Type
    └── Initializer (optional)
```

## Types

### Primitive Types

| Type | Description | Size (bytes) |
|------|-------------|--------------|
| void | No value | 0 |
| i1 | Boolean | 1 |
| i8 | 8-bit signed integer | 1 |
| i16 | 16-bit signed integer | 2 |
| i32 | 32-bit signed integer | 4 |
| i64 | 64-bit signed integer | 8 |
| u8 | 8-bit unsigned integer | 1 |
| u16 | 16-bit unsigned integer | 2 |
| u32 | 32-bit unsigned integer | 4 |
| u64 | 64-bit unsigned integer | 8 |
| f32 | 32-bit IEEE float | 4 |
| f64 | 64-bit IEEE double | 8 |

### Derived Types

#### Pointer Type

```
ptr<T>
```

A pointer to type T. Size depends on target architecture.

#### Array Type

```
[N x T]
```

An array of N elements of type T.

#### Struct Type

```
{ T1, T2, ..., Tn }
```

A structure with fields of types T1 through Tn.

#### Function Type

```
T(T1, T2, ..., Tn)
T(T1, T2, ..., Tn, ...)  // variadic
```

A function returning T with parameters T1 through Tn.

## Values

### Constants

```
const_i32 42           // Integer constant
const_f64 3.14159      // Float constant
const_null ptr<i32>    // Null pointer
const_string "hello"   // String constant
const_array [i16 x 3] { 1, 2, 3 }  // Array constant
```

### Parameters

Function parameters are referenced by index:

```
param 0    // First parameter
param 1    // Second parameter
```

### Instruction Results

Most instructions produce a result value that can be used by other instructions:

```
%result = add %a, %b
%x = load %ptr
```

### Global Variables

Global variables are referenced by name:

```
@global_var
```

## Instructions

### Arithmetic Instructions

#### add

```
%result = add %lhs, %rhs
```

Integer or floating-point addition.

**Operands:**
- `%lhs`: Left operand (integer or float)
- `%rhs`: Right operand (same type as lhs)

**Result:** Same type as operands

#### sub

```
%result = sub %lhs, %rhs
```

Integer or floating-point subtraction.

#### mul

```
%result = mul %lhs, %rhs
```

Integer or floating-point multiplication.

#### sdiv

```
%result = sdiv %lhs, %rhs
```

Signed integer division.

#### udiv

```
%result = udiv %lhs, %rhs
```

Unsigned integer division.

#### smod

```
%result = smod %lhs, %rhs
```

Signed integer modulo (remainder).

#### umod

```
%result = umod %lhs, %rhs
```

Unsigned integer modulo.

#### neg

```
%result = neg %val
```

Integer or floating-point negation.

### Bitwise Instructions

#### and

```
%result = and %lhs, %rhs
```

Bitwise AND.

#### or

```
%result = or %lhs, %rhs
```

Bitwise OR.

#### xor

```
%result = xor %lhs, %rhs
```

Bitwise XOR.

#### not

```
%result = not %val
```

Bitwise NOT (one's complement).

#### shl

```
%result = shl %val, %amount
```

Shift left. Shifts `%val` left by `%amount` bits.

#### shr

```
%result = shr %val, %amount
```

Logical shift right. Shifts `%val` right by `%amount` bits, filling with zeros.

#### sar

```
%result = sar %val, %amount
```

Arithmetic shift right. Shifts `%val` right by `%amount` bits, preserving sign.

### Comparison Instructions

All comparison instructions return `i1` (boolean).

#### cmp_eq

```
%result = cmp_eq %lhs, %rhs
```

Equal comparison.

#### cmp_ne

```
%result = cmp_ne %lhs, %rhs
```

Not equal comparison.

#### cmp_lt

```
%result = cmp_lt %lhs, %rhs
```

Signed less than.

#### cmp_le

```
%result = cmp_le %lhs, %rhs
```

Signed less than or equal.

#### cmp_gt

```
%result = cmp_gt %lhs, %rhs
```

Signed greater than.

#### cmp_ge

```
%result = cmp_ge %lhs, %rhs
```

Signed greater than or equal.

#### cmp_ult

```
%result = cmp_ult %lhs, %rhs
```

Unsigned less than.

#### cmp_ule

```
%result = cmp_ule %lhs, %rhs
```

Unsigned less than or equal.

#### cmp_ugt

```
%result = cmp_ugt %lhs, %rhs
```

Unsigned greater than.

#### cmp_uge

```
%result = cmp_uge %lhs, %rhs
```

Unsigned greater than or equal.

### Memory Instructions

#### alloca

```
%ptr = alloca T
```

Allocates space for one value of type T on the stack. Returns a pointer to the allocated space.

**Result:** `ptr<T>`

#### load

```
%val = load %ptr
```

Loads a value from memory.

**Operands:**
- `%ptr`: Pointer to load from

**Result:** Type pointed to by `%ptr`

#### store

```
store %val, %ptr
```

Stores a value to memory.

**Operands:**
- `%val`: Value to store
- `%ptr`: Pointer to store to

**Result:** None

#### gep (Get Element Pointer)

```
%ptr = gep %base, %idx1, %idx2, ...
```

Computes the address of a sub-element of an aggregate type.

**Operands:**
- `%base`: Base pointer
- `%idx1, %idx2, ...`: Indices

**Result:** Pointer to the element

**Example:**
```
// struct Point { int x, y; };
// Point *p;
// &p->y is:
%y_ptr = gep %p, 0, 1    // First 0 dereferences p, 1 selects field y
```

### Control Flow Instructions

Control flow instructions are **terminator instructions** - they end a basic block. Each basic block must end with exactly one terminator instruction.

**Terminator Instructions:**
- `br` - Unconditional branch
- `br_cond` - Conditional branch
- `ret` / `ret_void` - Return from function

#### br

```
br label %dest
```

Unconditional branch to block `%dest`.

**Operands:**
- `%dest`: Target block

**Result:** None (terminator)

#### br_cond

```
br_cond %cond, label %then, label %else
```

Conditional branch. If `%cond` is true, branches to `%then`, otherwise to `%else`.

**Operands:**
- `%cond`: Condition (i1)
- `%then`: True branch target
- `%else`: False branch target

**Result:** None (terminator)

#### call

```
%result = call %func(%arg1, %arg2, ...)
```

Calls a function.

**Operands:**
- `%func`: Function to call
- `%arg1, %arg2, ...`: Arguments

**Result:** Return value of function (void if function returns void)

#### ret

```
ret %val
```

Returns a value from the function.

**Operands:**
- `%val`: Value to return

**Result:** None (terminator)

#### ret_void

```
ret_void
```

Returns void from the function.

**Result:** None (terminator)

### Type Conversion Instructions

#### trunc

```
%result = trunc %val to T
```

Truncates an integer to a smaller integer type.

**Operands:**
- `%val`: Integer value
- `T`: Target type (must be smaller)

**Result:** Truncated value of type T

#### zext

```
%result = zext %val to T
```

Zero-extends an integer to a larger integer type.

**Operands:**
- `%val`: Integer value
- `T`: Target type (must be larger)

**Result:** Zero-extended value of type T

#### sext

```
%result = sext %val to T
```

Sign-extends an integer to a larger integer type.

**Operands:**
- `%val`: Integer value
- `T`: Target type (must be larger)

**Result:** Sign-extended value of type T

#### bitcast

```
%result = bitcast %val to T
```

Reinterprets the bits of a value as a different type. Types must have the same size.

**Operands:**
- `%val`: Value to cast
- `T`: Target type (same size)

**Result:** Value of type T

#### ptrtoint

```
%result = ptrtoint %ptr to T
```

Converts a pointer to an integer.

**Operands:**
- `%ptr`: Pointer value
- `T`: Integer type

**Result:** Integer value of type T

#### inttoptr

```
%result = inttoptr %val to T
```

Converts an integer to a pointer.

**Operands:**
- `%val`: Integer value
- `T`: Pointer type

**Result:** Pointer value of type T

### Miscellaneous Instructions

#### phi

```
%result = phi T [%val1, %block1], [%val2, %block2], ...
```

PHI node for SSA form. Selects a value based on which predecessor block control came from.

**Operands:**
- `T`: Result type
- `[%val, %block]` pairs: Value and corresponding predecessor block

**Result:** Selected value of type T

**Example:**
```
loop:
    %i = phi i32 [0, entry], [%i_next, loop]
    // %i is 0 if coming from entry, %i_next if coming from loop
```

**Important:** PHI nodes must appear at the beginning of a basic block, before any non-PHI instructions. The backend handles PHI resolution by emitting copy instructions before branches to the target block.

#### select

```
%result = select %cond, %then_val, %else_val
```

Selects between two values based on a condition. Like the ternary operator (`cond ? then_val : else_val`).

**Operands:**
- `%cond`: Condition (i1)
- `%then_val`: Value if true
- `%else_val`: Value if false

**Result:** Selected value

**Example:**
```
%abs = select %is_negative, %negated, %val
```

## Floating-Point Instructions

### Arithmetic

#### fadd

```
%result = fadd %lhs, %rhs
```

Floating-point addition.

**Operands:**
- `%lhs`: Left operand (f32 or f64)
- `%rhs`: Right operand (same type)

**Result:** Same type as operands

#### fsub

```
%result = fsub %lhs, %rhs
```

Floating-point subtraction.

#### fmul

```
%result = fmul %lhs, %rhs
```

Floating-point multiplication.

#### fdiv

```
%result = fdiv %lhs, %rhs
```

Floating-point division.

#### fneg

```
%result = fneg %val
```

Floating-point negation.

#### fabs

```
%result = fabs %val
```

Floating-point absolute value.

### Comparison

#### fcmp

```
%result = fcmp %lhs, %rhs
```

Floating-point comparison. Returns i1 (boolean).

**Note:** Currently implements equality comparison. Future versions may add ordered/unordered comparisons.

### Conversions

#### fptrunc

```
%result = fptrunc %val to f32
```

Truncates f64 to f32.

#### fpext

```
%result = fpext %val to f64
```

Extends f32 to f64.

#### fptosi

```
%result = fptosi %val to T
```

Converts floating-point to signed integer.

**Operands:**
- `%val`: Float value (f32 or f64)
- `T`: Target integer type (i32, i64, etc.)

#### fptoui

```
%result = fptoui %val to T
```

Converts floating-point to unsigned integer.

#### sitofp

```
%result = sitofp %val to T
```

Converts signed integer to floating-point.

**Operands:**
- `%val`: Signed integer value
- `T`: Target float type (f32 or f64)

#### uitofp

```
%result = uitofp %val to T
```

Converts unsigned integer to floating-point.

### Floating-Point Example

```
define f64 @circle_area(f64 %radius) {
entry:
    %pi = const_f64 3.14159265358979
    %r2 = fmul %radius, %radius
    %area = fmul %pi, %r2
    ret %area
}
```

## Well-Formedness Rules

### Basic Block Rules

1. Every basic block must end with a terminator instruction (br, br_cond, ret, ret_void)
2. Terminator instructions can only appear at the end of a block
3. Every basic block must be reachable from the entry block

### SSA Rules

1. Every value must be defined before it is used
2. Every value must be defined exactly once
3. PHI nodes must have an incoming value for every predecessor block
4. PHI nodes must be at the beginning of a block (before non-PHI instructions)

### Type Rules

1. Operands must have compatible types for each operation
2. Comparison operands must have the same type
3. Store value type must match pointer element type
4. Function call arguments must match parameter types

## Example IR

### Simple Addition Function

```
define i32 @add(i32 %a, i32 %b) {
entry:
    %result = add %a, %b
    ret %result
}
```

### Conditional

```
define i32 @max(i32 %a, i32 %b) {
entry:
    %cmp = cmp_gt %a, %b
    br_cond %cmp, label %then, label %else

then:
    br label %merge

else:
    br label %merge

merge:
    %result = phi i32 [%a, %then], [%b, %else]
    ret %result
}
```

### Loop

```
define i32 @sum_to_n(i32 %n) {
entry:
    br label %loop

loop:
    %i = phi i32 [0, %entry], [%i_next, %loop]
    %sum = phi i32 [0, %entry], [%sum_next, %loop]
    %cmp = cmp_lt %i, %n
    br_cond %cmp, label %body, label %exit

body:
    %sum_next = add %sum, %i
    %i_next = add %i, 1
    br label %loop

exit:
    ret %sum
}
```

### Memory Access

```
define void @swap(ptr<i32> %a, ptr<i32> %b) {
entry:
    %tmp_a = load %a
    %tmp_b = load %b
    store %tmp_b, %a
    store %tmp_a, %b
    ret_void
}
```

### Struct Access

```
// struct Point { int x, y; };

define i32 @get_y(ptr<{i32, i32}> %p) {
entry:
    %y_ptr = gep %p, 0, 1
    %y = load %y_ptr
    ret %y
}
```

## Lowering to Assembly

The backend lowers IR to target assembly:

1. **Instruction Selection**: Map IR operations to target instructions
2. **Register Allocation**: Assign values to registers (currently simple: use fixed registers)
3. **Code Emission**: Generate assembly text

### Example Lowering (x86-64)

IR:
```
%result = add %a, %b
ret %result
```

x86-64 Assembly:
```asm
    movl    %edi, %eax      ; Load %a (first param in EDI)
    addl    %esi, %eax      ; Add %b (second param in ESI)
    ret                      ; Return (result in EAX)
```

### Example Lowering (S/370)

IR:
```
%result = add %a, %b
ret %result
```

S/370 Assembly:
```asm
         L     R2,0(,R11)       Load addr of param 0
         N     R2,=X'7FFFFFFF'  Clear VL bit
         L     R2,0(,R2)        Load param value
         L     R3,4(,R11)       Load addr of param 1
         N     R3,=X'7FFFFFFF'  Clear VL bit
         L     R3,0(,R3)        Load param value
         AR    R2,R3            Add registers
         LR    R15,R2           Result in R15
         ... epilogue ...
         BR    R14              Return
```

### Example Lowering (ARM64)

IR:
```
%result = add %a, %b
ret %result
```

ARM64 Assembly (Linux):
```asm
        ldr x9, [x29, #-16]     ; Load %a from stack
        ldr x10, [x29, #-24]    ; Load %b from stack
        add x0, x9, x10         ; Add
        str x0, [x29, #-8]      ; Store result
        ldr x0, [x29, #-8]      ; Load result for return
        add sp, sp, #32         ; Restore stack
        ldp x29, x30, [sp], #16 ; Restore frame pointer and link register
        ret                     ; Return
```

ARM64 Assembly (macOS/Darwin):
```asm
        ldr x9, [x29, #-16]     ; Load %a from stack
        ldr x10, [x29, #-24]    ; Load %b from stack
        add x0, x9, x10         ; Add
        str x0, [x29, #-8]      ; Store result
        ldr x0, [x29, #-8]      ; Load result for return
        add sp, sp, #32         ; Restore stack
        ldp x29, x30, [sp], #16 ; Restore frame pointer and link register
        ret                     ; Return
```

**Note:** The ARM64 backend uses a stack-based approach where all SSA values are stored in stack slots. This simplifies register allocation but may generate more load/store instructions than necessary. Future optimizations may improve this.

## IR Debug/Dump API

ANVIL provides functions for inspecting IR structures, which is invaluable for debugging and understanding the generated IR.

### Printing IR

The debug API is automatically included via `anvil.h`:

```c
#include <anvil/anvil.h>

// Print module IR to stdout
anvil_print_module(mod);

// Print function IR to stdout
anvil_print_func(func);

// Print single instruction to stdout
anvil_print_instr(instr);
```

### Dumping to FILE*

For custom output destinations:

```c
// Dump to stderr for debugging
anvil_dump_module(stderr, mod);
anvil_dump_func(stderr, func);
anvil_dump_block(stderr, block);
anvil_dump_instr(stderr, instr);
anvil_dump_value(stderr, val);
anvil_dump_type(stderr, type);

// Dump to file
FILE *f = fopen("ir_dump.txt", "w");
anvil_dump_module(f, mod);
fclose(f);
```

### Converting to String

For programmatic manipulation:

```c
// Get IR as string (caller must free)
char *ir_str = anvil_module_to_string(mod);
printf("IR length: %zu bytes\n", strlen(ir_str));
free(ir_str);

// Get function IR as string
char *func_str = anvil_func_to_string(func);
// ... use func_str ...
free(func_str);
```

### Output Format

The IR dump format is human-readable and similar to LLVM IR:

```
; ModuleID = 'my_module'
; Functions: 2, Globals: 1

@counter = external global i32 42

define external i32 @factorial(i32 %arg0) {
; Stack size: 0 bytes, max call args: 1
entry:
    %cmp = cmp_le i8 %arg0, 1
    br_cond %cmp, label %base_case, label %recurse

recurse:
    %n_minus_1 = sub i32 %arg0, 1
    %rec_result = call i32 @factorial, %n_minus_1
    %product = mul i32 %arg0, %rec_result
    ret %product

base_case:
    ret 1

}

define external i32 @test_func(i32 %arg0, i32 %arg1) {
; Stack size: 0 bytes, max call args: 0
entry:
    %cmp = cmp_gt i8 %arg0, %arg1
    br_cond %cmp, label %then, label %else

then:
    %sum = add i32 %arg0, %arg1
    br label %merge

else:
    %diff = sub i32 %arg0, %arg1
    br label %merge

merge:
    %result = phi i32 %sum, %diff [%sum, %then] [%diff, %else]
    ret %result

}
```

### Format Details

| Element | Format | Example |
|---------|--------|---------|
| Module header | `; ModuleID = 'name'` | `; ModuleID = 'my_module'` |
| Global variable | `@name = linkage global type [init]` | `@counter = external global i32 42` |
| Function definition | `define linkage type @name(params)` | `define external i32 @factorial(i32 %arg0)` |
| Basic block | `label:` | `entry:` |
| Instruction | `%result = op type operands` | `%sum = add i32 %a, %b` |
| Branch | `br label %target` | `br label %loop` |
| Conditional branch | `br_cond %cond, label %t, label %f` | `br_cond %cmp, label %then, label %else` |
| PHI node | `%r = phi type [val, block]...` | `%i = phi i32 [0, %entry], [%next, %loop]` |
| Return | `ret value` or `ret_void` | `ret %result` |

## Advanced Examples

### Recursive Factorial

```
define i32 @factorial(i32 %n) {
entry:
    %cmp = cmp_le %n, 1
    br_cond %cmp, label %base, label %recurse

base:
    ret 1

recurse:
    %n_minus_1 = sub %n, 1
    %rec = call @factorial(%n_minus_1)
    %result = mul %n, %rec
    ret %result
}
```

### Array Sum with Loop

```
define i32 @sum_array(ptr<i32> %arr, i32 %len) {
entry:
    br label %loop

loop:
    %i = phi i32 [0, %entry], [%i_next, %body]
    %sum = phi i32 [0, %entry], [%sum_next, %body]
    %cmp = cmp_lt %i, %len
    br_cond %cmp, label %body, label %exit

body:
    %ptr = gep %arr, %i
    %val = load %ptr
    %sum_next = add %sum, %val
    %i_next = add %i, 1
    br label %loop

exit:
    ret %sum
}
```

### Struct Field Access

```
// struct Point { int x; int y; };

define i32 @distance_squared(ptr<{i32, i32}> %p1, ptr<{i32, i32}> %p2) {
entry:
    ; Load p1.x and p1.y
    %p1_x_ptr = struct_gep %p1, 0
    %p1_y_ptr = struct_gep %p1, 1
    %p1_x = load %p1_x_ptr
    %p1_y = load %p1_y_ptr
    
    ; Load p2.x and p2.y
    %p2_x_ptr = struct_gep %p2, 0
    %p2_y_ptr = struct_gep %p2, 1
    %p2_x = load %p2_x_ptr
    %p2_y = load %p2_y_ptr
    
    ; Compute (p2.x - p1.x)^2 + (p2.y - p1.y)^2
    %dx = sub %p2_x, %p1_x
    %dy = sub %p2_y, %p1_y
    %dx2 = mul %dx, %dx
    %dy2 = mul %dy, %dy
    %result = add %dx2, %dy2
    ret %result
}
```

### Calling External Functions

```
; Declare external function (malloc)
declare ptr<i8> @malloc(i64)

define ptr<i32> @alloc_array(i32 %count) {
entry:
    ; Calculate size: count * sizeof(i32)
    %count64 = zext %count to i64
    %size = mul %count64, 4
    
    ; Call malloc
    %mem = call @malloc(%size)
    
    ; Cast to i32*
    %result = bitcast %mem to ptr<i32>
    ret %result
}
```

### Conditional with Select

```
define i32 @abs(i32 %x) {
entry:
    %zero = const_i32 0
    %is_neg = cmp_lt %x, %zero
    %negated = neg %x
    %result = select %is_neg, %negated, %x
    ret %result
}
```

## Value Kinds

ANVIL IR values can be one of several kinds:

| Kind | Description | Example |
|------|-------------|---------|
| `ANVIL_VAL_CONST_INT` | Integer constant | `42`, `-1` |
| `ANVIL_VAL_CONST_FLOAT` | Float constant | `3.14`, `2.718` |
| `ANVIL_VAL_CONST_NULL` | Null pointer | `null` |
| `ANVIL_VAL_CONST_STRING` | String constant | `"hello"` |
| `ANVIL_VAL_CONST_ARRAY` | Array constant | `[1, 2, 3]` |
| `ANVIL_VAL_PARAM` | Function parameter | `%arg0` |
| `ANVIL_VAL_INSTR` | Instruction result | `%result` |
| `ANVIL_VAL_GLOBAL` | Global variable | `@counter` |
| `ANVIL_VAL_FUNC` | Function reference | `@factorial` |

## Linkage Types

| Linkage | Description |
|---------|-------------|
| `ANVIL_LINK_EXTERNAL` | Visible outside module (default) |
| `ANVIL_LINK_INTERNAL` | Only visible within module |
| `ANVIL_LINK_PRIVATE` | Not visible outside function |

## IR Operations Summary

| Category | Operations |
|----------|------------|
| **Arithmetic** | add, sub, mul, sdiv, udiv, smod, umod, neg |
| **Bitwise** | and, or, xor, not, shl, shr, sar |
| **Comparison** | cmp_eq, cmp_ne, cmp_lt, cmp_le, cmp_gt, cmp_ge, cmp_ult, cmp_ule, cmp_ugt, cmp_uge |
| **Memory** | alloca, load, store, gep, struct_gep |
| **Control Flow** | br, br_cond, call, ret, ret_void |
| **Type Conversion** | trunc, zext, sext, bitcast, ptrtoint, inttoptr |
| **Floating-Point** | fadd, fsub, fmul, fdiv, fneg, fabs, fcmp |
| **FP Conversion** | fptrunc, fpext, fptosi, fptoui, sitofp, uitofp |
| **Miscellaneous** | phi, select |
