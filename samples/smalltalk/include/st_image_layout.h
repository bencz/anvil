#ifndef ANVIL_SMALLTALK_IMAGE_LAYOUT_H
#define ANVIL_SMALLTALK_IMAGE_LAYOUT_H

#include "st_class_graph.h"
#include "st_runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ST_IMAGE_LAYOUT_FIXED_POINTERS = 1,
    ST_IMAGE_LAYOUT_INDEXED_VALUES,
    ST_IMAGE_LAYOUT_INDEXED_UINT8,
    ST_IMAGE_LAYOUT_INDEXED_UINT16,
    ST_IMAGE_LAYOUT_INDEXED_UINT32,
    ST_IMAGE_LAYOUT_BOXED_FLOAT64,
    ST_IMAGE_LAYOUT_CLOSURE,
    ST_IMAGE_LAYOUT_LARGE_INTEGER,
    ST_IMAGE_LAYOUT_CELL
} st_image_layout_recipe_t;

typedef enum {
    ST_IMAGE_LAYOUT_OK = 0,
    ST_IMAGE_LAYOUT_ERR_INVALID_ARGUMENT,
    ST_IMAGE_LAYOUT_ERR_INVALID_GRAPH,
    ST_IMAGE_LAYOUT_ERR_MALFORMED_PRAGMA,
    ST_IMAGE_LAYOUT_ERR_UNKNOWN_RECIPE,
    ST_IMAGE_LAYOUT_ERR_DUPLICATE_RECIPE,
    ST_IMAGE_LAYOUT_ERR_DUPLICATE_DEFAULT,
    ST_IMAGE_LAYOUT_ERR_MISSING_DEFAULT,
    ST_IMAGE_LAYOUT_ERR_DUPLICATE_CLASS_OBJECT_LAYOUT,
    ST_IMAGE_LAYOUT_ERR_MISSING_CLASS_OBJECT_LAYOUT,
    ST_IMAGE_LAYOUT_ERR_DUPLICATE_ABSTRACT,
    ST_IMAGE_LAYOUT_ERR_INCOMPATIBLE_SLOTS,
    ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY,
    ST_IMAGE_LAYOUT_ERR_OVERFLOW
} st_image_layout_status_t;

typedef struct {
    st_image_layout_status_t code;
    st_class_graph_id_t entity_id;
    st_source_span_t span;
    const st_ast_node_t *pragma;
} st_image_layout_diagnostic_t;

typedef struct {
    uint32_t runtime_class_id;
    st_class_graph_id_t graph_entity_id;
    uint32_t superclass_id;
    uint32_t metaclass_id;
    uint32_t default_shape_id;
    uint32_t flags;
    size_t shape_offset;
    size_t shape_count;
} st_image_runtime_class_layout_t;

typedef struct {
    uint32_t runtime_shape_id;
    uint32_t runtime_class_id;
    st_class_graph_id_t graph_entity_id;
    st_image_layout_recipe_t recipe;
    size_t fixed_word_count;
    st_indexed_format_t indexed_format;
    size_t bitmap_offset;
    size_t bitmap_word_count;
    bool is_default;
} st_image_runtime_shape_layout_t;

typedef void *(*st_image_layout_allocate_fn)(void *user, size_t size);
typedef void (*st_image_layout_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_image_layout_allocate_fn allocate;
    st_image_layout_deallocate_fn deallocate;
    void *user;
} st_image_layout_allocator_t;

typedef struct {
    st_image_layout_allocator_t allocator;
} st_image_layout_options_t;

typedef struct {
    st_image_layout_status_t status;
    uint32_t *entity_runtime_class_ids;
    size_t entity_count;
    st_image_runtime_class_layout_t *classes;
    size_t class_count;
    st_image_runtime_shape_layout_t *shapes;
    size_t shape_count;
    uint64_t *pointer_bitmaps;
    size_t pointer_bitmap_word_count;
    st_class_graph_id_t class_object_layout_entity_id;
    st_image_layout_diagnostic_t diagnostic;
    void *implementation;
} st_image_layout_result_t;

void st_image_layout_result_init(st_image_layout_result_t *result);
void st_image_layout_result_destroy(st_image_layout_result_t *result);

/* Compiles class pragmas and inherited source slots into the one authoritative
 * graph-ID -> runtime-ID/layout plan used by lowering and metadata emission.
 * The graph and its AST storage must outlive the result. Failure is
 * transactional except for the precise diagnostic/status. */
st_image_layout_status_t st_image_layout_build(
    st_image_layout_result_t *result,
    const st_class_graph_result_t *graph,
    const st_image_layout_options_t *options);

uint32_t st_image_layout_runtime_class_id(
    const st_image_layout_result_t *result,
    st_class_graph_id_t graph_entity_id);
const st_image_runtime_class_layout_t *st_image_layout_class(
    const st_image_layout_result_t *result, uint32_t runtime_class_id);
const st_image_runtime_shape_layout_t *st_image_layout_shape(
    const st_image_layout_result_t *result, uint32_t runtime_shape_id);

const char *st_image_layout_status_string(st_image_layout_status_t status);
const char *st_image_layout_recipe_string(st_image_layout_recipe_t recipe);

#endif
