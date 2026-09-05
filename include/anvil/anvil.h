/*
 * ANVIL - Abstract Intermediate Representation Library
 * 
 * A portable IR library for compiler code generation
 * Supports multiple backends through source IR and optional MachineIR lowering.
 * 
 */

#ifndef ANVIL_H
#define ANVIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* CPU Model System - models and feature flags */
#include "anvil_cpu.h"

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
typedef struct anvil_instr anvil_instr_t;

/* Target data-layout entry. ABI alignment controls object layout; preferred
 * alignment is the target's optimization preference and may be larger. */
typedef struct {
    size_t size;
    size_t abi_align;
    size_t preferred_align;
} anvil_layout_entry_t;

typedef struct {
    anvil_layout_entry_t pointer;
    anvil_layout_entry_t i1, i8, i16, i32, i64;
    anvil_layout_entry_t f32, f64;
    size_t aggregate_abi_align;
    size_t aggregate_preferred_align;
} anvil_data_layout_t;

/* Target architecture */
typedef enum {
    ANVIL_ARCH_NONE = -1,   /* No target has been selected */
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
    ANVIL_OUTPUT_ASM         /* Assembly text output */
} anvil_output_t;

/* Assembly syntax for mainframe */
typedef enum {
    ANVIL_SYNTAX_DEFAULT,    /* Default for architecture */
    ANVIL_SYNTAX_HLASM,      /* IBM HLASM for mainframes */
    ANVIL_SYNTAX_GAS         /* GNU Assembler syntax */
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
    ANVIL_FP_UNSPECIFIED = -1, /* No target-selected floating-point format */
    ANVIL_FP_IEEE754,        /* IEEE 754 (x86, x86-64, PowerPC, z/Architecture) */
    ANVIL_FP_HFP,            /* IBM Hexadecimal Floating Point (S/370, S/390) */
    ANVIL_FP_HFP_IEEE        /* HFP with IEEE 754 support (z/Architecture, some S/390) */
} anvil_fp_format_t;

/* Decimal storage format */
typedef enum {
    ANVIL_DECIMAL_PACKED,    /* Packed decimal: two digits per byte plus sign nibble */
    ANVIL_DECIMAL_ZONED      /* Zoned decimal: one digit per byte */
} anvil_decimal_encoding_t;

/* OS ABI / Platform variant */
typedef enum {
    ANVIL_ABI_DEFAULT,       /* Default for architecture */
    ANVIL_ABI_SYSV,          /* System V ABI (Linux, BSD) */
    ANVIL_ABI_DARWIN,        /* Darwin/macOS (Mach-O, underscore prefix) */
    ANVIL_ABI_WIN64,         /* Windows x64 ABI */
    ANVIL_ABI_MVS            /* IBM MVS/z/OS */
} anvil_abi_t;

/* Data types */
typedef enum {
    ANVIL_TYPE_VOID,
    ANVIL_TYPE_I1,
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
    ANVIL_TYPE_DECIMAL,
    ANVIL_TYPE_PTR,
    ANVIL_TYPE_STRUCT,
    ANVIL_TYPE_ARRAY,
    ANVIL_TYPE_FUNC,
    ANVIL_TYPE_VECTOR
} anvil_type_kind_t;

/* IEEE-754 comparison predicates. Ordered predicates are false on NaN;
 * unordered predicates are true when either operand is NaN. */
typedef enum {
    ANVIL_FCMP_FALSE,
    ANVIL_FCMP_OEQ,
    ANVIL_FCMP_OGT,
    ANVIL_FCMP_OGE,
    ANVIL_FCMP_OLT,
    ANVIL_FCMP_OLE,
    ANVIL_FCMP_ONE,
    ANVIL_FCMP_ORD,
    ANVIL_FCMP_UEQ,
    ANVIL_FCMP_UGT,
    ANVIL_FCMP_UGE,
    ANVIL_FCMP_ULT,
    ANVIL_FCMP_ULE,
    ANVIL_FCMP_UNE,
    ANVIL_FCMP_UNO,
    ANVIL_FCMP_TRUE
} anvil_fcmp_pred_t;

/* IR Operations */
typedef enum {
    /* Arithmetic */
    ANVIL_OP_ADD,
    ANVIL_OP_SUB,
    ANVIL_OP_MUL,
    ANVIL_OP_SDIV,           /* Signed division */
    ANVIL_OP_UDIV,           /* Unsigned division */
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
    ANVIL_OP_GEP,            /* Typed aggregate/pointer address walk */
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
    ANVIL_OP_VA_START,       /* Address of the first unnamed argument */
    ANVIL_OP_ATOMIC_LOAD,
    ANVIL_OP_ATOMIC_STORE,
    ANVIL_OP_ATOMIC_RMW,
    ANVIL_OP_ATOMIC_CMPXCHG,
    ANVIL_OP_ATOMIC_FENCE,
    
    ANVIL_OP_COUNT
} anvil_op_t;

typedef enum {
    ANVIL_ORDER_RELAXED,
    ANVIL_ORDER_ACQUIRE,
    ANVIL_ORDER_RELEASE,
    ANVIL_ORDER_ACQ_REL,
    ANVIL_ORDER_SEQ_CST
} anvil_memory_order_t;

typedef enum {
    ANVIL_ATOMIC_EXCHANGE,
    ANVIL_ATOMIC_ADD,
    ANVIL_ATOMIC_SUB,
    ANVIL_ATOMIC_AND,
    ANVIL_ATOMIC_OR,
    ANVIL_ATOMIC_XOR
} anvil_atomic_rmw_t;

typedef struct {
    anvil_memory_order_t order;
    anvil_memory_order_t failure_order;
    anvil_atomic_rmw_t rmw;
} anvil_atomic_info_t;

/* Atomic storage must be aligned to its size. Scalar integer and pointer
 * objects are supported; I1 and aggregate atomics require frontend expansion.
 * RMW and strong compare-exchange return the previous value. A failed compare
 * does not write the object; callers may compare the result with expected.
 * Consume may be represented conservatively as acquire. */
bool anvil_atomic_is_lock_free(anvil_ctx_t *ctx, anvil_type_t *type);
anvil_value_t *anvil_build_atomic_load(anvil_ctx_t *ctx, anvil_value_t *pointer, anvil_memory_order_t order, const char *name);
bool anvil_build_atomic_store(anvil_ctx_t *ctx, anvil_value_t *value, anvil_value_t *pointer, anvil_memory_order_t order);
anvil_value_t *anvil_build_atomic_rmw(anvil_ctx_t *ctx, anvil_atomic_rmw_t operation, anvil_value_t *pointer, anvil_value_t *value, anvil_memory_order_t order, const char *name);
anvil_value_t *anvil_build_atomic_cmpxchg(anvil_ctx_t *ctx, anvil_value_t *pointer, anvil_value_t *expected, anvil_value_t *desired,
                                       anvil_memory_order_t success, anvil_memory_order_t failure, const char *name);
bool anvil_build_atomic_fence(anvil_ctx_t *ctx, anvil_memory_order_t order);

/* Calling conventions */
typedef enum {
    ANVIL_CC_DEFAULT,        /* Default for target */
    ANVIL_CC_CDECL,          /* C calling convention */
    ANVIL_CC_STDCALL,        /* Windows stdcall */
    ANVIL_CC_FASTCALL,       /* Fastcall */
    ANVIL_CC_SYSV,           /* System V AMD64 ABI */
    ANVIL_CC_WIN64,          /* Windows x64 */
    ANVIL_CC_MVS             /* MVS linkage (mainframe) */
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
    anvil_abi_t abi;         /* OS ABI / platform variant */
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
    ANVIL_ERR_NO_TARGET,
    ANVIL_ERR_NO_BACKEND,
    ANVIL_ERR_CODEGEN,
    ANVIL_ERR_IO
} anvil_error_t;

/* ============================================================================
 * Context API
 * ============================================================================ */

/* Create a new anvil context */
anvil_ctx_t *anvil_ctx_create(void);

/* Atomically create a context configured for the requested target. */
anvil_ctx_t *anvil_ctx_create_for_target(anvil_arch_t arch);

/* Destroy context and free all resources */
void anvil_ctx_destroy(anvil_ctx_t *ctx);

/* Set target architecture */
anvil_error_t anvil_ctx_set_target(anvil_ctx_t *ctx, anvil_arch_t arch);

/* Get current target architecture */
anvil_arch_t anvil_ctx_get_target(anvil_ctx_t *ctx);

/* True only after a target and its backend/layout were initialized atomically. */
bool anvil_ctx_has_target(const anvil_ctx_t *ctx);

/* Set output format */
anvil_error_t anvil_ctx_set_output(anvil_ctx_t *ctx, anvil_output_t output);

/* Set assembly syntax */
anvil_error_t anvil_ctx_set_syntax(anvil_ctx_t *ctx, anvil_syntax_t syntax);

/* Set OS ABI / platform variant */
anvil_error_t anvil_ctx_set_abi(anvil_ctx_t *ctx, anvil_abi_t abi);

/* Get current OS ABI */
anvil_abi_t anvil_ctx_get_abi(anvil_ctx_t *ctx);

/* Set floating-point format (for architectures that support multiple formats) */
anvil_error_t anvil_ctx_set_fp_format(anvil_ctx_t *ctx, anvil_fp_format_t fp_format);

/* Get current floating-point format */
anvil_fp_format_t anvil_ctx_get_fp_format(anvil_ctx_t *ctx);

/* Get architecture info */
const anvil_arch_info_t *anvil_ctx_get_arch_info(anvil_ctx_t *ctx);
const anvil_data_layout_t *anvil_ctx_get_data_layout(const anvil_ctx_t *ctx);

/* Get architecture info without context (for early initialization) */
const anvil_arch_info_t *anvil_arch_get_info(anvil_arch_t arch);

/* Get last error message */
const char *anvil_ctx_get_error(anvil_ctx_t *ctx);
anvil_error_t anvil_ctx_get_last_error(anvil_ctx_t *ctx);
void anvil_ctx_clear_error(anvil_ctx_t *ctx);

/* Set CPU model for target-specific code generation */
anvil_error_t anvil_ctx_set_cpu(anvil_ctx_t *ctx, anvil_cpu_model_t cpu);

/* Get current CPU model */
anvil_cpu_model_t anvil_ctx_get_cpu(anvil_ctx_t *ctx);

/* Get CPU features for current CPU model */
anvil_cpu_features_t anvil_ctx_get_cpu_features(anvil_ctx_t *ctx);

/* Check if a specific feature is available */
bool anvil_ctx_has_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature);

/* Manually enable/disable specific features (overrides CPU model defaults) */
anvil_error_t anvil_ctx_enable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature);
anvil_error_t anvil_ctx_disable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature);

/* Get CPU model name as string */
const char *anvil_cpu_model_name(anvil_cpu_model_t cpu);

/* Get default CPU model for an architecture */
anvil_cpu_model_t anvil_arch_default_cpu(anvil_arch_t arch);

/* Get features for a specific CPU model */
anvil_cpu_features_t anvil_cpu_model_features(anvil_cpu_model_t cpu);

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
anvil_value_t *anvil_module_declare_global(anvil_module_t *mod,
                                            const char *name,
                                            anvil_type_t *type,
                                            anvil_linkage_t linkage);

/* Unified global/function symbol namespace. Lookup is average O(1);
 * enumeration uses stable insertion order for the module lifetime. */
anvil_value_t *anvil_module_lookup_symbol(const anvil_module_t *mod,
                                           const char *name);
size_t anvil_module_symbol_count(const anvil_module_t *mod);
anvil_value_t *anvil_module_symbol_at(const anvil_module_t *mod, size_t index);

/* Generate code for the module */
anvil_error_t anvil_module_codegen(anvil_module_t *mod, char **output, size_t *len);

/* Write generated code to file */
anvil_error_t anvil_module_write(anvil_module_t *mod, const char *filename);

/* Verify source-level IR invariants before optimization/code generation. */
bool anvil_module_verify(const anvil_module_t *mod, char *error, size_t error_len);
bool anvil_func_verify(const anvil_func_t *func, char *error, size_t error_len);

/* ============================================================================
 * Type API
 * ============================================================================ */

/* Get primitive types */
anvil_type_t *anvil_type_void(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_i1(anvil_ctx_t *ctx);
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
anvil_type_t *anvil_type_decimal(anvil_ctx_t *ctx,
                                  anvil_decimal_encoding_t encoding,
                                  unsigned precision,
                                  unsigned scale);
anvil_type_t *anvil_type_decimal_packed(anvil_ctx_t *ctx,
                                         unsigned precision,
                                         unsigned scale);
anvil_type_t *anvil_type_decimal_zoned(anvil_ctx_t *ctx,
                                        unsigned precision,
                                        unsigned scale);
anvil_type_t *anvil_type_ptr(anvil_ctx_t *ctx, anvil_type_t *pointee);

/* Create struct type */
anvil_type_t *anvil_type_struct(anvil_ctx_t *ctx, const char *name,
                                 anvil_type_t **fields, size_t num_fields);
/* Literal structs compare structurally. Identified structs compare nominally
 * and may be obtained opaque first to form recursive pointer graphs. */
anvil_type_t *anvil_type_literal_struct(anvil_ctx_t *ctx,
                                         anvil_type_t **fields,
                                         size_t num_fields, bool packed);
anvil_type_t *anvil_type_named_struct(anvil_ctx_t *ctx, const char *name);
bool anvil_type_struct_set_body(anvil_type_t *type, anvil_type_t **fields,
                                size_t num_fields, bool packed);
bool anvil_type_struct_is_identified(const anvil_type_t *type);
bool anvil_type_struct_is_opaque(const anvil_type_t *type);
bool anvil_type_struct_is_packed(const anvil_type_t *type);
const char *anvil_type_struct_name(const anvil_type_t *type);
size_t anvil_type_struct_field_count(const anvil_type_t *type);
anvil_type_t *anvil_type_struct_field_type(const anvil_type_t *type,
                                            size_t field_idx);
size_t anvil_type_struct_field_offset(const anvil_type_t *type,
                                      size_t field_idx);

/* Create array type */
anvil_type_t *anvil_type_array(anvil_ctx_t *ctx, anvil_type_t *elem, size_t count);
/* Fixed-width FP vectors use lane-wise arithmetic without reassociation or
 * contraction. Storage needs element alignment; preferred alignment is the
 * vector width. Vector calling conventions are not implicit. */
anvil_type_t *anvil_type_vector(anvil_ctx_t *ctx, anvil_type_t *element, size_t lanes);
anvil_type_t *anvil_type_vector_element(anvil_type_t *type);
size_t anvil_type_vector_lanes(anvil_type_t *type);
/* Zero means unsupported; positive values are relative instruction costs. */
unsigned anvil_vector_operation_cost(anvil_ctx_t *ctx, anvil_op_t operation, anvil_type_t *type);

/* Create function type */
anvil_type_t *anvil_type_func_cc(anvil_ctx_t *ctx, anvil_type_t *ret,
                                  anvil_type_t **params, size_t num_params,
                                  bool variadic, anvil_cc_t cc);
/* Convenience constructor using the target/ABI's canonical default calling
 * convention.  DEFAULT and accepted C aliases are resolved when the type is
 * constructed, so function-type equality always includes the effective CC. */
anvil_type_t *anvil_type_func(anvil_ctx_t *ctx, anvil_type_t *ret,
                               anvil_type_t **params, size_t num_params, bool variadic);
anvil_cc_t anvil_type_func_cc_value(const anvil_type_t *type);

/* Get type size in bytes */
size_t anvil_type_size(anvil_type_t *type);
unsigned anvil_type_bit_width(const anvil_type_t *type);

/* Get type alignment */
size_t anvil_type_align(anvil_type_t *type);
size_t anvil_type_preferred_align(anvil_type_t *type);

/* Decimal type metadata */
anvil_decimal_encoding_t anvil_type_decimal_encoding(anvil_type_t *type);
unsigned anvil_type_decimal_precision(anvil_type_t *type);
unsigned anvil_type_decimal_scale(anvil_type_t *type);

/* Check if type is boolean (i1) */
bool anvil_type_is_bool(anvil_type_t *type);
bool anvil_type_is_integer(anvil_type_t *type);
bool anvil_type_is_floating(anvil_type_t *type);
bool anvil_type_is_signed(anvil_type_t *type);
bool anvil_type_is_pointer(anvil_type_t *type);

/* ============================================================================
 * Value API
 * ============================================================================ */

/* Get the type of a value */
anvil_type_t *anvil_value_get_type(anvil_value_t *val);
anvil_module_t *anvil_value_get_module(const anvil_value_t *val);

/* Check if value is a comparison result (boolean) */
bool anvil_value_is_bool(anvil_value_t *val);
bool anvil_value_is_const_int(anvil_value_t *val);
bool anvil_value_is_const_float(anvil_value_t *val);
int64_t anvil_const_int_signed_value(anvil_value_t *val);
uint64_t anvil_const_int_unsigned_value(anvil_value_t *val);
double anvil_const_float_value(anvil_value_t *val);

/* ============================================================================
 * Function API
 * ============================================================================ */

/* Create a new function */
anvil_func_t *anvil_func_create(anvil_module_t *mod, const char *name,
                                 anvil_type_t *type, anvil_linkage_t linkage);

/* Declare an external function (no body, for linking) */
anvil_func_t *anvil_func_declare(anvil_module_t *mod, const char *name,
                                  anvil_type_t *type);
anvil_func_t *anvil_func_declare_linkage(anvil_module_t *mod,
                                          const char *name,
                                          anvil_type_t *type,
                                          anvil_linkage_t linkage);

/* Get function as a value (for use in calls) */
anvil_value_t *anvil_func_get_value(anvil_func_t *func);
/* Permit packing independent FP operations. Results keep their lane-wise
 * arithmetic, but the order of FP exception observations may change.
 * Disabled by default; this does not permit reassociation or contraction. */
void anvil_func_set_fp_vectorization(anvil_func_t *func, bool enabled);

/* Get function parameter */
anvil_value_t *anvil_func_get_param(anvil_func_t *func, size_t index);

/* Get entry block */
anvil_block_t *anvil_func_get_entry(anvil_func_t *func);

/* Conservative call effects. Clearing a bit is a frontend assertion about
 * every execution of the function, including its callees. Declarations and
 * indirect calls default to ALL. Volatile/atomic accesses require OBSERVABLE.
 * MAY_NOT_RETURN includes nonlocal transfers and nontermination. */
typedef enum {
    ANVIL_EFFECT_READ_MEMORY = 1u << 0,
    ANVIL_EFFECT_WRITE_MEMORY = 1u << 1,
    ANVIL_EFFECT_CAPTURE_POINTERS = 1u << 2,
    ANVIL_EFFECT_MAY_TRAP = 1u << 3,
    ANVIL_EFFECT_MAY_UNWIND = 1u << 4,
    ANVIL_EFFECT_MAY_NOT_RETURN = 1u << 5,
    ANVIL_EFFECT_OBSERVABLE = 1u << 6,
    ANVIL_EFFECT_ALL = (1u << 7) - 1,
} anvil_effect_t;

anvil_error_t anvil_func_set_effects(anvil_func_t *func, unsigned effects);
unsigned anvil_func_get_effects(const anvil_func_t *func);

/* ============================================================================
 * Basic Block API
 * ============================================================================ */

/* Create a new basic block */
anvil_block_t *anvil_block_create(anvil_func_t *func, const char *name);

/* Get block name */
const char *anvil_block_get_name(anvil_block_t *block);

/* Check if block has a terminator instruction (ret, br, br_cond, switch) */
bool anvil_block_has_terminator(anvil_block_t *block);

/* ============================================================================
 * IR Builder API
 * ============================================================================ */

/* Set insertion point */
bool anvil_set_insert_point(anvil_ctx_t *ctx, anvil_block_t *block);

/* Get current insertion block */
anvil_block_t *anvil_get_insert_block(anvil_ctx_t *ctx);

/* Constants */
anvil_value_t *anvil_const_i1(anvil_ctx_t *ctx, bool val);
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
anvil_value_t *anvil_const_decimal(anvil_ctx_t *ctx, anvil_type_t *type,
                                    const char *digits);
const char *anvil_const_decimal_digits(anvil_value_t *value);
anvil_value_t *anvil_const_null(anvil_ctx_t *ctx, anvil_type_t *ptr_type);
anvil_value_t *anvil_const_string(anvil_ctx_t *ctx, const char *str);
anvil_value_t *anvil_const_array(anvil_ctx_t *ctx, anvil_type_t *elem_type,
                                  anvil_value_t **elements, size_t num_elements);
anvil_value_t *anvil_const_struct(anvil_ctx_t *ctx, anvil_type_t *struct_type,
                                   anvil_value_t **fields, size_t num_fields);
/* Relocatable constants are owned by the exact module containing `symbol`.
 * A function address has type ptr<func>; a global address has type ptr<T>. */
anvil_value_t *anvil_const_symbol_addr(anvil_value_t *symbol);
/* Constant typed GEP.  Every index must be an integer constant.  The result
 * retains the base symbol's provenance and a checked signed-byte addend. */
anvil_value_t *anvil_const_gep(anvil_value_t *base,
                                anvil_type_t *source_type,
                                anvil_value_t **indices,
                                size_t num_indices);

/* Set global variable initializer */
bool anvil_global_set_initializer(anvil_value_t *global, anvil_value_t *init);

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
/* Zero alignment means the natural alignment of the accessed type. An explicit
 * alignment is a caller guarantee and must be a power of two at least as large
 * as the natural alignment. Volatile accesses remain observable and ordered
 * relative to other volatile accesses; volatile does not imply atomicity. */
typedef struct
{
    size_t alignment;
    bool is_volatile;
} anvil_memory_access_t;

anvil_value_t *anvil_build_load_ex(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr,
                                  const anvil_memory_access_t *access, const char *name);
bool anvil_build_store_ex(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *ptr,
                          const anvil_memory_access_t *access);

anvil_value_t *anvil_build_alloca(anvil_ctx_t *ctx, anvil_type_t *type, const char *name);
/* Cursor-based varargs. VA_START requires a variadic function. VA_ARG advances
 * a cursor stored in an i8** and returns a pointer to the requested object.
 * Targets with a different va_list representation must provide their own ABI
 * implementation; unsupported layouts fail instead of assuming stack-only C. */
anvil_value_t *anvil_build_va_start(anvil_ctx_t *ctx, const char *name);
anvil_value_t *anvil_build_va_copy(anvil_ctx_t *ctx, anvil_value_t *cursor, const char *name);
/* Copy the target's native cursor state into caller-owned storage. Destination
 * must have the native va_list size/alignment; cursor is returned by va_start.
 * Unlike the convenience va_copy temporary, distinct destinations stay
 * independent when this operation executes repeatedly in a loop. */
bool anvil_build_va_copy_into(anvil_ctx_t *ctx, anvil_value_t *destination, anvil_value_t *cursor);
anvil_value_t *anvil_build_va_arg(anvil_ctx_t *ctx, anvil_value_t *cursor_storage, anvil_type_t *type, const char *name);
/* Dynamic-size alloca: stack area sized to `count` * sizeof(type). */
anvil_value_t *anvil_build_alloca_dyn(anvil_ctx_t *ctx, anvil_type_t *type,
                                       anvil_value_t *count, const char *name);
anvil_value_t *anvil_build_load(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr, const char *name);
bool anvil_build_store(anvil_ctx_t *ctx, anvil_value_t *val, anvil_value_t *ptr);
anvil_value_t *anvil_build_gep(anvil_ctx_t *ctx, anvil_type_t *type, anvil_value_t *ptr,
                                anvil_value_t **indices, size_t num_indices, const char *name);
anvil_value_t *anvil_build_struct_gep(anvil_ctx_t *ctx, anvil_type_t *struct_type, 
                                       anvil_value_t *ptr, unsigned field_idx, const char *name);

/* Control flow */
bool anvil_build_br(anvil_ctx_t *ctx, anvil_block_t *dest);
bool anvil_build_br_cond(anvil_ctx_t *ctx, anvil_value_t *cond,
                         anvil_block_t *then_block, anvil_block_t *else_block);
anvil_instr_t *anvil_build_switch(anvil_ctx_t *ctx, anvil_value_t *value,
                                  anvil_block_t *default_block);
bool anvil_switch_add_case(anvil_instr_t *switch_instr,
                           anvil_value_t *case_value,
                           anvil_block_t *dest);
/* Build a direct or indirect typed call.  The signature and effective calling
 * convention are derived solely from callee's func/ptr<func> type.  `result`
 * may be NULL; when supplied it is cleared first and remains NULL for a
 * successful void call. */
bool anvil_build_call_checked(anvil_ctx_t *ctx, anvil_value_t *callee,
                               anvil_value_t **args, size_t num_args,
                               const char *name, anvil_value_t **result);
bool anvil_build_ret(anvil_ctx_t *ctx, anvil_value_t *val);
bool anvil_build_ret_void(anvil_ctx_t *ctx);

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
anvil_value_t *anvil_build_fcmp(anvil_ctx_t *ctx, anvil_fcmp_pred_t predicate,
                                anvil_value_t *lhs, anvil_value_t *rhs,
                                const char *name);

/* Misc */
anvil_value_t *anvil_build_phi(anvil_ctx_t *ctx, anvil_type_t *type, const char *name);
bool anvil_phi_add_incoming(anvil_value_t *phi, anvil_value_t *val, anvil_block_t *block);
anvil_value_t *anvil_build_select(anvil_ctx_t *ctx, anvil_value_t *cond,
                                   anvil_value_t *then_val, anvil_value_t *else_val, const char *name);

/* ============================================================================
 * Backend Registration API
 * ============================================================================ */

/* C-compatible value classification. Indirect parameters require a caller-owned
 * copy; an indirect result adds the first pointer parameter and returns that
 * pointer. This describes the scalar signature, not aggregate IR legalization.
 * Unsupported target/value combinations return ANVIL_ERR_INVALID_TYPE. */
typedef enum {
    ANVIL_ABI_VALUE_DIRECT,
    ANVIL_ABI_VALUE_INTEGER,
    ANVIL_ABI_VALUE_INDIRECT
} anvil_abi_value_kind_t;

typedef struct {
    anvil_abi_value_kind_t kind;
    anvil_type_t *transport_type;
    size_t temporary_alignment;
} anvil_abi_value_plan_t;

anvil_error_t anvil_abi_classify_value(anvil_ctx_t *ctx, anvil_type_t *type, bool is_return, anvil_abi_value_plan_t *plan);

/* Backend interface - for implementing new backends */
typedef struct anvil_backend_ops {
    const char *name;
    anvil_arch_t arch;
    
    /* Initialize backend */
    anvil_error_t (*init)(anvil_backend_t *be, anvil_ctx_t *ctx);
    
    /* Cleanup backend */
    void (*cleanup)(anvil_backend_t *be);
    
    /* Reset backend state (clear cached pointers to IR values) */
    void (*reset)(anvil_backend_t *be);
    
    /* Prepare/lower IR for code generation (optional).
     * This is called before codegen_module to perform architecture-specific
     * analysis, lowering, or transformation of the entire IR. Examples:
     * - Lower unsupported operations to sequences of supported ones
     * - Perform target-specific peephole optimizations on IR
     * - Legalize types (e.g., split 64-bit ops on 32-bit targets)
     * - Insert spill/reload code for register pressure
     * Returns ANVIL_OK on success. If NULL, this step is skipped. */
    anvil_error_t (*prepare_ir)(anvil_backend_t *be, anvil_module_t *mod);
    
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
    anvil_error_t (*classify_abi_value)(anvil_backend_t *be, anvil_type_t *type, bool is_return, anvil_abi_value_plan_t *plan);
    anvil_value_t *(*build_va_arg)(anvil_backend_t *be, anvil_value_t *cursor_storage, anvil_type_t *type, const char *name);
    bool (*atomic_is_lock_free)(anvil_backend_t *be, anvil_type_t *type);
    unsigned (*vector_operation_cost)(anvil_backend_t *be, anvil_op_t operation, anvil_type_t *type);
    anvil_value_t *(*build_va_copy)(anvil_backend_t *be, anvil_value_t *cursor, const char *name);
    bool (*build_va_copy_into)(anvil_backend_t *be, anvil_value_t *destination, anvil_value_t *cursor);
} anvil_backend_ops_t;

/* Register a custom backend */
anvil_error_t anvil_register_backend(const anvil_backend_ops_t *ops);

/* Get backend for architecture */
anvil_backend_t *anvil_get_backend(anvil_ctx_t *ctx, anvil_arch_t arch);

#ifdef __cplusplus
}
#endif

/* Include debug/dump API - must be after type declarations */
#include "anvil_debug.h"

#endif /* ANVIL_H */
