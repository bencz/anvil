#ifndef ANVIL_SMALLTALK_CLASS_GRAPH_H
#define ANVIL_SMALLTALK_CLASS_GRAPH_H

#include "st_ast.h"
#include "st_sema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t st_class_graph_id_t;
typedef uint32_t st_class_graph_method_id_t;

#define ST_CLASS_GRAPH_INVALID_ID UINT32_C(0)

typedef enum {
    ST_CLASS_GRAPH_OK = 0,
    ST_CLASS_GRAPH_ERR_INVALID_ARGUMENT,
    ST_CLASS_GRAPH_ERR_OUT_OF_MEMORY,
    ST_CLASS_GRAPH_ERR_OVERFLOW
} st_class_graph_status_t;

typedef enum {
    ST_CLASS_GRAPH_CLASS,
    ST_CLASS_GRAPH_METACLASS,
    ST_CLASS_GRAPH_NAMESPACE
} st_class_graph_entity_kind_t;

typedef enum {
    ST_CLASS_GRAPH_INSTANCE_SLOT,
    ST_CLASS_GRAPH_CLASS_VARIABLE
} st_class_graph_slot_kind_t;

typedef enum {
    ST_CLASS_GRAPH_DIAG_MALFORMED_AST,
    ST_CLASS_GRAPH_DIAG_DUPLICATE_DEFINITION,
    ST_CLASS_GRAPH_DIAG_EXTENSION_TARGET_MISSING,
    ST_CLASS_GRAPH_DIAG_EXTENSION_BEFORE_TARGET,
    ST_CLASS_GRAPH_DIAG_EXTENSION_TARGET_NOT_CLASS,
    ST_CLASS_GRAPH_DIAG_SUPERCLASS_MISSING,
    ST_CLASS_GRAPH_DIAG_SUPERCLASS_NOT_CLASS,
    ST_CLASS_GRAPH_DIAG_INHERITANCE_CYCLE,
    ST_CLASS_GRAPH_DIAG_DUPLICATE_METHOD,
    ST_CLASS_GRAPH_DIAG_DUPLICATE_VARIABLE,
    ST_CLASS_GRAPH_DIAG_SLOT_OVERFLOW
} st_class_graph_diagnostic_code_t;

typedef struct {
    size_t unit_index;
    st_ast_string_t source_name;
    st_source_span_t span;
} st_class_graph_origin_t;

typedef struct {
    st_class_graph_slot_kind_t kind;
    st_ast_string_t name;
    st_ast_string_t type_name;
    const st_ast_node_t *declaration;
    st_class_graph_origin_t origin;
    st_class_graph_id_t declaring_class;
    uint32_t slot;
    bool has_type;
} st_class_graph_slot_t;

typedef struct {
    st_class_graph_method_id_t id;
    const st_ast_node_t *node;
    st_ast_string_t selector;
    st_class_graph_origin_t origin;
    st_class_graph_id_t owner;
    st_class_graph_id_t instance_class;
    st_class_graph_id_t lexical_super;
    /* Opaque catalog-layer key plus logical flat entry count. Use the catalog
     * APIs below; catalog_offset no longer indexes catalog_entries directly. */
    size_t catalog_offset;
    size_t catalog_count;
    bool class_side;
} st_class_graph_method_t;

typedef struct {
    st_class_graph_id_t id;
    st_class_graph_entity_kind_t kind;
    st_ast_string_t name;
    const st_ast_node_t *declaration;
    st_class_graph_origin_t origin;
    st_class_graph_id_t namespace_id;
    st_class_graph_id_t superclass_id;
    st_class_graph_id_t metaclass_id;
    st_class_graph_id_t instance_class_id;
    size_t instance_slot_offset;
    size_t instance_slot_count;
    size_t class_variable_offset;
    size_t class_variable_count;
    /* Opaque catalog-layer key plus logical flat entry count. */
    size_t catalog_offset;
    size_t catalog_count;
    size_t own_method_count;
    bool inheritance_valid;
} st_class_graph_entity_t;

typedef struct {
    st_class_graph_diagnostic_code_t code;
    st_ast_string_t name;
    st_class_graph_origin_t origin;
    st_class_graph_origin_t related_origin;
    bool has_related_origin;
} st_class_graph_diagnostic_t;

typedef void *(*st_class_graph_allocate_fn)(void *user, size_t size);
typedef void (*st_class_graph_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_class_graph_allocate_fn allocate;
    st_class_graph_deallocate_fn deallocate;
    void *user;
} st_class_graph_allocator_t;

typedef struct {
    st_class_graph_allocator_t allocator;
} st_class_graph_options_t;

typedef struct {
    st_class_graph_status_t status;
    st_class_graph_entity_t *entities;
    size_t entity_count;
    st_class_graph_method_t *methods;
    size_t method_count;
    st_class_graph_slot_t *instance_slots;
    size_t instance_slot_count;
    st_class_graph_slot_t *class_variables;
    size_t class_variable_count;
    /* Shared global layer: every non-metaclass global appears exactly once.
     * Per-owner flat compatibility catalogs are cached privately and are not
     * duplicated in this array. */
    st_sema_external_t *catalog_entries;
    size_t catalog_entry_count;
    st_class_graph_diagnostic_t *diagnostics;
    size_t diagnostic_count;
    void *implementation;
} st_class_graph_result_t;

typedef struct {
    st_sema_catalog_t catalog;
    st_class_graph_allocator_t allocator;
    bool initialized;
} st_class_graph_sema_view_t;

typedef struct {
    size_t shared_global_count;
    size_t compatibility_view_count;
    size_t compatibility_entry_count;
    size_t method_index_capacity;
    size_t method_index_probe_count;
} st_class_graph_stats_t;

void st_class_graph_result_init(st_class_graph_result_t *result);
void st_class_graph_result_destroy(st_class_graph_result_t *result);

/* Units are consumed in image-to-application order and must outlive result.
 * Infrastructure failure is transactional: no partially built arrays remain
 * in result. Language errors are diagnostics and leave an inspectable graph. */
st_class_graph_status_t st_class_graph_build(
    st_class_graph_result_t *result,
    const st_ast_unit_t *const *units, size_t unit_count,
    const st_class_graph_options_t *options);

bool st_class_graph_succeeded(const st_class_graph_result_t *result);
const st_class_graph_entity_t *st_class_graph_entity(
    const st_class_graph_result_t *result, st_class_graph_id_t id);
const st_class_graph_method_t *st_class_graph_method(
    const st_class_graph_result_t *result, st_class_graph_method_id_t id);
const st_class_graph_method_t *st_class_graph_method_for_node(
    const st_class_graph_result_t *result, const st_ast_node_t *node);

/* Compatibility API. Fills a borrowed flat semantic catalog with the original
 * ordering and precedence. The first request for an owner materializes one
 * graph-lifetime view; all of that owner's methods share it. Its allocator is
 * deliberately empty so st_sema uses its own allocator for the result. */
bool st_class_graph_sema_catalog_for_method(
    const st_class_graph_result_t *result,
    st_class_graph_method_id_t method_id, st_sema_catalog_t *catalog_out);

/* Preferred sema integration. The scoped view contains only external names
 * which occur as variable uses in this method, while preserving the complete
 * catalog's kind precedence and namespace visibility. `view` must be
 * zero-initialized or destroyed. It must remain alive while sema may access its
 * catalog, then be released explicitly. The graph result and its source units
 * must outlive the view because names and AST nodes remain borrowed. Failure is
 * transactional. */
void st_class_graph_sema_view_init(st_class_graph_sema_view_t *view);
st_class_graph_status_t st_class_graph_sema_view_build_minimal(
    st_class_graph_sema_view_t *view,
    const st_class_graph_result_t *result,
    st_class_graph_method_id_t method_id);
void st_class_graph_sema_view_destroy(st_class_graph_sema_view_t *view);

bool st_class_graph_stats(const st_class_graph_result_t *result,
                          st_class_graph_stats_t *stats_out);

const char *st_class_graph_status_string(st_class_graph_status_t status);
const char *st_class_graph_diagnostic_string(
    st_class_graph_diagnostic_code_t code);

#endif
