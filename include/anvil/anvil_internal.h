/*
 * ANVIL - Internal structures and definitions
 * This header is not part of the public API
 */

#ifndef ANVIL_INTERNAL_H
#define ANVIL_INTERNAL_H

#include "anvil.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* String buffer for code generation */
typedef struct anvil_strbuf {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} anvil_strbuf_t;

/* Value kinds */
typedef enum {
    ANVIL_VAL_CONST_INT,
    ANVIL_VAL_CONST_FLOAT,
    ANVIL_VAL_CONST_DECIMAL,
    ANVIL_VAL_CONST_NULL,
    ANVIL_VAL_CONST_STRING,
    ANVIL_VAL_CONST_ARRAY,
    ANVIL_VAL_GLOBAL,
    ANVIL_VAL_FUNC,
    ANVIL_VAL_PARAM,
    ANVIL_VAL_INSTR,
    ANVIL_VAL_BLOCK
} anvil_val_kind_t;

/* Instruction structure */
typedef struct anvil_instr {
    anvil_op_t op;
    anvil_fcmp_pred_t fcmp_pred;
    anvil_value_t *result;
    anvil_value_t **operands;
    size_t num_operands;
    size_t operands_capacity;
    anvil_block_t *parent;
    struct anvil_instr *prev;
    struct anvil_instr *next;
    
    /* For PHI nodes */
    anvil_block_t **phi_blocks;
    size_t num_phi_incoming;
    size_t phi_capacity;
    
    /* For branch instructions */
    anvil_block_t *true_block;
    anvil_block_t *false_block;

    /* For switch terminators. The selector is operands[0]; each case value is
     * operands[1 + i], and switch_blocks[i] is its destination. true_block is
     * the default destination. */
    anvil_block_t **switch_blocks;
    size_t num_switch_cases;
    size_t switch_capacity;
    
    /* For struct_gep - stores struct type for offset calculation */
    anvil_type_t *aux_type;
    anvil_ctx_t *owner_ctx;
    anvil_module_t *owner_module;
    struct anvil_instr *ctx_next_owned;
} anvil_instr_t;

/* Value structure */
struct anvil_value {
    anvil_val_kind_t kind;
    anvil_type_t *type;
    anvil_ctx_t *owner_ctx;
    anvil_module_t *owner_module; /* NULL only for context-wide constants. */
    char *name;
    uint32_t id;

    struct anvil_value *ctx_next_owned;
    struct anvil_value *symbol_next;
    
    union {
        int64_t i;
        uint64_t u;
        double f;
        char *decimal;
        const char *str;
        anvil_instr_t *instr;
        anvil_func_t *func;
        struct {
            anvil_linkage_t linkage;
            anvil_value_t *init;
            bool is_declaration;
        } global;
        struct {
            size_t index;
            anvil_func_t *func;
        } param;
        struct {
            anvil_value_t **elements;
            size_t num_elements;
        } array;
    } data;
};

/* Type structure */
struct anvil_type {
    anvil_type_kind_t kind;
    size_t size;           /* Size in bytes (target-dependent) */
    size_t align;          /* Alignment in bytes */
    size_t preferred_align;
    bool is_signed;
    anvil_ctx_t *owner_ctx;

    /* Linked list used by the context to track every type it owns so they
     * can be freed in anvil_ctx_destroy. Previously composite types (ptr,
     * struct, array, func) leaked on every call because nothing tracked
     * them. */
    struct anvil_type *ctx_next;

    union {
        /* Pointer type */
        anvil_type_t *pointee;

        /* Array type */
        struct {
            anvil_type_t *elem;
            size_t count;
        } array;

        /* Struct type */
        struct {
            char *name;
            anvil_type_t **fields;
            size_t *offsets;
            size_t num_fields;
            bool packed;
            bool identified;
            bool complete;
            struct anvil_type *symbol_next;
        } struc;

        /* Decimal type */
        struct {
            anvil_decimal_encoding_t encoding;
            unsigned precision;
            unsigned scale;
        } decimal;

        /* Function type */
        struct {
            anvil_type_t *ret;
            anvil_type_t **params;
            size_t num_params;
            bool variadic;
        } func;
    } data;
};

/* Basic block structure */
struct anvil_block {
    char *name;
    anvil_func_t *parent;
    anvil_module_t *owner_module;
    anvil_instr_t *first;
    anvil_instr_t *last;
    struct anvil_block *next;
    uint32_t id;
    struct anvil_block *ctx_next_owned;
    
    /* For control flow analysis */
    anvil_block_t **preds;
    size_t num_preds;
    anvil_block_t **succs;
    size_t num_succs;
};

/* Function structure */
struct anvil_func {
    char *name;
    anvil_type_t *type;
    anvil_linkage_t linkage;
    anvil_cc_t cc;
    anvil_module_t *parent;
    anvil_ctx_t *owner_ctx;
    struct anvil_func *ctx_next_owned;
    
    anvil_value_t **params;
    size_t num_params;
    
    anvil_block_t *entry;
    anvil_block_t *blocks;
    anvil_block_t *last_block;  /* Tail of singly-linked blocks list, for O(1) append. */
    size_t num_blocks;
    
    struct anvil_func *next;
    uint32_t id;
    
    /* Stack frame info */
    size_t stack_size;
    size_t max_call_args;
    
    /* Declaration only (no body) - for external functions */
    bool is_declaration;
    
    /* Associated value for use in calls */
    anvil_value_t *value;
};

/* Global variable */
typedef struct anvil_global {
    anvil_value_t *value;
    struct anvil_global *next;
} anvil_global_t;

/* Module structure */
struct anvil_module {
    char *name;
    anvil_ctx_t *ctx;
    
    anvil_func_t *funcs;
    size_t num_funcs;
    
    anvil_global_t *globals;
    size_t num_globals;

    anvil_value_t **symbol_buckets;
    size_t symbol_bucket_count;
    anvil_value_t **symbols;
    size_t num_symbols;
    size_t symbol_capacity;
    
    /* String table for constants */
    struct {
        const char **strings;
        size_t count;
        size_t cap;
    } strings;
    
    struct anvil_module *next;
};

/* Backend structure */
struct anvil_backend {
    const anvil_backend_ops_t *ops;
    anvil_ctx_t *ctx;
    anvil_syntax_t syntax;
    void *priv;
};

/* Context structure */
struct anvil_ctx {
    anvil_arch_t arch;
    anvil_output_t output;
    anvil_syntax_t syntax;
    anvil_fp_format_t fp_format;  /* Floating-point format */
    anvil_abi_t abi;              /* OS ABI / platform variant */
    anvil_data_layout_t data_layout;
    bool target_configured;
    bool target_frozen;
    
    /* CPU model and features */
    anvil_cpu_model_t cpu_model;       /* Specific CPU model */
    anvil_cpu_features_t cpu_features; /* Active CPU features (from model + overrides) */
    anvil_cpu_features_t features_enabled;  /* Manually enabled features */
    anvil_cpu_features_t features_disabled; /* Manually disabled features */
    
    anvil_backend_t *backend;
    
    /* Current insertion cursor. New instructions are inserted after it. */
    anvil_block_t *insert_block;
    anvil_instr_t *insert_point;
    
    /* Type cache */
    anvil_type_t *type_void;
    anvil_type_t *type_i1;
    anvil_type_t *type_i8;
    anvil_type_t *type_i16;
    anvil_type_t *type_i32;
    anvil_type_t *type_i64;
    anvil_type_t *type_u8;
    anvil_type_t *type_u16;
    anvil_type_t *type_u32;
    anvil_type_t *type_u64;
    anvil_type_t *type_f32;
    anvil_type_t *type_f64;
    anvil_type_t *type_ptr_i8;   /* Cached i8* — most common composite type. */
    anvil_type_t *type_ptr_void; /* Cached void*. */

    /* Head of the linked list of fully initialized types owned by this
     * context.  Types are linked only after construction succeeds. */
    anvil_type_t *types;
    anvil_type_t **named_struct_buckets;
    size_t named_struct_bucket_count;
    size_t named_struct_count;

    /* Allocation registries are independent from live CFG/list topology. */
    anvil_value_t *owned_values;
    anvil_instr_t *owned_instrs;
    anvil_block_t *owned_blocks;
    anvil_func_t *owned_funcs;
    
    /* Modules */
    anvil_module_t *modules;
    
    /* ID counters */
    uint32_t next_value_id;
    uint32_t next_block_id;
    uint32_t next_func_id;
    
    /* Error handling */
    char error_msg[256];
    anvil_error_t last_error;
    bool alloc_fail_enabled;
    size_t alloc_fail_after;
    
    /* Optimization */
    struct anvil_pass_manager *pass_manager;
    int opt_level;  /* anvil_opt_level_t */
};

/* ============================================================================
 * Internal utility functions
 * ============================================================================ */

/* String buffer */
void anvil_strbuf_init(anvil_strbuf_t *sb);
void anvil_strbuf_destroy(anvil_strbuf_t *sb);
void anvil_strbuf_append(anvil_strbuf_t *sb, const char *str);
void anvil_strbuf_appendf(anvil_strbuf_t *sb, const char *fmt, ...);
void anvil_strbuf_append_char(anvil_strbuf_t *sb, char c);
char *anvil_strbuf_detach(anvil_strbuf_t *sb, size_t *len);

/* Value creation */
anvil_value_t *anvil_value_create(anvil_ctx_t *ctx, anvil_val_kind_t kind,
                                   anvil_type_t *type, const char *name);
void anvil_value_free_all(anvil_ctx_t *ctx);
typedef enum {
    ANVIL_CONST_DAG_INVALID = 0,
    ANVIL_CONST_DAG_VALID = 1,
    ANVIL_CONST_DAG_NOMEM = -1
} anvil_const_dag_status_t;
anvil_const_dag_status_t
anvil_value_check_constant_dag(const anvil_value_t *value, anvil_ctx_t *ctx);
bool anvil_value_is_constant_dag(const anvil_value_t *value,
                                 anvil_ctx_t *ctx);

/* Instruction creation */
anvil_instr_t *anvil_instr_create(anvil_ctx_t *ctx, anvil_op_t op,
                                   anvil_type_t *type, const char *name);
bool anvil_instr_add_operand(anvil_instr_t *instr, anvil_value_t *val);
bool anvil_instr_reserve_operands(anvil_instr_t *instr, size_t total);
bool anvil_instr_add_operands(anvil_instr_t *instr,
                              anvil_value_t *const *values,
                              size_t count);
bool anvil_instr_insert(anvil_ctx_t *ctx, anvil_instr_t *instr);
void anvil_ir_free_all(anvil_ctx_t *ctx);
void anvil_func_free_all(anvil_ctx_t *ctx);

void *anvil_ctx_malloc(anvil_ctx_t *ctx, size_t size);
void *anvil_ctx_calloc(anvil_ctx_t *ctx, size_t count, size_t size);
void *anvil_ctx_realloc(anvil_ctx_t *ctx, void *ptr, size_t size);
char *anvil_ctx_strdup(anvil_ctx_t *ctx, const char *str);
void anvil_test_fail_alloc_after(anvil_ctx_t *ctx, size_t successes);
void anvil_test_disable_alloc_fail(anvil_ctx_t *ctx);

/* Type utilities */
void anvil_type_init_sizes(anvil_ctx_t *ctx);
void anvil_ctx_freeze_target(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_create(anvil_ctx_t *ctx, anvil_type_kind_t kind);
void anvil_type_free(anvil_type_t *type);
bool anvil_types_equal(const anvil_type_t *lhs, const anvil_type_t *rhs);
bool anvil_sem_binary_types(anvil_op_t op, const anvil_type_t *lhs,
                            const anvil_type_t *rhs,
                            const anvil_type_t *result);
bool anvil_sem_unary_types(anvil_op_t op, const anvil_type_t *operand,
                           const anvil_type_t *result);
bool anvil_sem_cmp_types(anvil_op_t op, const anvil_type_t *lhs,
                         const anvil_type_t *rhs,
                         const anvil_type_t *result);
bool anvil_sem_cast_types(anvil_op_t op, const anvil_type_t *source,
                          const anvil_type_t *result);
bool anvil_sem_bool_type(const anvil_type_t *type);
bool anvil_sem_type_is_sized(const anvil_type_t *type);
anvil_type_t *anvil_sem_memory_object_type(const anvil_value_t *value);
anvil_type_t *anvil_sem_callee_func_type(const anvil_value_t *callee);
typedef enum {
    ANVIL_GEP_STEP_SCALE,
    ANVIL_GEP_STEP_FIELD_OFFSET
} anvil_gep_step_kind_t;
typedef struct {
    anvil_gep_step_kind_t kind;
    size_t amount; /* byte scale or fixed byte offset */
    anvil_type_t *result_type;
} anvil_gep_step_t;
/* Analyze one formal GEP index. index_ordinal zero is the pointer step over
 * source elements; later steps descend arrays or structs. */
bool anvil_gep_analyze_step(anvil_type_t **current,
                            const anvil_value_t *index,
                            size_t index_ordinal,
                            anvil_gep_step_t *step);
bool anvil_gep_const_step_offset(const anvil_gep_step_t *step,
                                 const anvil_value_t *index,
                                 int64_t *offset);
bool anvil_gep_accumulate_offset(int64_t *total, int64_t step_offset);
bool anvil_module_symbol_prepare(anvil_module_t *mod, size_t additional);
void anvil_module_symbol_register(anvil_module_t *mod, anvil_value_t *value);
bool anvil_symbol_linkage_compatible(anvil_linkage_t existing,
                                     bool existing_is_declaration,
                                     anvil_linkage_t incoming,
                                     bool incoming_is_declaration,
                                     bool is_function);

/* CPU catalogue (implemented in cpu_table.c) */
void anvil_update_cpu_features(anvil_ctx_t *ctx);
const void *anvil_cpu_table_find(anvil_cpu_model_t model);
const char *anvil_cpu_table_info_name(const void *opaque);
anvil_arch_t anvil_cpu_table_info_arch(const void *opaque);

/* Error handling */
void anvil_set_error(anvil_ctx_t *ctx, anvil_error_t err, const char *fmt, ...);

/* ============================================================================
 * Backend registration
 * ============================================================================ */

/* Built-in backends */
extern const anvil_backend_ops_t anvil_backend_x86;
extern const anvil_backend_ops_t anvil_backend_x86_64;
extern const anvil_backend_ops_t anvil_backend_s370;
extern const anvil_backend_ops_t anvil_backend_s370_xa;
extern const anvil_backend_ops_t anvil_backend_s390;
extern const anvil_backend_ops_t anvil_backend_zarch;
extern const anvil_backend_ops_t anvil_backend_ppc32;
extern const anvil_backend_ops_t anvil_backend_ppc64;
extern const anvil_backend_ops_t anvil_backend_ppc64le;
extern const anvil_backend_ops_t anvil_backend_arm64;

/* Initialize all built-in backends */
void anvil_init_backends(void);

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_INTERNAL_H */
