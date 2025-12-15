# ANVIL Documentation Index

## Overview

This documentation provides comprehensive technical details about the ANVIL library for compiler developers, contributors

## Documents

| Document | Description |
|----------|-------------|
| [API.md](API.md) | Complete API reference with all functions, types, and enums |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Internal architecture and design decisions |
| [IR.md](IR.md) | Intermediate Representation specification |
| [OPTIMIZATION.md](OPTIMIZATION.md) | IR optimization passes and pass manager |
| [BACKENDS.md](BACKENDS.md) | Backend system and how to implement new backends |
| [MAINFRAME.md](MAINFRAME.md) | IBM Mainframe backends (S/370, S/390, z/Architecture) |
| [HACKING.md](HACKING.md) | Guide for contributors and hackers |

## Quick Reference

### Creating a Simple Function

```c
anvil_ctx_t *ctx = anvil_ctx_create();
anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64);
anvil_module_t *mod = anvil_module_create(ctx, "example");

anvil_type_t *i32 = anvil_type_i32(ctx);
anvil_type_t *params[] = { i32, i32 };
anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);

anvil_func_t *func = anvil_func_create(mod, "add", func_type, ANVIL_LINK_EXTERNAL);
anvil_block_t *entry = anvil_func_get_entry(func);
anvil_set_insert_point(ctx, entry);

anvil_value_t *a = anvil_func_get_param(func, 0);
anvil_value_t *b = anvil_func_get_param(func, 1);
anvil_value_t *result = anvil_build_add(ctx, a, b, "result");
anvil_build_ret(ctx, result);

char *output = NULL;
size_t len = 0;
anvil_module_codegen(mod, &output, &len);
```

### Supported Architectures

| Architecture | Enum Value | Bits | Endian | Stack | FP Format |
|--------------|------------|------|--------|-------|-----------|
| x86 | `ANVIL_ARCH_X86` | 32 | Little | Down | IEEE 754 |
| x86-64 | `ANVIL_ARCH_X86_64` | 64 | Little | Down | IEEE 754 |
| S/370 | `ANVIL_ARCH_S370` | 24 | Big | Up | HFP |
| S/370-XA | `ANVIL_ARCH_S370_XA` | 31 | Big | Up | HFP |
| S/390 | `ANVIL_ARCH_S390` | 31 | Big | Up | HFP |
| z/Architecture | `ANVIL_ARCH_ZARCH` | 64 | Big | Up | HFP+IEEE |
| PowerPC 32 | `ANVIL_ARCH_PPC32` | 32 | Big | Down | IEEE 754 |
| PowerPC 64 | `ANVIL_ARCH_PPC64` | 64 | Big | Down | IEEE 754 |
| PowerPC 64 LE | `ANVIL_ARCH_PPC64LE` | 64 | Little | Down | IEEE 754 |

**FP Format Legend:**
- **IEEE 754**: Standard IEEE floating-point
- **HFP**: IBM Hexadecimal Floating Point (base-16 exponent)
- **HFP+IEEE**: Both HFP and IEEE 754 supported

### Type Quick Reference

| Type | Function | Size |
|------|----------|------|
| void | `anvil_type_void(ctx)` | 0 |
| i8 | `anvil_type_i8(ctx)` | 1 |
| i16 | `anvil_type_i16(ctx)` | 2 |
| i32 | `anvil_type_i32(ctx)` | 4 |
| i64 | `anvil_type_i64(ctx)` | 8 |
| f32 | `anvil_type_f32(ctx)` | 4 |
| f64 | `anvil_type_f64(ctx)` | 8 |
| ptr | `anvil_type_ptr(ctx, pointee)` | arch-dependent |

### IR Operations Quick Reference

| Category | Operations |
|----------|------------|
| Arithmetic | add, sub, mul, sdiv, udiv, smod, umod, neg |
| Bitwise | and, or, xor, not, shl, shr, sar |
| Compare | cmp_eq, cmp_ne, cmp_lt, cmp_le, cmp_gt, cmp_ge |
| Memory | alloca, load, store, gep, struct_gep |
| Control | br, br_cond, call, ret |
| Convert | trunc, zext, sext, bitcast, ptrtoint, inttoptr |
| Float | fadd, fsub, fmul, fdiv, fneg, fabs, fcmp |
| FP Convert | fptrunc, fpext, fptosi, fptoui, sitofp, uitofp |

## Recent Features

### Struct Support (STRUCT_GEP)
- Struct field access via `anvil_build_struct_gep()` for all backends
- Automatic field offset calculation at compile time
- Example: `examples/struct_test.c`

### Array Support (GEP)
- Full array indexing via `anvil_build_gep()` for all backends
- Automatic element size calculation and shift optimization
- Example: `examples/array_test.c`

### Floating-Point Support
- Full FP arithmetic for all mainframe backends
- **HFP**: S/370, S/370-XA, S/390 (ADR, MDR, DDR)
- **IEEE 754**: z/Architecture, S/390 optional (ADBR, MDBR, DDBR)
- FP format selection via `anvil_ctx_set_fp_format()`

### Control Flow
- Full loop and conditional support
- Function-prefixed labels (`func$block`) for unique naming
- Direct conditional branches

### Local Variables
- Stack slot allocation via `alloca`
- Direct stack offset addressing
- Automatic dynamic area sizing (includes FP temp areas)

### GCCMVS Compatibility (Mainframes)
- **CSECT**: Blank (no module name prefix)
- **AMODE/RMODE**: `AMODE ANY`, `RMODE ANY` for maximum flexibility
- **Function Names**: UPPERCASE (e.g., `FACTORIAL`, `SUM_ARRAY`)
- **Stack Allocation**: Direct stack offset from R13 (no GETMAIN/FREEMAIN)
- **VL Bit**: NOT cleared, allowing full 31/64-bit addressing
- **Block Labels**: `FUNCNAME$BLOCKNAME` format

### Global Variables Support
- Full support for global variables on all backends
- Direct load/store to globals (no intermediate address calculation)
- Type-aware storage allocation (C, H, F, FD, E, D for mainframes)
- Support for initialized globals with `DC` (Define Constant)
- UPPERCASE naming convention for mainframes (GCCMVS compatible)
- Example: `examples/global_test.c`

### Mainframe Optimizations
- **AHI/AGHI**: Immediate addition (S/390, z/Architecture)
- **Relative branches**: J/JNZ for better performance
- **Direct stack access**: No intermediate registers for locals
- **Direct global access**: L/ST for S/370-S/390, LGRL/STGRL for z/Architecture
- **Native FP conversion**: CFDBR for IEEE float→int (z/Architecture)
- **Stack-based code**: Faster than GETMAIN/FREEMAIN
