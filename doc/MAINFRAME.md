# IBM Mainframe Backends

This document provides detailed information about the IBM mainframe backends (S/370, S/390, z/Architecture).

## Overview

ANVIL supports three generations of IBM mainframe architecture:

| Architecture | Bits | Introduced | Key Features |
|--------------|------|------------|--------------|
| S/370 | 24 | 1970 | Base architecture |
| S/370-XA | 31 | 1983 | 31-bit addressing |
| S/390 | 31 | 1990 | Extended addressing, new instructions |
| z/Architecture | 64 | 2000 | 64-bit, relative addressing |

## Common Characteristics

All IBM mainframe architectures share:

1. **Big-endian** byte order
2. **Stack grows upward** (toward higher addresses)
3. **Chained save areas** instead of push/pop
4. **R1 parameter passing** convention
5. **HLASM** assembly syntax

## Register Conventions

All three architectures use the same register conventions (MVS Linkage):

| Register | Usage |
|----------|-------|
| R0 | Work register (volatile, destroyed by system calls) |
| R1 | Parameter list pointer |
| R2-R10 | General purpose / work registers |
| R11 | Saved parameter pointer (ANVIL convention) |
| R12 | Base register for addressability |
| R13 | Save area pointer |
| R14 | Return address |
| R15 | Entry point address / return code |

## Save Area Format

### S/370, S/370-XA and S/390 (72 bytes)

```
Offset  Size  Content
------  ----  -------
+0      4     Reserved (used by PL/I)
+4      4     Pointer to previous save area (caller's)
+8      4     Pointer to next save area (callee's)
+12     4     R14 (return address)
+16     4     R15 (entry point)
+20     4     R0
+24     4     R1
+28     4     R2
+32     4     R3
+36     4     R4
+40     4     R5
+44     4     R6
+48     4     R7
+52     4     R8
+56     4     R9
+60     4     R10
+64     4     R11
+68     4     R12
```

### z/Architecture F4SA (144 bytes)

```
Offset  Size  Content
------  ----  -------
+0      4     Reserved
+4      4     Reserved
+8      8     Pointer to previous save area (caller's)
+16     8     Pointer to next save area (callee's)
+24     8     R14 (return address)
+32     8     R15 (entry point)
+40     8     R0
+48     8     R1
+56     8     R2
+64     8     R3
+72     8     R4
+80     8     R5
+88     8     R6
+96     8     R7
+104    8     R8
+112    8     R9
+120    8     R10
+128    8     R11
+136    8     R12
```

## Parameter Passing

### MVS Convention

Parameters are passed via R1, which points to a list of addresses:

```
R1 --> +0: Address of parameter 1
       +4: Address of parameter 2  (or +8 for 64-bit)
       +8: Address of parameter 3  (or +16 for 64-bit)
       ...
       +n: Address of parameter N (with high bit set)
```

The high-order bit of the last parameter address is set to 1 (VL bit).

### Accessing Parameters

```hlasm
* R11 contains saved R1 (parameter list pointer)

* Load first parameter (32-bit)
         L     R2,0(,R11)         Load address of param 0
         L     R2,0(,R2)          Load actual value

* Load second parameter
         L     R3,4(,R11)         Load address of param 1
         L     R3,0(,R3)          Load actual value
```

For 64-bit (z/Architecture):

```hlasm
* Load first parameter (64-bit)
         LG    R2,0(,R11)         Load address of param 0
         LG    R2,0(,R2)          Load actual value
```

**Note:** ANVIL does NOT clear the VL (Variable List) bit when loading parameters. This allows full 31-bit or 64-bit addressing. The VL bit is only set on the last parameter address and is handled by the caller, not the callee.

## GCCMVS Compatibility

ANVIL generates code compatible with GCCMVS conventions for maximum portability and compatibility with existing mainframe toolchains.

### Key Conventions

| Convention | ANVIL Implementation |
|------------|---------------------|
| **CSECT** | Blank (no module name prefix) |
| **AMODE/RMODE** | `AMODE ANY`, `RMODE ANY` |
| **Function Names** | UPPERCASE (e.g., `FACTORIAL`, `SUM_ARRAY`) |
| **Block Labels** | `FUNCNAME$BLOCKNAME` format |
| **Stack Allocation** | Direct stack offset from R13 (no GETMAIN) |
| **VL Bit** | NOT cleared (allows full addressing) |

### Why These Conventions Matter

1. **CSECT Blank**: Allows the linker to assign the control section name, improving compatibility with different link-edit configurations.

2. **AMODE/RMODE ANY**: Enables the program to run in any addressing mode (24-bit, 31-bit, or 64-bit), maximizing flexibility.

3. **Uppercase Names**: Standard mainframe convention for external symbols. Required for proper linkage resolution.

4. **No VL Bit Clearing**: The instruction `N Rx,=X'7FFFFFFF'` was previously used to clear the high-order bit of parameter addresses. This limits addressing to 2GB (31-bit). ANVIL now preserves the full address, allowing access to all 4GB (32-bit) or more (64-bit).

5. **Stack Allocation**: Using `LA R2,72(,R13)` instead of `GETMAIN` is faster and follows the GCCMVS pattern. Each function's stack frame is allocated relative to the caller's save area.

### Generated Header (GCCMVS Style)

```hlasm
***********************************************************************
*        Generated by ANVIL for IBM S/370
***********************************************************************
         CSECT
         AMODE ANY
         RMODE ANY
*
```

## Stack-Based Code Generation

ANVIL generates stack-based (non-GETMAIN) code for better performance and GCCMVS compatibility.

### Stack Frame Layout

Each function allocates its stack frame relative to R13:

```
Caller's R13 --> +0:   Caller's Save Area (72 bytes for 32-bit)
                 +72:  Our Save Area starts here
                       +0:   Reserved
                       +4:   Pointer to previous SA (caller's)
                       +8:   Pointer to next SA (callee's)
                       +12:  R14 (return address)
                       ...
                 +144: Local variables (for 32-bit)
                 +N:   Parameter list for outgoing calls
```

### Prologue (Stack-Based)

**S/370 / S/370-XA / S/390 Example:**
```hlasm
FACTORIAL DS    0H
         STM   R14,R12,12(R13)    Save caller's registers
         LR    R12,R15            Copy entry point to base reg
         USING FACTORIAL,R12      Establish addressability
         LR    R11,R1             Save parameter list pointer
*        Set up save area chain (stack allocation)
         LA    R2,72(,R13)        R2 -> our save area (after caller's)
         ST    R13,4(,R2)         Chain: new->prev = caller's
         ST    R2,8(,R13)         Chain: caller->next = new
         LR    R13,R2             R13 -> our save area
*
```

**z/Architecture Example:**
```hlasm
FACTORIAL DS    0H
         STMG  R14,R12,24(R13)    Save caller's registers (64-bit)
         LGR   R12,R15            Copy entry point to base reg
         USING FACTORIAL,R12      Establish addressability
         LGR   R11,R1             Save parameter list pointer
*        Set up save area chain (stack allocation)
         LA    R2,144(,R13)       R2 -> our save area (144 bytes for 64-bit)
         STG   R13,8(,R2)         Chain: new->prev = caller's
         STG   R2,16(,R13)        Chain: caller->next = new
         LGR   R13,R2             R13 -> our save area
*
```

### Epilogue (Stack-Based)

**S/370 / S/370-XA / S/390 Example:**
```hlasm
*        Function epilogue
         L     R13,4(,R13)        Restore caller's SA pointer
         L     R14,12(,R13)       Restore return address
         LM    R0,R12,20(,R13)    Restore R0-R12
         BR    R14                Return to caller
```

**z/Architecture Example:**
```hlasm
*        Function epilogue
         LG    R13,8(,R13)        Restore caller's SA pointer
         LG    R14,24(,R13)       Restore return address
         LMG   R0,R12,40(,R13)    Restore R0-R12
         BR    R14                Return to caller
```

**Key Difference from GETMAIN-based code:** No need to save/restore R15 around FREEMAIN, making the epilogue simpler and faster.

## Legacy GETMAIN-Based Code (Historical Reference)

For reference, the older GETMAIN-based approach is documented below. ANVIL no longer generates this style of code, but it may be useful for understanding existing mainframe programs.

### Dynamic Area Layout

```
R13 --> +0:   Save Area (72 or 144 bytes)
        +SA:  Local variables (stack slots)
        +N:   Parameter list for outgoing calls
```

### Local Variable Allocation

ANVIL allocates local variables in the dynamic area using fixed stack slots:

**S/370 / S/370-XA / S/390 (32-bit):**
```
R13 --> +0:   Save Area (72 bytes)
        +72:  Local var 0 (4 bytes)
        +76:  Local var 1 (4 bytes)
        +80:  Local var 2 (4 bytes)
        ...
        +N:   Parameter list for calls
```

**z/Architecture (64-bit):**
```
R13 --> +0:   Save Area (144 bytes)
        +144: Local var 0 (8 bytes)
        +152: Local var 1 (8 bytes)
        +160: Local var 2 (8 bytes)
        ...
        +N:   Parameter list for calls
```

### Stack Slot Access

ANVIL generates direct stack slot access for local variables:

```hlasm
* Initialize local variable to 0
         XC    72(4,R13),72(R13)  Clear 4 bytes at offset 72

* Store value to local variable
         ST    R2,72(,R13)        Store to stack slot

* Load value from local variable
         L     R2,72(,R13)        Load from stack slot
```

This is more efficient than using intermediate registers for address calculation.

### Prologue (Reentrant)

**S/370 Example:**
```hlasm
FUNC     DS    0H
         STM   R14,R12,12(R13)    Save caller's registers
         LR    R12,R15            Establish base register
         USING FUNC,R12           Tell assembler
         LR    R11,R1             Save parameter list pointer
*
         GETMAIN R,LV=DYN@FUNC    Allocate dynamic storage
         LR    R2,R1              R2 -> new area
         ST    R13,4(,R2)         Chain: new->prev = caller
         ST    R2,8(,R13)         Chain: caller->next = new
         LR    R13,R2             R13 -> our area
```

### Epilogue (Reentrant)

**S/370 Example:**
```hlasm
*        Function epilogue
         LR    R2,R15             Save return value
         LR    R1,R13             R1 -> area to free
         L     R13,4(,R13)        Restore caller's SA pointer
         FREEMAIN R,LV=DYN@FUNC,A=(1)  Free our storage
         LR    R15,R2             Restore return value
         L     R14,12(,R13)       Restore return address only
         LM    R0,R12,20(,R13)    Restore R0-R12 (skip R14,R15)
         BR    R14                Return
```

**Critical Note:** Do NOT use `LM R14,R12,12(R13)` in the epilogue as it overwrites R15 (the return value).

## S/370 Specifics

### Addressing Mode

- 24-bit addresses (16MB address space)
- High byte of address is ignored
- AMODE 24, RMODE 24

### Key Instructions

| Instruction | Description |
|-------------|-------------|
| L | Load (32-bit) |
| ST | Store (32-bit) |
| LR | Load Register |
| AR | Add Register |
| SR | Subtract Register |
| MR | Multiply Register (even-odd pair) |
| DR | Divide Register (even-odd pair) |
| NR | AND Register |
| OR | OR Register |
| XR | XOR Register |
| CR | Compare Register |
| LM | Load Multiple |
| STM | Store Multiple |
| LA | Load Address (0-4095) |
| B | Branch |
| BE | Branch if Equal |
| BNE | Branch if Not Equal |
| BL | Branch if Low |
| BH | Branch if High |

### Limitations

- No immediate instructions (except LA for 0-4095)
- 4KB addressability limit per base register
- Division requires even-odd register pair

## S/370-XA Specifics

### Addressing Mode

- 31-bit addresses (2GB address space)
- Bit 0 is AMODE flag (must be preserved)
- AMODE 31, RMODE ANY

### Changes from S/370

- Uses `BASR` (Branch and Save Register) instead of `BALR` for calls to preserve the high-order bit of the return address.
- Allocation uses `LOC=ANY` to allow memory above 16MB line.
- Does NOT include S/390 immediate instructions (AHI, LHI, etc.) or relative branches.

## S/370-XA Specifics

### Addressing Mode

- 31-bit addresses (2GB address space)
- Bit 0 is AMODE flag (must be preserved)
- AMODE 31, RMODE ANY

### Changes from S/370

- Uses `BASR` (Branch and Save Register) instead of `BALR` for calls to preserve the high-order bit of the return address.
- Allocation uses `LOC=ANY` to allow memory above 16MB line.
- Does NOT include S/390 immediate instructions (AHI, LHI, etc.) or relative branches.

## S/390 Specifics

### Addressing Mode

- 31-bit addresses (2GB address space)
- Bit 0 is AMODE flag
- AMODE 31, RMODE ANY

### Additional Instructions

| Instruction | Description |
|-------------|-------------|
| LHI | Load Halfword Immediate (-32768 to 32767) |
| AHI | Add Halfword Immediate |
| MHI | Multiply Halfword Immediate |
| CHI | Compare Halfword Immediate |
| MSR | Multiply Single Register |
| J | Jump (relative) |
| JE | Jump if Equal (relative) |
| JNE | Jump if Not Equal (relative) |
| JL | Jump if Low (relative) |
| JH | Jump if High (relative) |
| JNH | Jump if Not High (relative) |
| JNL | Jump if Not Low (relative) |
| BASR | Branch and Save Register |

### Improvements over S/370

- Halfword immediate instructions
- Relative branch instructions (no base register needed)
- More efficient multiplication

### ANVIL Optimizations for S/390

ANVIL automatically uses optimized instructions when possible:

```hlasm
* Instead of:
         LA    R3,1
         AR    R2,R3

* ANVIL generates:
         AHI   R2,1            Add halfword immediate
```

## z/Architecture Specifics

### Addressing Mode

- 64-bit addresses
- AMODE 64, RMODE 64

### 64-bit Instructions

| Instruction | Description |
|-------------|-------------|
| LG | Load (64-bit) |
| STG | Store (64-bit) |
| LGR | Load Register (64-bit) |
| AGR | Add Register (64-bit) |
| SGR | Subtract Register (64-bit) |
| MSGR | Multiply Single Register (64-bit) |
| NGR | AND Register (64-bit) |
| OGR | OR Register (64-bit) |
| XGR | XOR Register (64-bit) |
| CGR | Compare Register (64-bit) |
| LMG | Load Multiple (64-bit) |
| STMG | Store Multiple (64-bit) |
| LGHI | Load Halfword Immediate (64-bit) |
| LGFI | Load Fullword Immediate (64-bit) |
| AGHI | Add Halfword Immediate (64-bit) |
| LARL | Load Address Relative Long |
| BRASL | Branch Relative and Save Long |
| LGRL | Load Relative Long |
| NIHH | AND Immediate High Halfword |

### Key Differences from 32-bit

1. **Save area is 144 bytes** (F4SA format)
2. **STMG offset is 24**, not 8 (offsets 8 and 16 are for SA chaining)
3. **Parameter list entries are 8 bytes** each
4. **Use STORAGE macro** instead of GETMAIN for 64-bit

### ANVIL Optimizations for z/Architecture

ANVIL automatically uses optimized 64-bit instructions:

```hlasm
* Instead of:
         LGHI  R3,1
         AGR   R2,R3

* ANVIL generates:
         AGHI  R2,1            Add halfword immediate 64-bit
```

## HLASM Syntax

### Column Layout

```
Column 1-8:   Label (optional)
Column 10+:   Operation
Column 16+:   Operands
Column 72:    Continuation marker
```

### Common Directives

```hlasm
name     CSECT                    Control Section
name     AMODE 24/31/64           Addressing Mode
name     RMODE 24/ANY/64          Residency Mode
         USING name,Rn            Establish addressability
         DROP  Rn                 Drop addressability
         LTORG                    Literal pool
         DS    nC                 Define Storage (character)
         DS    nF                 Define Storage (fullword)
         DS    nFD                Define Storage (doubleword)
         DC    F'value'           Define Constant (fullword)
         EQU   value              Equate symbol
         END   entry              End of assembly
```

### Literal Notation

```hlasm
         L     R2,=F'100'         Fullword literal
         L     R2,=H'50'          Halfword literal
         L     R2,=X'7FFFFFFF'    Hex literal
         L     R2,=A(SYMBOL)      Address literal
         L     R2,=V(EXTERNAL)    External address literal
```

## Generated Code Examples

### Simple Add Function (GCCMVS Style)

**S/370:**
```hlasm
***********************************************************************
*        Generated by ANVIL for IBM S/370
***********************************************************************
         CSECT
         AMODE ANY
         RMODE ANY
*
ADD      DS    0H
         STM   R14,R12,12(R13)    Save caller's registers
         LR    R12,R15            Copy entry point to base reg
         USING ADD,R12            Establish addressability
         LR    R11,R1             Save parameter list pointer
*        Set up save area chain (stack allocation)
         LA    R2,72(,R13)        R2 -> our save area
         ST    R13,4(,R2)         Chain: new->prev = caller's
         ST    R2,8(,R13)         Chain: caller->next = new
         LR    R13,R2             R13 -> our save area
*
ADD$ENTRY DS    0H
         L     R2,0(,R11)         Load addr of param 0
         L     R2,0(,R2)          Load param value
         L     R3,4(,R11)         Load addr of param 1
         L     R3,0(,R3)          Load param value
         AR    R2,R3              Add registers
         LR    R15,R2             Result in R15
*        Function epilogue
         L     R13,4(,R13)        Restore caller's SA pointer
         L     R14,12(,R13)       Restore return address
         LM    R0,R12,20(,R13)    Restore R0-R12
         BR    R14                Return to caller
*
         DROP  R12
*
*        Stack frame sizes
DYN@ADD  EQU   88                 Stack frame size for ADD
*
         LTORG                    Literal pool
*
*        Register equates
R0       EQU   0
R1       EQU   1
R2       EQU   2
R3       EQU   3
R4       EQU   4
R5       EQU   5
R6       EQU   6
R7       EQU   7
R8       EQU   8
R9       EQU   9
R10      EQU   10
R11      EQU   11
R12      EQU   12
R13      EQU   13
R14      EQU   14
R15      EQU   15
*
         END   ADD
```

**z/Architecture:**
```hlasm
***********************************************************************
*        Generated by ANVIL for IBM z/Architecture
***********************************************************************
         CSECT
         AMODE ANY
         RMODE ANY
*
ADD      DS    0H
         STMG  R14,R12,24(R13)    Save caller's registers
         LGR   R12,R15            Copy entry point to base reg
         USING ADD,R12            Establish addressability
         LGR   R11,R1             Save parameter list pointer
*        Set up save area chain (stack allocation)
         LA    R2,144(,R13)       R2 -> our save area (144 bytes for 64-bit)
         STG   R13,8(,R2)         Chain: new->prev = caller's
         STG   R2,16(,R13)        Chain: caller->next = new
         LGR   R13,R2             R13 -> our save area
*
ADD$ENTRY DS    0H
         LG    R2,0(,R11)         Load addr of param 0
         LG    R2,0(,R2)          Load param value
         LG    R3,8(,R11)         Load addr of param 1
         LG    R3,0(,R3)          Load param value
         AGR   R2,R3              Add 64-bit registers
         LGR   R15,R2             Result in R15
*        Function epilogue
         LG    R13,8(,R13)        Restore caller's SA pointer
         LG    R14,24(,R13)       Restore return address
         LMG   R0,R12,40(,R13)    Restore R0-R12
         BR    R14                Return to caller
*
         DROP  R12
*
*        Stack frame sizes
DYN@ADD  EQU   176                Stack frame size for ADD
*
         LTORG                    Literal pool
*
         END   ADD
```

## Comparison Operations

### Branch Offset Calculation

**S/370 / S/370-XA / S/390 (32-bit):**
```hlasm
         CR    R2,R3              Compare registers (4 bytes)
         LA    R15,1              Assume true (4 bytes)
         BE    *+6                Skip if equal (4 bytes)
         SR    R15,R15            Set false (2 bytes)
```

Branch offset is +6 because:
- BE instruction: 4 bytes
- SR instruction: 2 bytes
- Total to skip: 6 bytes

**z/Architecture (64-bit):**
```hlasm
         CGR   R2,R3              Compare registers (4 bytes)
         LGHI  R15,1              Assume true (4 bytes)
         JE    *+8                Skip if equal (4 bytes)
         SGR   R15,R15            Set false (4 bytes)
```

Branch offset is +8 because:
- JE instruction: 4 bytes
- SGR instruction: 4 bytes
- Total to skip: 8 bytes

## Function Calls

### Building Parameter List

```hlasm
*        Call setup (reentrant)
         LA    R0,value1          Load first param value
         ST    R0,72(,R13)        Store at offset 72 (after SA)
         LA    R0,value2          Load second param value
         ST    R0,76(,R13)        Store at offset 76
         LA    R1,72(,R13)        R1 -> param list
         OI    76(R13),X'80'      Mark last param (VL bit)
         L     R15,=V(TARGET)     Load target address
         BALR  R14,R15            Call
         NI    76(R13),X'7F'      Clear VL bit (cleanup)
```

## Addressability

### 4KB Limitation

Each base register can address 4096 bytes (offsets 0-4095). For larger programs:

1. **Multiple base registers**: Use R12, R11, R10, etc.
2. **LTORG placement**: Place literal pools every 4KB
3. **Relative addressing** (S/390+): Use relative branches

### USING and DROP

```hlasm
FUNC     DS    0H
         USING FUNC,R12           R12 is base for FUNC
         ...
         USING DATA,R11           R11 is base for DATA section
         ...
         DROP  R11                No longer using R11
```

## Memory Allocation Details

### GETMAIN Parameters

```hlasm
* Register form (R)
         GETMAIN R,LV=length      Basic allocation
         GETMAIN R,LV=length,LOC=ANY    Above 16MB (31-bit)
         GETMAIN R,LV=length,LOC=BELOW  Below 16MB (24-bit)

* After GETMAIN: R1 = address of allocated storage
*                R15 = return code (0 = success)
```

### FREEMAIN Parameters

```hlasm
* Register form (R)
         FREEMAIN R,LV=length,A=(1)     Free storage at address in R1
         FREEMAIN R,LV=length,A=addr    Free storage at specified address
```

### STORAGE Macro (z/OS)

```hlasm
* OBTAIN
         STORAGE OBTAIN,LENGTH=length,LOC=ANY
* R1 = address of allocated storage

* RELEASE
         STORAGE RELEASE,LENGTH=length,ADDR=(1)
* Storage at address in R1 is freed
```

## Control Flow and Labels

### Branch Label Naming

ANVIL generates unique labels using the `function$block` naming convention to avoid conflicts:

```hlasm
sum_to_n$entry     DS    0H
         ...
         B     sum_to_n$loop_cond      Branch to loop condition
sum_to_n$loop_cond DS    0H
         ...
         BH    sum_to_n$loop_end       Branch if i > n
sum_to_n$loop_body DS    0H
         ...
         B     sum_to_n$loop_cond      Loop back
sum_to_n$loop_end  DS    0H
         ...
```

### Loop Example (Sum 1 to N)

**S/390 Generated Code (GCCMVS Style):**
```hlasm
SUM_TO_N$ENTRY DS    0H
         XC    72(4,R13),72(R13)  Init sum = 0
         LA    R2,1
         ST    R2,76(,R13)        Init i = 1
         J     SUM_TO_N$LOOP_COND
*
SUM_TO_N$LOOP_COND DS    0H
         L     R2,76(,R13)        Load i
         L     R3,0(,R11)         Load &n
         L     R3,0(,R3)          Load n
         CR    R2,R3              Compare i : n
         JH    SUM_TO_N$LOOP_END  Exit if i > n
*
SUM_TO_N$LOOP_BODY DS    0H
         L     R2,72(,R13)        Load sum
         L     R3,76(,R13)        Load i
         AR    R2,R3              sum = sum + i
         ST    R2,72(,R13)        Store sum
         L     R2,76(,R13)        Load i
         AHI   R2,1               i = i + 1 (optimized!)
         ST    R2,76(,R13)        Store i
         J     SUM_TO_N$LOOP_COND Loop back
*
SUM_TO_N$LOOP_END DS    0H
         L     R15,72(,R13)       Return sum
```

**Note:** Function and block names are uppercase (`SUM_TO_N$ENTRY`, `SUM_TO_N$LOOP_COND`, etc.) following GCCMVS conventions.

## Array Access (GEP)

ANVIL supports array indexing through the GEP (Get Element Pointer) operation, which computes element addresses using the mainframe's indexed addressing capabilities.

### GEP Code Generation

For `arr[i]` where `arr` is a pointer to `int` (4 bytes):

**S/370, S/370-XA, S/390 (32-bit):**
```hlasm
         L     R2,arr_addr        Load base pointer
         L     R3,i_val           Load index
         SLL   R3,2               Index * 4 (sizeof int)
         AR    R2,R3              Base + offset
         LR    R15,R2             Result pointer
*        Load the element
         L     R15,0(,R2)         Load arr[i]
```

**z/Architecture (64-bit):**
```hlasm
         LG    R2,arr_addr        Load base pointer (64-bit)
         LG    R3,i_val           Load index (64-bit)
         SLLG  R3,R3,2            Index * 4 (64-bit shift)
         AGR   R2,R3              Base + offset (64-bit add)
         LGR   R15,R2             Result pointer
*        Load the element
         L     R15,0(,R2)         Load arr[i] (32-bit int)
```

### Element Size Optimization

The index multiplication uses shifts for power-of-2 sizes:

| Element Type | Size | Instruction |
|--------------|------|-------------|
| i8/u8 | 1 | (no shift) |
| i16/u16 | 2 | `SLL R3,1` |
| i32/u32/f32 | 4 | `SLL R3,2` |
| i64/u64/f64 | 8 | `SLL R3,3` / `SLLG R3,R3,3` |

For non-power-of-2 sizes, `MH` (Multiply Halfword) is used.

### Example: Sum Array Elements

```c
int sum_array(int *arr, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += arr[i];  // Uses GEP
    }
    return sum;
}
```

See `examples/array_test.c` for a complete example.

## Struct Field Access (STRUCT_GEP)

ANVIL supports struct field access through the STRUCT_GEP operation, which computes field addresses using fixed offsets calculated at compile time.

### STRUCT_GEP Code Generation

For `p->field` where `p` is a pointer to a struct:

**S/370, S/370-XA, S/390 (32-bit):**
```hlasm
*        Field at offset 0 (e.g., p->x)
         LR    R15,R2             Struct field at offset 0

*        Field at offset 4 (e.g., p->y)
         LA    R15,4(,R2)        Struct field at offset 4

*        Field at large offset (>4095)
         LR    R15,R2             Load base
         AHI   R15,offset         Add field offset
```

**z/Architecture (64-bit):**
```hlasm
*        Field at offset 0
         LGR   R15,R2             Struct field at offset 0

*        Field at offset 4
         LA    R15,4(,R2)        Struct field at offset 4

*        Field at large offset
         LGR   R15,R2             Load base
         AGHI  R15,offset         Add field offset
```

### Struct Layout

ANVIL calculates struct field offsets automatically with proper alignment:

```c
/* struct Point { int x; int y; } */
/* Field 0 (x): offset 0, size 4 */
/* Field 1 (y): offset 4, size 4 */
/* Total size: 8 bytes */

/* struct Mixed { char a; int b; short c; } */
/* Field 0 (a): offset 0, size 1 */
/* Field 1 (b): offset 4, size 4 (aligned to 4) */
/* Field 2 (c): offset 8, size 2 */
/* Total size: 12 bytes (padded to alignment) */
```

### Example: Distance Squared

```c
int dist_sq(struct Point *p) {
    return p->x * p->x + p->y * p->y;
}
```

**Generated Assembly (S/390):**
```hlasm
*        Load p->x
         LR    R15,R2             Struct field at offset 0
         L     R15,0(,R15)        Load x value
*        Load p->y  
         LA    R15,4(,R2)        Struct field at offset 4
         L     R15,0(,R15)        Load y value
*        Compute x*x + y*y
         MSR   R2,R2              x * x
         MSR   R3,R3              y * y
         AR    R2,R3              x*x + y*y
```

See `examples/struct_test.c` for a complete example.

## Floating-Point Support

### FP Formats

IBM mainframes support two floating-point formats:

| Format | Name | Description |
|--------|------|-------------|
| HFP | Hexadecimal FP | Base-16 exponent, IBM proprietary (S/370, S/390) |
| BFP | Binary FP | IEEE 754 standard (z/Architecture, S/390 optional) |

### FP Registers

All mainframe architectures have 4 FP register pairs (0, 2, 4, 6) for short (32-bit) operations.
z/Architecture extends this to 16 FPRs (0-15).

### HFP Instructions (S/370, S/370-XA, S/390)

```hlasm
*        Load/Store
         LE    0,addr            Load Short (32-bit)
         LD    0,addr            Load Long (64-bit)
         STE   0,addr            Store Short
         STD   0,addr            Store Long
         LER   0,2               Load Register Short
         LDR   0,2               Load Register Long

*        Arithmetic (Short)
         AER   0,2               Add Short
         SER   0,2               Subtract Short
         MER   0,2               Multiply Short
         DER   0,2               Divide Short

*        Arithmetic (Long)
         ADR   0,2               Add Long
         SDR   0,2               Subtract Long
         MDR   0,2               Multiply Long
         DDR   0,2               Divide Long

*        Sign Operations
         LCER  0,0               Load Complement (Negate) Short
         LCDR  0,0               Load Complement (Negate) Long
         LPER  0,0               Load Positive (Abs) Short
         LPDR  0,0               Load Positive (Abs) Long

*        Compare
         CER   0,2               Compare Short
         CDR   0,2               Compare Long
```

### IEEE 754 (BFP) Instructions (z/Architecture, S/390)

```hlasm
*        Arithmetic (Short BFP)
         AEBR  0,2               Add Short BFP
         SEBR  0,2               Subtract Short BFP
         MEEBR 0,2               Multiply Short BFP
         DEBR  0,2               Divide Short BFP

*        Arithmetic (Long BFP)
         ADBR  0,2               Add Long BFP
         SDBR  0,2               Subtract Long BFP
         MDBR  0,2               Multiply Long BFP
         DDBR  0,2               Divide Long BFP

*        Sign Operations (BFP)
         LCEBR 0,0               Load Complement Short BFP
         LCDBR 0,0               Load Complement Long BFP
         LPEBR 0,0               Load Positive Short BFP
         LPDBR 0,0               Load Positive Long BFP

*        Compare (BFP)
         CEBR  0,2               Compare Short BFP
         CDBR  0,2               Compare Long BFP

*        Conversion (BFP - z/Architecture)
         CEFBR 0,R2              Convert Int to Short BFP
         CDFBR 0,R2              Convert Int to Long BFP
         CFEBR R2,0,0            Convert Short BFP to Int
         CFDBR R2,0,0            Convert Long BFP to Int
```

### Float→Int Conversion (HFP)

For HFP, there's no direct conversion instruction. ANVIL uses the "Magic Number" technique:

```hlasm
*        Convert HFP Double to Integer
         LD    0,fp_value        Load the double
         AW    0,=X'4E00000000000000' Add magic number (unnormalized)
         STD   0,temp            Store result
         L     R15,temp+4        Load low 32 bits (integer result)
```

The magic number `X'4E00000000000000'` has exponent 78 (decimal), representing 16^14.
This shifts the mantissa so the integer portion ends up in the low 32 bits.

### FP Format Selection in ANVIL

```c
/* Select IEEE 754 for z/Architecture */
anvil_ctx_set_target(ctx, ANVIL_ARCH_ZARCH);
anvil_ctx_set_fp_format(ctx, ANVIL_FP_IEEE754);

/* Select HFP for S/390 (default) */
anvil_ctx_set_target(ctx, ANVIL_ARCH_S390);
/* HFP is default, or explicitly: */
anvil_ctx_set_fp_format(ctx, ANVIL_FP_HFP);
```

### Dynamic Area Layout with FP

The dynamic storage area includes space for FP temporaries:

**S/370, S/370-XA, S/390 (88 bytes minimum):**
```
+0    Save Area (72 bytes)
+72   FP Temp 1 (8 bytes)
+80   FP Temp 2 (8 bytes) - for conversions
+88   Local variables start
```

**z/Architecture (160 bytes minimum):**
```
+0    Save Area (144 bytes)
+144  FP Temp 1 (8 bytes)
+152  FP Temp 2 (8 bytes) - for conversions
+160  Local variables start
```

## Debugging Tips

1. **Check save area chaining**: Verify +4 and +8 offsets are correct
2. **Verify register preservation**: R2-R12 must be restored
3. **Check VL bit handling**: Clear before using parameter address
4. **Verify STMG/LMG offsets**: 24 for z/Architecture, 12 for 32-bit
5. **Use SNAP macro**: Dump registers and storage for debugging
6. **Check label names**: Ensure branch targets use `func$block` format
7. **Verify stack slot offsets**: 88+ for S/370/S/370-XA/S/390, 160+ for z/Architecture
8. **FP format consistency**: Ensure HFP/BFP instructions match the data format

## References

- IBM z/Architecture Principles of Operation (SA22-7832)
- IBM z/OS MVS Programming: Assembler Services Guide (SA23-1368)
- IBM High Level Assembler Language Reference (SC26-4940)
