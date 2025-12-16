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

/* Memory pool for efficient allocation */
typedef struct anvil_pool {
    void *blocks;
    size_t block_size;
    size_t used;
    struct anvil_pool *next;
} anvil_pool_t;

/* String buffer for code generation */
typedef struct anvil_strbuf {
    char *data;
    size_t len;
    size_t cap;
} anvil_strbuf_t;

/* Value kinds */
typedef enum {
    ANVIL_VAL_CONST_INT,
    ANVIL_VAL_CONST_FLOAT,
    ANVIL_VAL_CONST_NULL,
    ANVIL_VAL_CONST_STRING,
    ANVIL_VAL_GLOBAL,
    ANVIL_VAL_FUNC,
    ANVIL_VAL_PARAM,
    ANVIL_VAL_INSTR,
    ANVIL_VAL_BLOCK
} anvil_val_kind_t;

/* Instruction structure */
typedef struct anvil_instr {
    anvil_op_t op;
    anvil_value_t *result;
    anvil_value_t **operands;
    size_t num_operands;
    anvil_block_t *parent;
    struct anvil_instr *prev;
    struct anvil_instr *next;
    
    /* For PHI nodes */
    anvil_block_t **phi_blocks;
    size_t num_phi_incoming;
    
    /* For branch instructions */
    anvil_block_t *true_block;
    anvil_block_t *false_block;
    
    /* For struct_gep - stores struct type for offset calculation */
    anvil_type_t *aux_type;
} anvil_instr_t;

/* Value structure */
struct anvil_value {
    anvil_val_kind_t kind;
    anvil_type_t *type;
    char *name;
    uint32_t id;
    
    union {
        int64_t i;
        uint64_t u;
        double f;
        const char *str;
        anvil_instr_t *instr;
        anvil_func_t *func;
        struct {
            anvil_linkage_t linkage;
            anvil_value_t *init;
        } global;
        struct {
            size_t index;
            anvil_func_t *func;
        } param;
    } data;
};

/* Type structure */
struct anvil_type {
    anvil_type_kind_t kind;
    size_t size;           /* Size in bytes (target-dependent) */
    size_t align;          /* Alignment in bytes */
    bool is_signed;
    
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
        } struc;
        
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
    anvil_instr_t *first;
    anvil_instr_t *last;
    struct anvil_block *next;
    uint32_t id;
    
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
    
    anvil_value_t **params;
    size_t num_params;
    
    anvil_block_t *entry;
    anvil_block_t *blocks;
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
    
    anvil_backend_t *backend;
    
    /* Current insertion point */
    anvil_block_t *insert_block;
    anvil_instr_t *insert_point;
    
    /* Type cache */
    anvil_type_t *type_void;
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
    
    /* Memory pool */
    anvil_pool_t *pool;
    
    /* Modules */
    anvil_module_t *modules;
    
    /* ID counters */
    uint32_t next_value_id;
    uint32_t next_block_id;
    uint32_t next_func_id;
    
    /* Error handling */
    char error_msg[256];
    anvil_error_t last_error;
    
    /* Optimization */
    struct anvil_pass_manager *pass_manager;
    int opt_level;  /* anvil_opt_level_t */
};

/* ============================================================================
 * Internal utility functions
 * ============================================================================ */

/* Memory management */
void *anvil_alloc(anvil_ctx_t *ctx, size_t size);
void *anvil_realloc(anvil_ctx_t *ctx, void *ptr, size_t old_size, size_t new_size);
char *anvil_strdup(anvil_ctx_t *ctx, const char *str);
void anvil_pool_init(anvil_pool_t *pool, size_t block_size);
void anvil_pool_destroy(anvil_pool_t *pool);

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

/* Instruction creation */
anvil_instr_t *anvil_instr_create(anvil_ctx_t *ctx, anvil_op_t op,
                                   anvil_type_t *type, const char *name);
void anvil_instr_add_operand(anvil_instr_t *instr, anvil_value_t *val);
void anvil_instr_insert(anvil_ctx_t *ctx, anvil_instr_t *instr);

/* Type utilities */
void anvil_type_init_sizes(anvil_ctx_t *ctx);
anvil_type_t *anvil_type_create(anvil_ctx_t *ctx, anvil_type_kind_t kind);

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
