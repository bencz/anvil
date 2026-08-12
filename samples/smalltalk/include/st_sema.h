#ifndef ANVIL_SMALLTALK_SEMA_H
#define ANVIL_SMALLTALK_SEMA_H

#include "st_ast.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t st_sema_binding_id_t;
typedef uint32_t st_sema_scope_id_t;
typedef uint32_t st_sema_block_id_t;

#define ST_SEMA_INVALID_ID UINT32_MAX

typedef enum {
    ST_SEMA_OK = 0,
    ST_SEMA_ERR_INVALID_ARGUMENT,
    ST_SEMA_ERR_OUT_OF_MEMORY,
    ST_SEMA_ERR_OVERFLOW
} st_sema_status_t;

typedef enum {
    ST_SEMA_BIND_SELF,
    ST_SEMA_BIND_SUPER,
    ST_SEMA_BIND_THIS_CONTEXT,
    ST_SEMA_BIND_METHOD_ARGUMENT,
    ST_SEMA_BIND_BLOCK_ARGUMENT,
    ST_SEMA_BIND_TEMPORARY,
    ST_SEMA_BIND_INSTANCE_VARIABLE,
    ST_SEMA_BIND_CLASS_VARIABLE,
    ST_SEMA_BIND_GLOBAL,
    ST_SEMA_BIND_FORWARD_GLOBAL
} st_sema_binding_kind_t;

enum {
    ST_SEMA_BINDING_READONLY = 1u << 0,
    ST_SEMA_BINDING_ASSIGNED = 1u << 1,
    ST_SEMA_BINDING_CAPTURED = 1u << 2,
    ST_SEMA_BINDING_NEEDS_CELL = 1u << 3,
    ST_SEMA_BINDING_IMPLICIT = 1u << 4,
    ST_SEMA_BINDING_EXTERNAL = 1u << 5
};

typedef struct {
    st_sema_binding_kind_t kind;
    st_ast_string_t name;
    st_ast_string_t type_name;
    const st_ast_node_t *declaration;
    st_sema_scope_id_t scope;
    uint32_t slot;
    uint32_t external_id;
    uint32_t flags;
    bool has_type;
} st_sema_binding_t;

typedef enum {
    ST_SEMA_ACCESS_READ,
    ST_SEMA_ACCESS_WRITE,
    ST_SEMA_ACCESS_SUPER_RECEIVER
} st_sema_access_t;

typedef struct {
    const st_ast_node_t *site;
    st_sema_binding_id_t binding;
    st_sema_scope_id_t scope;
    st_sema_access_t access;
} st_sema_reference_t;

typedef enum {
    ST_SEMA_CAPTURE_VALUE,
    ST_SEMA_CAPTURE_CELL,
    ST_SEMA_CAPTURE_SELF
} st_sema_capture_mode_t;

typedef struct {
    st_sema_binding_id_t binding;
    st_sema_capture_mode_t mode;
} st_sema_capture_t;

typedef struct {
    const st_ast_node_t *node;
    st_sema_scope_id_t scope;
    st_sema_block_id_t parent;
    size_t capture_offset;
    size_t capture_count;
    bool has_nonlocal_return;
} st_sema_block_t;

typedef struct {
    st_sema_scope_id_t parent;
    st_sema_block_id_t block;
    size_t first_binding;
    size_t binding_count;
} st_sema_scope_t;

typedef enum {
    ST_SEMA_RETURN_LOCAL_METHOD,
    ST_SEMA_RETURN_HOME_METHOD
} st_sema_return_kind_t;

typedef struct {
    const st_ast_node_t *expression;
    st_sema_return_kind_t kind;
    st_sema_block_id_t block;
} st_sema_return_t;

typedef enum {
    ST_SEMA_DIAG_MALFORMED_AST,
    ST_SEMA_DIAG_DUPLICATE_DECLARATION,
    ST_SEMA_DIAG_RESERVED_DECLARATION,
    ST_SEMA_DIAG_READONLY_ASSIGNMENT,
    ST_SEMA_DIAG_UNDEFINED_NAME,
    ST_SEMA_DIAG_INVALID_SUPER,
    ST_SEMA_DIAG_RETURN_WITHOUT_HOME
} st_sema_diagnostic_code_t;

typedef struct {
    st_sema_diagnostic_code_t code;
    st_source_span_t span;
    st_source_span_t related_span;
    st_ast_string_t name;
    bool has_related_span;
} st_sema_diagnostic_t;

typedef enum {
    ST_SEMA_EXTERNAL_INSTANCE_VARIABLE,
    ST_SEMA_EXTERNAL_CLASS_VARIABLE,
    ST_SEMA_EXTERNAL_GLOBAL,
    ST_SEMA_EXTERNAL_FORWARD_GLOBAL
} st_sema_external_kind_t;

typedef struct {
    st_ast_string_t name;
    st_sema_external_kind_t kind;
    uint32_t slot;
    uint32_t external_id;
} st_sema_external_t;

typedef void *(*st_sema_allocate_fn)(void *user, size_t size);
typedef void (*st_sema_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_sema_allocate_fn allocate;
    st_sema_deallocate_fn deallocate;
    void *user;
} st_sema_allocator_t;

/* The catalog is read-only. Missing names are always diagnosed; a forward
 * global is accepted only when the caller explicitly supplies an entry of
 * ST_SEMA_EXTERNAL_FORWARD_GLOBAL kind. */
typedef struct {
    const st_sema_external_t *entries;
    size_t count;
    bool has_lexical_super;
    st_sema_allocator_t allocator;
} st_sema_catalog_t;

typedef struct {
    st_sema_status_t status;
    st_sema_binding_t *bindings;
    size_t binding_count;
    st_sema_reference_t *references;
    size_t reference_count;
    st_sema_scope_t *scopes;
    size_t scope_count;
    st_sema_block_t *blocks;
    size_t block_count;
    st_sema_capture_t *captures;
    size_t capture_count;
    st_sema_return_t *returns;
    size_t return_count;
    st_sema_diagnostic_t *diagnostics;
    size_t diagnostic_count;
    bool requires_context;
    bool may_be_nonlocal_return_home;
    void *implementation;
} st_sema_result_t;

void st_sema_result_init(st_sema_result_t *result);
void st_sema_result_destroy(st_sema_result_t *result);

/* `result` must have been initialized and must not contain an earlier analysis.
 * The returned status reports infrastructure failure. Language errors are
 * reported in result->diagnostics and make st_sema_succeeded() false. */
st_sema_status_t st_sema_analyze_method(st_sema_result_t *result,
                                        const st_ast_node_t *method,
                                        const st_sema_catalog_t *catalog);
bool st_sema_succeeded(const st_sema_result_t *result);

const st_sema_reference_t *st_sema_reference_for_node(
    const st_sema_result_t *result, const st_ast_node_t *node);
const st_sema_block_t *st_sema_block_for_node(
    const st_sema_result_t *result, const st_ast_node_t *node);

const char *st_sema_status_string(st_sema_status_t status);
const char *st_sema_diagnostic_string(st_sema_diagnostic_code_t code);

#endif
