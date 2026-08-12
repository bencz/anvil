#include "st_image_layout.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    st_image_layout_allocator_t allocator;
} layout_implementation_t;

typedef struct {
    st_image_layout_recipe_t recipe;
    bool is_default;
    const st_ast_node_t *pragma;
} recipe_request_t;

typedef struct {
    st_image_layout_result_t *result;
    const st_class_graph_result_t *graph;
    st_image_layout_allocator_t allocator;
    size_t shape_capacity;
    size_t bitmap_capacity;
} layout_builder_t;

static st_image_layout_status_t fail(
    layout_builder_t *builder, st_image_layout_status_t status,
    const st_class_graph_entity_t *entity, const st_ast_node_t *pragma);

static void *default_allocate(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void default_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static void release(st_image_layout_allocator_t allocator, void *pointer)
{
    if (pointer != NULL) {
        allocator.deallocate(allocator.user, pointer);
    }
}

static void *allocate_zeroed(st_image_layout_allocator_t allocator,
                             size_t count, size_t element_size)
{
    void *memory;

    if (count == 0u) {
        return NULL;
    }
    if (element_size == 0u || count > SIZE_MAX / element_size) {
        return NULL;
    }

    memory = allocator.allocate(allocator.user, count * element_size);
    if (memory != NULL) {
        memset(memory, 0, count * element_size);
    }
    return memory;
}

static bool string_is(st_ast_string_t string, const char *expected)
{
    size_t length = strlen(expected);
    return string.data != NULL && string.length == length
        && memcmp(string.data, expected, length) == 0;
}

static bool node_is_variable(const st_ast_node_t *node, const char *name)
{
    return node != NULL && node->kind == ST_AST_VARIABLE
        && string_is(node->as.variable.name, name);
}

static bool result_is_empty(const st_image_layout_result_t *result)
{
    return result != NULL && result->status == ST_IMAGE_LAYOUT_OK
        && result->entity_runtime_class_ids == NULL
        && result->entity_count == 0u && result->classes == NULL
        && result->class_count == 0u && result->shapes == NULL
        && result->shape_count == 0u && result->pointer_bitmaps == NULL
        && result->pointer_bitmap_word_count == 0u
        && result->class_object_layout_entity_id == 0u
        && result->implementation == NULL;
}

static st_image_layout_status_t find_class_object_layout(
    layout_builder_t *builder)
{
    for (size_t entity_index = 0u;
         entity_index < builder->graph->entity_count; entity_index++) {
        const st_class_graph_entity_t *entity =
            &builder->graph->entities[entity_index];
        const st_ast_list_t *pragmas;

        if (entity->kind != ST_CLASS_GRAPH_CLASS
                || entity->declaration == NULL
                || entity->declaration->kind != ST_AST_CLASS) {
            continue;
        }

        pragmas = &entity->declaration->as.class_decl.pragmas;
        for (size_t pragma_index = 0u; pragma_index < pragmas->count;
             pragma_index++) {
            const st_ast_node_t *pragma = pragmas->items[pragma_index];
            if (pragma == NULL || pragma->kind != ST_AST_MESSAGE
                    || !string_is(pragma->as.message.selector,
                                  "classObjectLayout:")) {
                continue;
            }
            if (pragma->as.message.arguments.count != 1u
                    || pragma->as.message.arguments.items == NULL
                    || pragma->as.message.arguments.items[0] == NULL
                    || pragma->as.message.arguments.items[0]->kind
                        != ST_AST_TRUE) {
                return fail(builder, ST_IMAGE_LAYOUT_ERR_MALFORMED_PRAGMA,
                            entity, pragma);
            }
            if (builder->result->class_object_layout_entity_id != 0u) {
                return fail(
                    builder,
                    ST_IMAGE_LAYOUT_ERR_DUPLICATE_CLASS_OBJECT_LAYOUT,
                    entity, pragma);
            }
            builder->result->class_object_layout_entity_id = entity->id;
        }
    }
    if (builder->result->class_object_layout_entity_id == 0u) {
        return fail(builder, ST_IMAGE_LAYOUT_ERR_MISSING_CLASS_OBJECT_LAYOUT,
                    NULL, NULL);
    }
    return ST_IMAGE_LAYOUT_OK;
}

static st_image_layout_status_t fail(
    layout_builder_t *builder, st_image_layout_status_t status,
    const st_class_graph_entity_t *entity, const st_ast_node_t *pragma)
{
    builder->result->status = status;
    builder->result->diagnostic.code = status;
    builder->result->diagnostic.entity_id = entity != NULL ? entity->id : 0u;
    builder->result->diagnostic.span = pragma != NULL
        ? pragma->span : entity != NULL ? entity->origin.span
                                       : (st_source_span_t){0};
    builder->result->diagnostic.pragma = pragma;
    return status;
}

static bool reserve_array(layout_builder_t *builder, void **array,
                          size_t *capacity, size_t count, size_t required,
                          size_t element_size)
{
    size_t next;
    void *replacement;

    if (required <= *capacity) {
        return true;
    }

    next = *capacity == 0u ? 16u : *capacity;
    while (next < required) {
        if (next > SIZE_MAX / 2u) {
            return false;
        }
        next *= 2u;
    }

    replacement = allocate_zeroed(builder->allocator, next, element_size);
    if (replacement == NULL) {
        return false;
    }
    if (count != 0u) {
        memcpy(replacement, *array, count * element_size);
    }

    release(builder->allocator, *array);
    *array = replacement;
    *capacity = next;
    return true;
}

static bool recipe_for_node(const st_ast_node_t *node,
                            st_image_layout_recipe_t *recipe_out)
{
    if (node_is_variable(node, "fixedPointers")) {
        *recipe_out = ST_IMAGE_LAYOUT_FIXED_POINTERS;
    } else if (node_is_variable(node, "indexedValues")) {
        *recipe_out = ST_IMAGE_LAYOUT_INDEXED_VALUES;
    } else if (node_is_variable(node, "indexedUInt8")) {
        *recipe_out = ST_IMAGE_LAYOUT_INDEXED_UINT8;
    } else if (node_is_variable(node, "indexedUInt16")) {
        *recipe_out = ST_IMAGE_LAYOUT_INDEXED_UINT16;
    } else if (node_is_variable(node, "indexedUInt32")) {
        *recipe_out = ST_IMAGE_LAYOUT_INDEXED_UINT32;
    } else if (node_is_variable(node, "boxedFloat64")) {
        *recipe_out = ST_IMAGE_LAYOUT_BOXED_FLOAT64;
    } else if (node_is_variable(node, "closure")) {
        *recipe_out = ST_IMAGE_LAYOUT_CLOSURE;
    } else if (node_is_variable(node, "cell")) {
        *recipe_out = ST_IMAGE_LAYOUT_CELL;
    } else if (node_is_variable(node, "largeInteger")) {
        *recipe_out = ST_IMAGE_LAYOUT_LARGE_INTEGER;
    } else {
        return false;
    }

    return true;
}

static st_image_layout_status_t parse_requests(
    layout_builder_t *builder, const st_class_graph_entity_t *entity,
    recipe_request_t **requests_out, size_t *count_out)
{
    const st_ast_list_t *pragmas = &entity->declaration->as.class_decl.pragmas;
    recipe_request_t *requests;
    size_t count = 0u;
    *requests_out = NULL;
    *count_out = 0u;
    requests = allocate_zeroed(builder->allocator, pragmas->count,
                               sizeof(*requests));
    if (pragmas->count != 0u && requests == NULL) {
        return fail(builder, ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY, entity, NULL);
    }

    for (size_t index = 0u; index < pragmas->count; index++) {
        const st_ast_node_t *pragma = pragmas->items[index];
        const st_ast_list_t *arguments;
        bool is_default = false;
        st_image_layout_recipe_t recipe;
        if (pragma == NULL || pragma->kind != ST_AST_MESSAGE) {
            release(builder->allocator, requests);
            return fail(builder, ST_IMAGE_LAYOUT_ERR_MALFORMED_PRAGMA,
                        entity, pragma);
        }
        if (!string_is(pragma->as.message.selector, "shape:")
                && !string_is(pragma->as.message.selector,
                              "shape:default:")) {
            continue;
        }
        arguments = &pragma->as.message.arguments;
        if ((string_is(pragma->as.message.selector, "shape:")
                && arguments->count != 1u)
                || (string_is(pragma->as.message.selector,
                              "shape:default:")
                    && arguments->count != 2u)
                || arguments->items == NULL) {
            release(builder->allocator, requests);
            return fail(builder, ST_IMAGE_LAYOUT_ERR_MALFORMED_PRAGMA,
                        entity, pragma);
        }
        if (!recipe_for_node(arguments->items[0], &recipe)) {
            release(builder->allocator, requests);
            return fail(builder, ST_IMAGE_LAYOUT_ERR_UNKNOWN_RECIPE,
                        entity, pragma);
        }
        if (arguments->count == 2u) {
            if (arguments->items[1] == NULL
                    || (arguments->items[1]->kind != ST_AST_TRUE
                        && arguments->items[1]->kind != ST_AST_FALSE)) {
                release(builder->allocator, requests);
                return fail(builder, ST_IMAGE_LAYOUT_ERR_MALFORMED_PRAGMA,
                            entity, pragma);
            }
            is_default = arguments->items[1]->kind == ST_AST_TRUE;
        }
        for (size_t prior = 0u; prior < count; prior++) {
            if (requests[prior].recipe == recipe) {
                release(builder->allocator, requests);
                return fail(builder, ST_IMAGE_LAYOUT_ERR_DUPLICATE_RECIPE,
                            entity, pragma);
            }
            if (is_default && requests[prior].is_default) {
                release(builder->allocator, requests);
                return fail(builder, ST_IMAGE_LAYOUT_ERR_DUPLICATE_DEFAULT,
                            entity, pragma);
            }
        }
        requests[count++] = (recipe_request_t){ recipe, is_default, pragma };
    }
    if (count == 1u && !requests[0].is_default) {
        requests[0].is_default = true;
    }
    if (count > 1u) {
        bool has_default = false;

        for (size_t index = 0u; index < count; index++) {
            has_default = has_default || requests[index].is_default;
        }
        if (!has_default) {
            const st_ast_node_t *pragma = requests[0].pragma;
            release(builder->allocator, requests);
            return fail(builder, ST_IMAGE_LAYOUT_ERR_MISSING_DEFAULT,
                        entity, pragma);
        }
    }
    *requests_out = requests;
    *count_out = count;
    return ST_IMAGE_LAYOUT_OK;
}

static bool recipe_layout(st_image_layout_recipe_t recipe,
                          size_t pointer_slot_count,
                          size_t *fixed_word_count_out,
                          st_indexed_format_t *indexed_format_out,
                          bool *raw_out)
{
    *raw_out = false;
    *fixed_word_count_out = pointer_slot_count;

    switch (recipe) {
        case ST_IMAGE_LAYOUT_FIXED_POINTERS:
            *indexed_format_out = ST_INDEXED_NONE;
            return true;
        case ST_IMAGE_LAYOUT_INDEXED_VALUES:
            *indexed_format_out = ST_INDEXED_VALUES;
            return true;
        case ST_IMAGE_LAYOUT_INDEXED_UINT8:
            *indexed_format_out = ST_INDEXED_UINT8;
            return true;
        case ST_IMAGE_LAYOUT_INDEXED_UINT16:
            *indexed_format_out = ST_INDEXED_UINT16;
            return true;
        case ST_IMAGE_LAYOUT_INDEXED_UINT32:
            *indexed_format_out = ST_INDEXED_UINT32;
            return true;
        case ST_IMAGE_LAYOUT_BOXED_FLOAT64:
            *fixed_word_count_out = 1u;
            *indexed_format_out = ST_INDEXED_NONE;
            *raw_out = true;
            return true;
        case ST_IMAGE_LAYOUT_CLOSURE:
            *fixed_word_count_out = 4u;
            *indexed_format_out = ST_INDEXED_VALUES;
            *raw_out = true;
            return true;
        case ST_IMAGE_LAYOUT_CELL:
            *fixed_word_count_out = 1u;
            *indexed_format_out = ST_INDEXED_NONE;
            *raw_out = true;
            return true;
        case ST_IMAGE_LAYOUT_LARGE_INTEGER:
            *fixed_word_count_out = 1u;
            *indexed_format_out = ST_INDEXED_UINT32;
            *raw_out = true;
            return true;
    }
    return false;
}

static st_image_layout_status_t class_flags(
    layout_builder_t *builder, const st_class_graph_entity_t *entity,
    uint32_t *flags_out)
{
    const st_ast_list_t *pragmas = &entity->declaration->as.class_decl.pragmas;
    bool seen = false;
    *flags_out = 0u;
    for (size_t index = 0u; index < pragmas->count; index++) {
        const st_ast_node_t *pragma = pragmas->items[index];

        if (pragma == NULL || pragma->kind != ST_AST_MESSAGE
                || !string_is(pragma->as.message.selector, "abstract:")) {
            continue;
        }
        if (seen) {
            return fail(builder, ST_IMAGE_LAYOUT_ERR_DUPLICATE_ABSTRACT,
                        entity, pragma);
        }
        seen = true;
        if (pragma->as.message.arguments.count != 1u
                || pragma->as.message.arguments.items == NULL
                || pragma->as.message.arguments.items[0] == NULL
                || (pragma->as.message.arguments.items[0]->kind != ST_AST_TRUE
                    && pragma->as.message.arguments.items[0]->kind
                        != ST_AST_FALSE)) {
            return fail(builder, ST_IMAGE_LAYOUT_ERR_MALFORMED_PRAGMA,
                        entity, pragma);
        }
        if (pragma->as.message.arguments.items[0]->kind == ST_AST_TRUE) {
            *flags_out = ST_CLASS_ABSTRACT;
        }
    }
    return ST_IMAGE_LAYOUT_OK;
}

static st_image_layout_status_t append_shape(
    layout_builder_t *builder, const st_class_graph_entity_t *entity,
    uint32_t runtime_class_id, recipe_request_t request)
{
    st_image_layout_result_t *result = builder->result;
    st_image_runtime_shape_layout_t shape;
    size_t bitmap_words;
    bool raw;

    memset(&shape, 0, sizeof(shape));
    if (!recipe_layout(request.recipe, entity->instance_slot_count,
                       &shape.fixed_word_count, &shape.indexed_format, &raw)) {
        return fail(builder, ST_IMAGE_LAYOUT_ERR_UNKNOWN_RECIPE,
                    entity, request.pragma);
    }
    if (raw && entity->instance_slot_count != 0u) {
        return fail(builder, ST_IMAGE_LAYOUT_ERR_INCOMPATIBLE_SLOTS,
                    entity, request.pragma);
    }

    bitmap_words = shape.fixed_word_count == 0u
        ? 0u : (shape.fixed_word_count + 63u) / 64u;
    if (result->shape_count >= ST_HEADER_SHAPE_MAX
            || bitmap_words > SIZE_MAX - result->pointer_bitmap_word_count) {
        return fail(builder, ST_IMAGE_LAYOUT_ERR_OVERFLOW,
                    entity, request.pragma);
    }
    if (!reserve_array(builder, (void **)&result->shapes,
                       &builder->shape_capacity, result->shape_count,
                       result->shape_count + 1u, sizeof(*result->shapes))
            || !reserve_array(builder, (void **)&result->pointer_bitmaps,
                       &builder->bitmap_capacity,
                       result->pointer_bitmap_word_count,
                       result->pointer_bitmap_word_count + bitmap_words,
                       sizeof(*result->pointer_bitmaps))) {
        return fail(builder, ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY,
                    entity, request.pragma);
    }

    shape.runtime_shape_id = (uint32_t)result->shape_count + 1u;
    shape.runtime_class_id = runtime_class_id;
    shape.graph_entity_id = entity->id;
    shape.recipe = request.recipe;
    shape.bitmap_offset = result->pointer_bitmap_word_count;
    shape.bitmap_word_count = bitmap_words;
    shape.is_default = request.is_default;
    if (request.recipe == ST_IMAGE_LAYOUT_CELL) {
        result->pointer_bitmaps[shape.bitmap_offset] = UINT64_C(1);
    } else if (!raw) {
        for (size_t word = 0u; word < bitmap_words; word++) {
            result->pointer_bitmaps[shape.bitmap_offset + word] = UINT64_MAX;
        }
        if (bitmap_words != 0u && (shape.fixed_word_count & 63u) != 0u) {
            result->pointer_bitmaps[shape.bitmap_offset + bitmap_words - 1u] =
                (UINT64_C(1) << (shape.fixed_word_count & 63u)) - 1u;
        }
    }
    result->pointer_bitmap_word_count += bitmap_words;
    result->shapes[result->shape_count++] = shape;
    return ST_IMAGE_LAYOUT_OK;
}

static st_image_layout_status_t build_class_shapes(
    layout_builder_t *builder, const st_class_graph_entity_t *entity,
    st_image_runtime_class_layout_t *runtime_class)
{
    recipe_request_t *requests = NULL;
    size_t request_count = 0u;
    st_image_layout_status_t status;

    if (entity->kind == ST_CLASS_GRAPH_METACLASS) {
        const st_class_graph_entity_t *class_object_layout =
            st_class_graph_entity(
                builder->graph,
                builder->result->class_object_layout_entity_id);
        recipe_request_t request = {
            ST_IMAGE_LAYOUT_FIXED_POINTERS, true, NULL
        };
        st_class_graph_entity_t layout_entity;

        if (class_object_layout == NULL
                || class_object_layout->kind != ST_CLASS_GRAPH_CLASS) {
            return fail(builder, ST_IMAGE_LAYOUT_ERR_INVALID_GRAPH,
                        entity, NULL);
        }

        layout_entity = *entity;
        layout_entity.instance_slot_count =
            class_object_layout->instance_slot_count;
        return append_shape(builder, &layout_entity,
                            runtime_class->runtime_class_id, request);
    }

    status = parse_requests(builder, entity, &requests, &request_count);
    if (status != ST_IMAGE_LAYOUT_OK) {
        return status;
    }
    if (request_count == 0u) {
        release(builder->allocator, requests);
        requests = NULL;
    }
    if (request_count == 0u && entity->superclass_id != 0u) {
        uint32_t superclass_id = builder->result->entity_runtime_class_ids[
            entity->superclass_id - 1u];
        const st_image_runtime_class_layout_t *superclass =
            st_image_layout_class(builder->result, superclass_id);
        if (superclass == NULL || superclass->shape_count == 0u) {
            release(builder->allocator, requests);
            return fail(builder, ST_IMAGE_LAYOUT_ERR_INVALID_GRAPH,
                        entity, NULL);
        }
        requests = allocate_zeroed(builder->allocator, superclass->shape_count,
                                   sizeof(*requests));
        if (requests == NULL) {
            return fail(builder, ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY,
                        entity, NULL);
        }

        request_count = superclass->shape_count;
        for (size_t index = 0u; index < request_count; index++) {
            const st_image_runtime_shape_layout_t *shape =
                &builder->result->shapes[superclass->shape_offset + index];
            requests[index].recipe = shape->recipe;
            requests[index].is_default = shape->is_default;
        }
    }
    if (request_count == 0u) {
        requests = allocate_zeroed(builder->allocator, 1u, sizeof(*requests));
        if (requests == NULL) {
            return fail(builder, ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY,
                        entity, NULL);
        }

        requests[0] = (recipe_request_t){
            ST_IMAGE_LAYOUT_FIXED_POINTERS, true, NULL
        };
        request_count = 1u;
    }
    for (size_t index = 0u; index < request_count; index++) {
        status = append_shape(builder, entity, runtime_class->runtime_class_id,
                              requests[index]);
        if (status != ST_IMAGE_LAYOUT_OK) {
            release(builder->allocator, requests);
            return status;
        }
    }
    release(builder->allocator, requests);
    return ST_IMAGE_LAYOUT_OK;
}

void st_image_layout_result_init(st_image_layout_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
}

void st_image_layout_result_destroy(st_image_layout_result_t *result)
{
    layout_implementation_t *implementation;

    if (result == NULL) {
        return;
    }

    implementation = result->implementation;
    if (implementation != NULL) {
        st_image_layout_allocator_t allocator = implementation->allocator;
        release(allocator, result->entity_runtime_class_ids);
        release(allocator, result->classes);
        release(allocator, result->shapes);
        release(allocator, result->pointer_bitmaps);
        release(allocator, implementation);
    }
    st_image_layout_result_init(result);
}

st_image_layout_status_t st_image_layout_build(
    st_image_layout_result_t *result, const st_class_graph_result_t *graph,
    const st_image_layout_options_t *options)
{
    layout_builder_t builder;
    layout_implementation_t *implementation;
    st_image_layout_allocator_t allocator = {
        default_allocate, default_deallocate, NULL
    };
    size_t class_count = 0u;

    if (result == NULL) {
        return ST_IMAGE_LAYOUT_ERR_INVALID_ARGUMENT;
    }
    if (!result_is_empty(result) || graph == NULL
            || !st_class_graph_succeeded(graph) || graph->entities == NULL
            || graph->entity_count == 0u) {
        result->status = ST_IMAGE_LAYOUT_ERR_INVALID_ARGUMENT;
        return result->status;
    }
    if (options != NULL) {
        if ((options->allocator.allocate == NULL)
                != (options->allocator.deallocate == NULL)) {
            result->status = ST_IMAGE_LAYOUT_ERR_INVALID_ARGUMENT;
            return result->status;
        }
        if (options->allocator.allocate != NULL) {
            allocator = options->allocator;
        }
    }

    memset(&builder, 0, sizeof(builder));
    builder.result = result;
    builder.graph = graph;
    builder.allocator = allocator;
    for (size_t index = 0u; index < graph->entity_count; index++) {
        if (graph->entities[index].kind != ST_CLASS_GRAPH_NAMESPACE) {
            class_count++;
        }
    }
    if (class_count == 0u || class_count > ST_HEADER_CLASS_MAX) {
        result->status = ST_IMAGE_LAYOUT_ERR_OVERFLOW;
        return result->status;
    }

    implementation = allocator.allocate(allocator.user,
                                         sizeof(*implementation));
    result->entity_runtime_class_ids = allocate_zeroed(
        allocator, graph->entity_count,
        sizeof(*result->entity_runtime_class_ids));
    result->classes = allocate_zeroed(allocator, class_count,
                                      sizeof(*result->classes));
    if (implementation == NULL || result->entity_runtime_class_ids == NULL
            || result->classes == NULL) {
        release(allocator, implementation);
        release(allocator, result->entity_runtime_class_ids);
        release(allocator, result->classes);
        st_image_layout_result_init(result);
        result->status = ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY;
        return result->status;
    }
    implementation->allocator = allocator;
    result->implementation = implementation;
    result->entity_count = graph->entity_count;
    result->class_count = class_count;

    {
        st_image_layout_status_t status = find_class_object_layout(&builder);
        if (status != ST_IMAGE_LAYOUT_OK) {
            st_image_layout_diagnostic_t diagnostic = result->diagnostic;
            st_image_layout_result_destroy(result);
            result->status = status;
            result->diagnostic = diagnostic;
            return status;
        }
    }

    for (size_t index = 0u, runtime = 0u; index < graph->entity_count; index++) {
        const st_class_graph_entity_t *entity = &graph->entities[index];

        if (entity->kind == ST_CLASS_GRAPH_NAMESPACE) {
            continue;
        }

        result->entity_runtime_class_ids[index] = (uint32_t)++runtime;
        result->classes[runtime - 1u].runtime_class_id = (uint32_t)runtime;
        result->classes[runtime - 1u].graph_entity_id = entity->id;
    }
    for (size_t index = 0u; index < class_count; index++) {
        st_image_runtime_class_layout_t *runtime_class = &result->classes[index];
        const st_class_graph_entity_t *entity = st_class_graph_entity(
            graph, runtime_class->graph_entity_id);
        runtime_class->superclass_id = st_image_layout_runtime_class_id(
            result, entity->superclass_id);

        /* A root class-side object is still a Behavior. Smalltalk closes the
         * finite superclass graph by making the root metaclass inherit from
         * Class; Class then reaches ClassDescription, Behavior and Object.
         * Without this bridge, ordinary class objects cannot inherit new,
         * new:, lookupSelector:, and the rest of the Behavior protocol. */
        if (entity->kind == ST_CLASS_GRAPH_METACLASS
                && entity->superclass_id == ST_CLASS_GRAPH_INVALID_ID) {
            runtime_class->superclass_id = st_image_layout_runtime_class_id(
                result, result->class_object_layout_entity_id);
            if (runtime_class->superclass_id == 0u) {
                st_image_layout_diagnostic_t diagnostic = result->diagnostic;
                st_image_layout_result_destroy(result);
                result->status = ST_IMAGE_LAYOUT_ERR_INVALID_GRAPH;
                result->diagnostic = diagnostic;
                return result->status;
            }
        }

        /*
         * The current class-graph validator models every metaclass as an
         * instance of itself.  Preserve that explicit self-knot in the
         * runtime descriptors.  This is a compatibility limitation of the
         * current graph model, not a general Smalltalk object-model rule.
         */
        runtime_class->metaclass_id = entity->kind == ST_CLASS_GRAPH_METACLASS
            ? runtime_class->runtime_class_id
            : st_image_layout_runtime_class_id(result, entity->metaclass_id);
        runtime_class->flags = entity->kind == ST_CLASS_GRAPH_METACLASS
            ? ST_CLASS_METACLASS : 0u;
        if (entity->kind == ST_CLASS_GRAPH_CLASS) {
            st_image_layout_status_t flag_status = class_flags(
                &builder, entity, &runtime_class->flags);
            if (flag_status != ST_IMAGE_LAYOUT_OK) {
                st_image_layout_diagnostic_t diagnostic = result->diagnostic;
                st_image_layout_result_destroy(result);
                result->status = flag_status;
                result->diagnostic = diagnostic;
                return flag_status;
            }
        }
        runtime_class->shape_offset = result->shape_count;
        st_image_layout_status_t status = build_class_shapes(
            &builder, entity, runtime_class);
        if (status != ST_IMAGE_LAYOUT_OK) {
            st_image_layout_diagnostic_t diagnostic = result->diagnostic;
            st_image_layout_result_destroy(result);
            result->status = status;
            result->diagnostic = diagnostic;
            return status;
        }
        runtime_class->shape_count = result->shape_count
            - runtime_class->shape_offset;
        for (size_t shape_index = 0u;
             shape_index < runtime_class->shape_count; shape_index++) {
            const st_image_runtime_shape_layout_t *shape =
                &result->shapes[runtime_class->shape_offset + shape_index];
            if (shape->is_default) {
                runtime_class->default_shape_id = shape->runtime_shape_id;
            }
        }
        if (runtime_class->default_shape_id == 0u) {
            st_image_layout_diagnostic_t diagnostic;
            fail(&builder, ST_IMAGE_LAYOUT_ERR_MISSING_DEFAULT, entity, NULL);
            diagnostic = result->diagnostic;
            st_image_layout_result_destroy(result);
            result->status = ST_IMAGE_LAYOUT_ERR_MISSING_DEFAULT;
            result->diagnostic = diagnostic;
            return result->status;
        }
    }
    result->status = ST_IMAGE_LAYOUT_OK;
    return ST_IMAGE_LAYOUT_OK;
}

uint32_t st_image_layout_runtime_class_id(
    const st_image_layout_result_t *result,
    st_class_graph_id_t graph_entity_id)
{
    if (result == NULL || result->status != ST_IMAGE_LAYOUT_OK
            || graph_entity_id == 0u
            || (size_t)graph_entity_id > result->entity_count
            || result->entity_runtime_class_ids == NULL) {
        return 0u;
    }
    return result->entity_runtime_class_ids[graph_entity_id - 1u];
}

const st_image_runtime_class_layout_t *st_image_layout_class(
    const st_image_layout_result_t *result, uint32_t runtime_class_id)
{
    if (result == NULL || result->status != ST_IMAGE_LAYOUT_OK
            || runtime_class_id == 0u
            || (size_t)runtime_class_id > result->class_count
            || result->classes == NULL) {
        return NULL;
    }
    return &result->classes[runtime_class_id - 1u];
}

const st_image_runtime_shape_layout_t *st_image_layout_shape(
    const st_image_layout_result_t *result, uint32_t runtime_shape_id)
{
    if (result == NULL || result->status != ST_IMAGE_LAYOUT_OK
            || runtime_shape_id == 0u
            || (size_t)runtime_shape_id > result->shape_count
            || result->shapes == NULL) {
        return NULL;
    }
    return &result->shapes[runtime_shape_id - 1u];
}

const char *st_image_layout_status_string(st_image_layout_status_t status)
{
    switch (status) {
        case ST_IMAGE_LAYOUT_OK:
            return "ok";
        case ST_IMAGE_LAYOUT_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case ST_IMAGE_LAYOUT_ERR_INVALID_GRAPH:
            return "invalid class graph";
        case ST_IMAGE_LAYOUT_ERR_MALFORMED_PRAGMA:
            return "malformed shape pragma";
        case ST_IMAGE_LAYOUT_ERR_UNKNOWN_RECIPE:
            return "unknown shape recipe";
        case ST_IMAGE_LAYOUT_ERR_DUPLICATE_RECIPE:
            return "duplicate shape recipe";
        case ST_IMAGE_LAYOUT_ERR_DUPLICATE_DEFAULT:
            return "duplicate default shape";
        case ST_IMAGE_LAYOUT_ERR_MISSING_DEFAULT:
            return "missing default shape";
        case ST_IMAGE_LAYOUT_ERR_DUPLICATE_CLASS_OBJECT_LAYOUT:
            return "duplicate class-object layout role";
        case ST_IMAGE_LAYOUT_ERR_MISSING_CLASS_OBJECT_LAYOUT:
            return "missing class-object layout role";
        case ST_IMAGE_LAYOUT_ERR_DUPLICATE_ABSTRACT:
            return "duplicate abstract class pragma";
        case ST_IMAGE_LAYOUT_ERR_INCOMPATIBLE_SLOTS:
            return "raw shape has source slots";
        case ST_IMAGE_LAYOUT_ERR_OUT_OF_MEMORY:
            return "out of memory";
        case ST_IMAGE_LAYOUT_ERR_OVERFLOW:
            return "layout size overflow";
    }
    return "unknown image layout status";
}

const char *st_image_layout_recipe_string(st_image_layout_recipe_t recipe)
{
    switch (recipe) {
        case ST_IMAGE_LAYOUT_FIXED_POINTERS:
            return "fixedPointers";
        case ST_IMAGE_LAYOUT_INDEXED_VALUES:
            return "indexedValues";
        case ST_IMAGE_LAYOUT_INDEXED_UINT8:
            return "indexedUInt8";
        case ST_IMAGE_LAYOUT_INDEXED_UINT16:
            return "indexedUInt16";
        case ST_IMAGE_LAYOUT_INDEXED_UINT32:
            return "indexedUInt32";
        case ST_IMAGE_LAYOUT_BOXED_FLOAT64:
            return "boxedFloat64";
        case ST_IMAGE_LAYOUT_CLOSURE:
            return "closure";
        case ST_IMAGE_LAYOUT_CELL:
            return "cell";
        case ST_IMAGE_LAYOUT_LARGE_INTEGER:
            return "largeInteger";
    }
    return "unknown";
}
