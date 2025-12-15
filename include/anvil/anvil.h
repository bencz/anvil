/*
 * ANVIL - Abstract Intermediate Representation Library
 * 
 * A portable IR library for compiler code generation
 * Supports multiple backends: x86, x86-64, S/370, S/390, z/Architecture
 * 
 */

#ifndef ANVIL_H
#define ANVIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Version info */
#define ANVIL_VERSION_MAJOR 0
#define ANVIL_VERSION_MINOR 1
#define ANVIL_VERSION_PATCH 0

/* Forward declarations */
typedef struct anvil_ctx anvil_ctx_t;
typedef struct anvil_module anvil_module_t;
typedef struct anvil_func anvil_func_t;
typedef struct anvil_block anvil_block_t;
typedef struct anvil_value anvil_value_t;
typedef struct anvil_type anvil_type_t;
typedef struct anvil_backend anvil_backend_t;

/* Target architecture */
typedef enum {
    ANVIL_ARCH_X86,          /* x86 32-bit, little-endian, stack grows down */
    ANVIL_ARCH_X86_64,       /* x86-64, little-endian, stack grows down */
    ANVIL_ARCH_S370,         /* IBM S/370, 24-bit addressing, big-endian, stack grows up */
    ANVIL_ARCH_S370_XA,      /* IBM S/370-XA, 31-bit addressing, big-endian, stack grows up */
    ANVIL_ARCH_S390,         /* IBM S/390, 31-bit addressing, big-endian, stack grows up */
    ANVIL_ARCH_ZARCH,        /* IBM z/Architecture, 64-bit, big-endian, stack grows up */
    ANVIL_ARCH_PPC32,        /* PowerPC 32-bit, big-endian, stack grows down */
    ANVIL_ARCH_PPC64,        /* PowerPC 64-bit, big-endian, stack grows down */
    ANVIL_ARCH_PPC64LE,      /* PowerPC 64-bit, little-endian, stack grows down */
    ANVIL_ARCH_ARM64,        /* ARM64/AArch64, little-endian, stack grows down */
    ANVIL_ARCH_COUNT
} anvil_arch_t;

/* Output format */
typedef enum {
    ANVIL_OUTPUT_ASM,        /* Assembly text output */
    ANVIL_OUTPUT_BINARY      /* Binary opcodes (future) */
} anvil_output_t;

/* Assembly syntax for mainframe */
typedef enum {
    ANVIL_SYNTAX_DEFAULT,    /* Default for architecture */
    ANVIL_SYNTAX_HLASM,      /* IBM HLASM for mainframes */
    ANVIL_SYNTAX_GAS,        /* GNU Assembler syntax */
    ANVIL_SYNTAX_NASM,       /* NASM syntax for x86 */
    ANVIL_SYNTAX_MASM        /* Microsoft MASM syntax */
} anvil_syntax_t;

/* Endianness */
typedef enum {
    ANVIL_ENDIAN_LITTLE,
    ANVIL_ENDIAN_BIG
} anvil_endian_t;

/* Stack growth direction */
typedef enum {
    ANVIL_STACK_DOWN,        /* Stack grows toward lower addresses (x86) */
    ANVIL_STACK_UP           /* Stack grows toward higher addresses (mainframe) */
} anvil_stack_dir_t;

/* Floating-point format */
typedef enum {
    ANVIL_FP_IEEE754,        /* IEEE 754 (x86, x86-64, PowerPC, z/Architecture) */
    ANVIL_FP_HFP,            /* IBM Hexadecimal Floating Point (S/370, S/390) */
    ANVIL_FP_HFP_IEEE        /* HFP with IEEE 754 support (z/Architecture, some S/390) */
} anvil_fp_format_t;

/* Data types */
typedef enum {
    ANVIL_TYPE_VOID,
    ANVIL_TYPE_I8,
    ANVIL_TYPE_I16,
    ANVIL_TYPE_I32,
    ANVIL_TYPE_I64,
    ANVIL_TYPE_U8,
    ANVIL_TYPE_U16,
    ANVIL_TYPE_U32,
    ANVIL_TYPE_U64,
    ANVIL_TYPE_F32,
    ANVIL_TYPE_F64,
    ANVIL_TYPE_PTR,
    ANVIL_TYPE_STRUCT,
    ANVIL_TYPE_ARRAY,
    ANVIL_TYPE_FUNC
} anvil_type_kind_t;

/* IR Operations */
typedef enum {
    /* Arithmetic */
    ANVIL_OP_ADD,
    ANVIL_OP_SUB,
    ANVIL_OP_MUL,
    ANVIL_OP_DIV,
    ANVIL_OP_SDIV,           /* Signed division */
    ANVIL_OP_UDIV,           /* Unsigned division */
    ANVIL_OP_MOD,
    ANVIL_OP_SMOD,           /* Signed modulo */
    ANVIL_OP_UMOD,           /* Unsigned modulo */
    ANVIL_OP_NEG,
    
    /* Bitwise */
    ANVIL_OP_AND,
    ANVIL_OP_OR,
    ANVIL_OP_XOR,
    ANVIL_OP_NOT,
    ANVIL_OP_SHL,            /* Shift left */
    ANVIL_OP_SHR,            /* Shift right (logical) */
    ANVIL_OP_SAR,            /* Shift right (arithmetic) */
    
    /* Comparison */
    ANVIL_OP_CMP_EQ,
    ANVIL_OP_CMP_NE,
    ANVIL_OP_CMP_LT,
    ANVIL_OP_CMP_LE,
    ANVIL_OP_CMP_GT,
    ANVIL_OP_CMP_GE,
    ANVIL_OP_CMP_ULT,        /* Unsigned less than */
    ANVIL_OP_CMP_ULE,
    ANVIL_OP_CMP_UGT,
    ANVIL_OP_CMP_UGE,
    
    /* Memory */
    ANVIL_OP_LOAD,
    ANVIL_OP_STORE,
    ANVIL_OP_ALLOCA,         /* Stack allocation */
    ANVIL_OP_GEP,            /* Get element pointer (array indexing) */
    ANVIL_OP_STRUCT_GEP,     /* Get struct field pointer (fixed offset) */
    
    /* Control flow */
    ANVIL_OP_BR,             /* Unconditional branch */
    ANVIL_OP_BR_COND,        /* Conditional branch */
    ANVIL_OP_CALL,
    ANVIL_OP_RET,
    ANVIL_OP_SWITCH,
    
    /* Type conversion */
    ANVIL_OP_TRUNC,          /* Truncate */
    ANVIL_OP_ZEXT,           /* Zero extend */
    ANVIL_OP_SEXT,           /* Sign extend */
    ANVIL_OP_FPTRUNC,
    ANVIL_OP_FPEXT,
    ANVIL_OP_FPTOSI,
    ANVIL_OP_FPTOUI,
    ANVIL_OP_SITOFP,
    ANVIL_OP_UITOFP,
    ANVIL_OP_PTRTOINT,
    ANVIL_OP_INTTOPTR,
    ANVIL_OP_BITCAST,
    
    /* Floating-point arithmetic */
    ANVIL_OP_FADD,           /* FP add */
    ANVIL_OP_FSUB,           /* FP subtract */
    ANVIL_OP_FMUL,           /* FP multiply */
    ANVIL_OP_FDIV,           /* FP divide */
    ANVIL_OP_FNEG,           /* FP negate */
    ANVIL_OP_FABS,           /* FP absolute value */
    ANVIL_OP_FCMP,           /* FP compare */
    
    /* Misc */
    ANVIL_OP_PHI,
    ANVIL_OP_SELECT,
    ANVIL_OP_NOP,
    
    ANVIL_OP_COUNT
} anvil_op_t;

/* Calling conventions */
typedef enum {
    ANVIL_CC_DEFAULT,        /* Default for target */
    ANVIL_CC_CDECL,          /* C calling convention */
    ANVIL_CC_STDCALL,        /* Windows stdcall */
    ANVIL_CC_FASTCALL,       /* Fastcall */
    ANVIL_CC_SYSV,           /* System V AMD64 ABI */
    ANVIL_CC_WIN64,          /* Windows x64 */
    ANVIL_CC_MVS,            /* MVS linkage (mainframe) */
    ANVIL_CC_XPLINK          /* z/OS XPLINK */
} anvil_cc_t;

/* Linkage types */
typedef enum {
    ANVIL_LINK_INTERNAL,     /* Internal/static linkage */
    ANVIL_LINK_EXTERNAL,     /* External linkage */
    ANVIL_LINK_WEAK,         /* Weak linkage */
    ANVIL_LINK_COMMON        /* Common linkage */
} anvil_linkage_t;

/* Architecture properties (read-only) */
typedef struct {
    anvil_arch_t arch;
    const char *name;
    int ptr_size;            /* Pointer size in bytes */
    int addr_bits;           /* Address bits (24, 31, 32, 64) */
    int word_size;           /* Native word size in bytes */
    int num_gpr;             /* Number of general purpose registers */
    int num_fpr;             /* Number of floating point registers */
    anvil_endian_t endian;
    anvil_stack_dir_t stack_dir;
    anvil_fp_format_t fp_format; /* Floating-point format */
    bool has_condition_codes;
    bool has_delay_slots;
} anvil_arch_info_t;

/* Error codes */
typedef enum {
    ANVIL_OK = 0,
    ANVIL_ERR_NOMEM,
    ANVIL_ERR_INVALID_ARG,
    ANVIL_ERR_INVALID_TYPE,
    ANVIL_ERR_INVALID_OP,
    ANVIL_ERR_NO_BACKEND,
    ANVIL_ERR_CODEGEN,
    ANVIL_ERR_IO
} anvil_error_t;

/* ============================================================================
 * Context API
 * ============================================================================ */

/* Create a new anvil context */
anvil_ctx_t *anvil_ctx_create(void);

/* Destroy context and free all resources */
void anvil_ctx_destroy(anvil_ctx_t *ctx);

/* Set target architecture */
anvil_error_t anvil_ctx_set_target(anvil_ctx_t *ctx, anvil_arch_t arch);

/* Set output format */
anvil_error_t anvil_ctx_set_output(anvil_ctx_t *ctx, anvil_output_t output);

/* Set assembly syntax */
anvil_error_t anvil_ctx_set_syntax(anvil_ctx_t *ctx, anvil_syntax_t syntax);

/* Set floating-point format (for architectures that support multiple formats) */
anvil_error_t anvil_ctx_set_fp_format(anvil_ctx_t *ctx, anvil_fp_format_t fp_format);

/* Get current floating-point format */
anvil_fp_format_t anvil_ctx_get_fp_format(anvil_ctx_t *ctx);

/* Get architecture info */
const anvil_arch_info_t *anvil_ctx_get_arch_info(anvil_ctx_t *ctx);

/* Get last error message */
const char *anvil_ctx_get_error(anvil_ctx_t *ctx);

/* ============================================================================
 * Module API
 * ============================================================================ */

/* Create a new module */
anvil_module_t *anvil_module_create(anvil_ctx_t *ctx, const char *name);

/* Destroy a module */
void anvil_module_destroy(anvil_module_t *mod);

/* Add a global variable */
anvil_value_t *anvil_module_add_global(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type, anvil_linkage_t linkage);

/* Add an external declaration */
anvil_value_t *anvil_module_add_extern(anvil_module_t *mod, const char *name,
                                        anvil_type_t *type);

/* Generate code for the module */
anvil_error_t anvil_module_codegen(anvil_module_t *mod, char **output, size_t *len);

/* Write generated code to file */
anvil_error_t anvil_module_write(anvil_module_t *mod, const char *filename);

/* ============================================================================
 * Type API
 * ============================================================================ */

/* Get primitive types */
anvil_type_t *anvil_type_void(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_i8(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_i16(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_i32(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_i64(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_u8(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_u16(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_u32(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_u64(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_f32(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_f64(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_ptr(anvil_ctx_t *ctx, anvil_type_t *pointee);

/* Create struct type */
anvil_type_t *anvil_type_struct(anvil_ctx_t *ctx, const char *name,
                                 anvil_type_t **fields, size_t num_fields);

/* Create array type */
anvil_type_t *anvil_type_array(anvil_ctx_t *ctx, anvil_type_t *elem, size_t count);

/* Create function type */
anvil_type_t *anvil_type_func(anvil_ctx_t *ctx, anvil_type_t *ret,
                               anvil_type_t **params, size_t num_params, bool variadic);

/* Get type size in bytes */
size_t anvil_type_size(anvil_type_t *type);

/* Get type alignment */
size_t anvil_type_align(anvil_type_t *type);

/* ============================================================================
 * Function API
 * ============================================================================ */

/* Create a new function */
anvil_func_t *anvil_func_create(anvil_module_t *mod, const char *name,
                                 anvil_type_t *type, anvil_linkage_t linkage);

/* Declare an external function (no body, for linking) */
anvil_func_t *anvil_func_declare(anvil_module_t *mod, const char *name,
                                  anvil_type_t *type);

/* Get function as a value (for use in calls) */
anvil_value_t *anvil_func_get_value(anvil_func_t *func);

/* Set calling convention */
void anvil_func_set_cc(anvil_func_t *func, anvil_cc_t cc);

/* Get function parameter */
anvil_value_t *anvil_func_get_param(anvil_func_t *func, size_t index);

/* Get entry block */
anvil_block_t *anvil_func_get_entry(anvil_func_t *func);

/* ============================================================================
 * Basic Block API
 * ============================================================================ */

/* Create a new basic block */
anvil_block_t *anvil_block_create(anvil_func_t *func, const char *name);

/* Get block name */
const char *anvil_block_get_name(anvil_block_t *block);

/* ============================================================================
 * IR Builder API
 * ============================================================================ */

/* Set insertion point */
void anvil_set_insert_point(anvil_ctx_t *ctx, anvil_block_t *block);

/* Constants */
anvil_value_t *anvil_const_i8(anvil_ctx_t *ctx, int8_t val);
anvil_value_t *anvil_const_i16(anvil_ctx_t *ctx, int16_t val);
anvil_value_t *anvil_const_i32(anvil_ctx_t *ctx, int32_t val);
anvil_value_t *anvil_const_i64(anvil_ctx_t *ctx, int64_t val);
anvil_value_t *anvil_const_u8(anvil_ctx_t *ctx, uint8_t val);
anvil_value_t *anvil_const_u16(anvil_ctx_t *ctx, uint16_t val);
anvil_value_t *anvil_const_u32(anvil_ctx_t *ctx, uint32_t val);
anvil_value_t *anvil_const_u64(anvil_ctx_t *ctx, uint64_t val);
anvil_value_t *anvil_const_f32(anvil_ctx_t *ctx, float val);
anvil_value_t *anvil_const_f64(anvil_ctx_t *ctx, double val);
anvil_value_t *anvil_const_null(anvil_ctx_t *ctx, anvil_type_t *ptr_type);
anvil_value_t *anvil_const_string(anvil_ctx_t *ctx, const char *str);

/* Arithmetic operations */
anvil_value_t *anvil_build_add(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_sub(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_mul(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_sdiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_udiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_smod(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_umod(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_neg(anvil_ctx_t *ctx, anvil_value_t *val, const char *name);

/* Bitwise operations */
anvil_value_t *anvil_build_and(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_or(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_xor(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_not(anvil_ctx_t *ctx, anvil_value_t *val, const char *name);
anvil_value_t *anvil_build_shl(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *amt, const char *name);
anvil_value_t *anvil_build_shr(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *amt, const char *name);
anvil_value_t *anvil_build_sar(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *amt, const char *name);

/* Comparison operations */
anvil_value_t *anvil_build_cmp_eq(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_ne(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_lt(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_le(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_gt(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_ge(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_ult(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_ule(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_ugt(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_cmp_uge(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);

/* Memory operations */
anvil_value_t *anvil_build_alloca(anvil_ctx_t *ctx, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_load(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr, const char *name);
anvil_value_t *anvil_build_store(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *ptr);
anvil_value_t *anvil_build_gep(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr,
                                anvil_value_t **indices, size_t num_indices, const char *name);
anvil_value_t *anvil_build_struct_gep(anvil_ctx_t *ctx, anvil_type_t *struct_type, 
                                       anvil_value_t *ptr, unsigned field_idx, const char *name);

/* Control flow */
anvil_value_t *anvil_build_br(anvil_ctx_t *ctx, anvil_block_t *dest);
anvil_value_t *anvil_build_br_cond(anvil_ctx_t *ctx, anvil_value_t *cond,
                                    anvil_block_t *then_block, anvil_block_t *else_block);
anvil_value_t *anvil_build_call(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *callee,
                                 anvil_value_t **args, size_t num_args, const char *name);
anvil_value_t *anvil_build_ret(anvil_ctx_t *ctx, anvil_value_t *val);
anvil_value_t *anvil_build_ret_void(anvil_ctx_t *ctx);

/* Type conversions */
anvil_value_t *anvil_build_trunc(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_zext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_sext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_bitcast(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_ptrtoint(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_inttoptr(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_fptrunc(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_fpext(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_fptosi(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_fptoui(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_sitofp(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);
anvil_value_t *anvil_build_uitofp(anvil_ctx_t *ctx, anvil_value_t *val, anvil_type_t *type, const char *name);

/* Floating-point operations */
anvil_value_t *anvil_build_fadd(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_fsub(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_fmul(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_fdiv(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);
anvil_value_t *anvil_build_fneg(anvil_ctx_t *ctx, anvil_value_t *val, const char *name);
anvil_value_t *anvil_build_fabs(anvil_ctx_t *ctx, anvil_value_t *val, const char *name);
anvil_value_t *anvil_build_fcmp(anvil_ctx_t *ctx, anvil_value_t *lhs, anvil_value_t *rhs, const char *name);

/* Misc */
anvil_value_t *anvil_build_phi(anvil_ctx_t *ctx, anvil_type_t *type, const char *name);
void anvil_phi_add_incoming(anvil_value_t *phi, anvil_value_t *val, anvil_block_t *block);
anvil_value_t *anvil_build_select(anvil_ctx_t *ctx, anvil_value_t *cond,
                                   anvil_value_t *then_val, anvil_value_t *else_val, const char *name);

/* ============================================================================
 * Backend Registration API
 * ============================================================================ */

/* Backend interface - for implementing new backends */
typedef struct anvil_backend_ops {
    const char *name;
    anvil_arch_t arch;
    
    /* Initialize backend */
    anvil_error_t (*init)(anvil_backend_t *be, anvil_ctx_t *ctx);
    
    /* Cleanup backend */
    void (*cleanup)(anvil_backend_t *be);
    
    /* Generate code for a module */
    anvil_error_t (*codegen_module)(anvil_backend_t *be, anvil_module_t *mod,
                                     char **output, size_t *len);
    
    /* Generate code for a function */
    anvil_error_t (*codegen_func)(anvil_backend_t *be, anvil_func_t *func,
                                   char **output, size_t *len);
    
    /* Get architecture info */
    const anvil_arch_info_t *(*get_arch_info)(anvil_backend_t *be);
    
    /* Private data */
    void *priv;
} anvil_backend_ops_t;

/* Register a custom backend */
anvil_error_t anvil_register_backend(const anvil_backend_ops_t *ops);

/* Get backend for architecture */
anvil_backend_t *anvil_get_backend(anvil_ctx_t *ctx, anvil_arch_t arch);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_H */
