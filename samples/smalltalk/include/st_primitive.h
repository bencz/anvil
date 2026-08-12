#ifndef ANVIL_SMALLTALK_PRIMITIVE_H
#define ANVIL_SMALLTALK_PRIMITIVE_H

#include "st_ast.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ST_PRIMITIVE_OK = 0,
    ST_PRIMITIVE_ERR_INVALID_ARGUMENT,
    ST_PRIMITIVE_ERR_INVALID_NAME,
    ST_PRIMITIVE_ERR_INVALID_IMPLEMENTATION,
    ST_PRIMITIVE_ERR_DUPLICATE,
    ST_PRIMITIVE_ERR_INCOMPATIBLE,
    ST_PRIMITIVE_ERR_OUT_OF_MEMORY,
    ST_PRIMITIVE_ERR_OVERFLOW
} st_primitive_status_t;

typedef enum {
    /* Lowered directly by the AOT compiler.  intrinsic_id selects a concrete,
     * implemented lowering rule; zero never denotes an implementation. */
    ST_PRIMITIVE_INTRINSIC,
    /* Lowered to an ordinary relocation against runtime_symbol. */
    ST_PRIMITIVE_RUNTIME_SYMBOL,
    /* Same uniform C ABI, but the callee may publish cooperative NLR or
     * exception state.  Lowering must therefore create a control scope and
     * route every exit through its unique epilogue. */
    ST_PRIMITIVE_RUNTIME_CONTROL_SYMBOL
} st_primitive_implementation_kind_t;

typedef enum {
    ST_PRIMITIVE_INSTANCE_ONLY,
    ST_PRIMITIVE_CLASS_ONLY,
    ST_PRIMITIVE_INSTANCE_OR_CLASS
} st_primitive_receiver_policy_t;

typedef enum {
    /* The implementation is total for operands satisfying its contract. */
    ST_PRIMITIVE_CANNOT_FAIL,
    /* Failure resumes the Smalltalk expressions following the pragma. */
    ST_PRIMITIVE_FALL_THROUGH
} st_primitive_failure_policy_t;

#define ST_PRIMITIVE_INVALID_INTRINSIC_ID UINT32_C(0)

typedef void *(*st_primitive_allocate_fn)(void *user, size_t size);
typedef void (*st_primitive_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_primitive_allocate_fn allocate;
    st_primitive_deallocate_fn deallocate;
    void *user;
} st_primitive_allocator_t;

/* Registration input. Strings are copied. Intrinsics require a nonzero
 * intrinsic_id and no runtime_symbol; both runtime-symbol kinds require the
 * invalid intrinsic ID and a valid C-linkage runtime_symbol. */
typedef struct {
    const char *name;
    size_t name_length;
    uint32_t method_arity;
    st_primitive_receiver_policy_t receiver_policy;
    st_primitive_failure_policy_t failure_policy;
    st_primitive_implementation_kind_t implementation_kind;
    uint32_t intrinsic_id;
    const char *runtime_symbol;
    size_t runtime_symbol_length;
} st_primitive_spec_t;

typedef struct {
    st_ast_string_t name;
    uint32_t method_arity;
    st_primitive_receiver_policy_t receiver_policy;
    st_primitive_failure_policy_t failure_policy;
    st_primitive_implementation_kind_t implementation_kind;
    uint32_t intrinsic_id;
    st_ast_string_t runtime_symbol;
} st_primitive_t;

typedef struct st_primitive_catalog_entry st_primitive_catalog_entry_t;
typedef struct st_primitive_catalog_slot st_primitive_catalog_slot_t;

typedef struct {
    st_primitive_catalog_entry_t **entries;
    size_t count;
    size_t entry_capacity;
    st_primitive_catalog_slot_t *slots;
    size_t table_capacity;
    st_primitive_allocator_t allocator;
    st_primitive_status_t status;
    bool initialized;
} st_primitive_catalog_t;

/* `catalog` must be zero-initialized or previously destroyed.  Catalog
 * mutation is owned/serialized by the compiler driver.  Registration
 * is transactional: allocation failure or a rejected duplicate leaves the
 * catalog byte-for-byte semantically unchanged and is safe to retry. */
bool st_primitive_catalog_init(st_primitive_catalog_t *catalog,
                               st_primitive_allocator_t allocator);
void st_primitive_catalog_destroy(st_primitive_catalog_t *catalog);
st_primitive_status_t st_primitive_catalog_register(
    st_primitive_catalog_t *catalog, const st_primitive_spec_t *spec,
    const st_primitive_t **primitive_out);
const st_primitive_t *st_primitive_catalog_lookup(
    const st_primitive_catalog_t *catalog, const void *name, size_t length);
const st_primitive_t *st_primitive_catalog_get(
    const st_primitive_catalog_t *catalog, size_t index);
size_t st_primitive_catalog_count(const st_primitive_catalog_t *catalog);

typedef enum {
    ST_PRIMITIVE_DIAG_MALFORMED_AST,
    ST_PRIMITIVE_DIAG_MALFORMED_PRAGMA,
    ST_PRIMITIVE_DIAG_DUPLICATE_PRAGMA,
    ST_PRIMITIVE_DIAG_MISSING_IMPLEMENTATION,
    ST_PRIMITIVE_DIAG_ARITY_MISMATCH,
    ST_PRIMITIVE_DIAG_RECEIVER_MISMATCH,
    ST_PRIMITIVE_DIAG_MISSING_FALLBACK
} st_primitive_diagnostic_code_t;

typedef struct {
    const st_ast_node_t *method;
    const st_ast_node_t *pragma;
    const st_primitive_t *primitive;
    size_t unit_index;
    st_ast_string_t source_name;
} st_primitive_binding_t;

typedef struct {
    st_primitive_diagnostic_code_t code;
    const st_ast_node_t *method;
    const st_ast_node_t *pragma;
    size_t unit_index;
    st_ast_string_t source_name;
    st_ast_string_t requested_name;
    uint32_t expected_arity;
    uint32_t actual_arity;
} st_primitive_diagnostic_t;

typedef struct {
    st_primitive_allocator_t allocator;
} st_primitive_resolve_options_t;

typedef struct {
    st_primitive_status_t status;
    st_primitive_binding_t *bindings;
    size_t binding_count;
    st_primitive_diagnostic_t *diagnostics;
    size_t diagnostic_count;
    st_primitive_allocator_t allocator;
    bool resolved;
} st_primitive_result_t;

void st_primitive_result_init(st_primitive_result_t *result);
void st_primitive_result_destroy(st_primitive_result_t *result);

/* `result` must have been initialized.  Units and catalog must outlive the
 * result because bindings borrow AST nodes
 * and immutable catalog records.  Infrastructure failure preserves any old
 * result.  Language errors return OK with diagnostics and no false binding. */
st_primitive_status_t st_primitive_resolve(
    st_primitive_result_t *result, const st_ast_unit_t *const *units,
    size_t unit_count, const st_primitive_catalog_t *catalog,
    const st_primitive_resolve_options_t *options);
bool st_primitive_result_succeeded(const st_primitive_result_t *result);

const char *st_primitive_status_string(st_primitive_status_t status);
const char *st_primitive_diagnostic_string(
    st_primitive_diagnostic_code_t code);

#endif
