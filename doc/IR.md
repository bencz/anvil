# ANVIL Intermediate Representation Specification

This document specifies the ANVIL IR (Intermediate Representation).

## Overview

ANVIL IR is a low-level, typed, SSA-based intermediate representation designed for code generation. It is similar in spirit to LLVM IR but simpler.

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

#### select

```
%result = select %cond, %then_val, %else_val
```

Selects between two values based on a condition. Like the ternary operator.

**Operands:**
- `%cond`: Condition (i1)
- `%then_val`: Value if true
- `%else_val`: Value if false

**Result:** Selected value

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
