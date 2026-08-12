#include "st_aot_bootstrap.h"
#include "st_heap_primitives.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
#define CHECK(c) do { if (!(c)) {                                             \
    fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c);    \
    failures++;                                                               \
} } while (0)

typedef struct {
    size_t calls;
    size_t fail_on;
    size_t live;
} allocation_t;

static void *tracked_allocate(void *user, size_t size)
{
    allocation_t *allocation = user;
    allocation->calls++;
    if (allocation->fail_on == allocation->calls) return NULL;
    void *pointer = malloc(size);
    if (pointer != NULL) allocation->live++;
    return pointer;
}

static void tracked_deallocate(void *user, void *pointer)
{
    allocation_t *allocation = user;
    if (pointer != NULL) {
        CHECK(allocation->live != 0u);
        allocation->live--;
        free(pointer);
    }
}

enum {
    C_OBJECT = 1, C_OBJECT_META,
    C_CLASS, C_CLASS_META,
    C_META_CLASS, C_META_CLASS_META,
    C_INTEGER, C_INTEGER_META,
    C_SMALL_INTEGER, C_SMALL_INTEGER_META,
    C_ARRAY, C_ARRAY_META,
    C_METHOD_DICTIONARY, C_METHOD_DICTIONARY_META,
    C_STRING, C_STRING_META,
    C_SYMBOL, C_SYMBOL_META,
    C_COMPILED_METHOD, C_COMPILED_METHOD_META,
    C_LIMIT
};

enum {
    S_STRING16 = C_LIMIT,
    S_STRING32,
    S_SYMBOL16,
    S_SYMBOL32,
    S_LIMIT
};

typedef struct {
    StClassDescriptor class_storage[C_LIMIT - 1u];
    StShapeDescriptor shape_storage[S_LIMIT - 1u];
    const StClassDescriptor *classes[C_LIMIT - 1u];
    const StShapeDescriptor *shapes[S_LIMIT - 1u];
    st_runtime_descriptors_t descriptors;
    st_image_entity_metadata_t entities[C_LIMIT - 1u];
    uint32_t entity_runtime_ids[C_LIMIT - 1u];
    st_image_runtime_layout_metadata_t layouts[S_LIMIT - 1u];
    st_image_slot_metadata_t slots[14];
    st_image_selector_metadata_t selectors[2];
    st_image_method_metadata_t methods[2];
    StMethodDescriptor runtime_methods[2];
    StMethodBinding bindings[2];
    StMethodEntry entries[2];
    st_method_slot_t method_slots[2];
    st_image_metadata_descriptor_t metadata;
    st_heap_t heap;
    st_image_runtime_t image;
    st_lookup_context_t lookup;
    st_symbol_intern_context_t symbols;
    st_aot_bootstrap_context_t bootstrap;
} fixture_t;

static st_value_t dummy_method(StFrame *frame)
{
    return frame == NULL ? st_value_nil() : frame->receiver;
}

static size_t minimum_size(size_t fixed_words)
{
    return (sizeof(st_heap_object_t) + fixed_words * sizeof(uint64_t) + 7u)
        & ~(size_t)7u;
}

static void set_shape(fixture_t *fixture, uint32_t shape_id,
                      uint32_t class_id, size_t fixed_words,
                      st_indexed_format_t indexed_format,
                      const uint64_t *bitmap)
{
    fixture->shape_storage[shape_id - 1u] = (StShapeDescriptor) {
        .shape_id = shape_id,
        .class_id = class_id,
        .allocation_alignment = 8u,
        .minimum_allocation_size = minimum_size(fixed_words),
        .fixed_word_count = fixed_words,
        .indexed_format = indexed_format,
        .fixed_pointer_bitmap = fixed_words == 0u ? NULL : bitmap,
        .fixed_pointer_bitmap_word_count = fixed_words == 0u ? 0u : 1u
    };
    fixture->shapes[shape_id - 1u] = &fixture->shape_storage[shape_id - 1u];
}

static void build_fixture(fixture_t *fixture)
{
    static const char *const names[(C_LIMIT - 1u) / 2u] = {
        "Object", "Class", "MetaClass", "Integer", "SmallInteger",
        "Array", "MethodDictionary", "String", "Symbol",
        "CompiledMethod"
    };
    static const uint32_t normal_supers[(C_LIMIT - 1u) / 2u] = {
        0u, C_OBJECT, C_OBJECT, C_OBJECT, C_INTEGER,
        C_OBJECT, C_OBJECT, C_OBJECT, C_STRING, C_OBJECT
    };
    static const uint64_t bitmap9 = UINT64_C(0x1ff);
    static const uint64_t bitmap5 = UINT64_C(0x1f);
    static const uint64_t bitmap3 = UINT64_C(0x7);
    static const char *const class_slot_names[9] = {
        "superClass", "subClasses", "methodDictionary",
        "instanceVariables", "name", "comment", "category",
        "classVariables", "namespace"
    };
    static const char *const meta_slot_names[5] = {
        "superClass", "subClasses", "methodDictionary",
        "instanceVariables", "instanceClass"
    };
    memset(fixture, 0, sizeof(*fixture));
    for (uint32_t class_id = 1u; class_id < C_LIMIT; class_id++) {
        size_t pair = (class_id - 1u) / 2u;
        bool meta = (class_id & 1u) == 0u;
        uint32_t normal_super = normal_supers[pair];
        uint32_t superclass_id = meta
            ? normal_super == 0u ? 0u : normal_super + 1u
            : normal_super;
        uint32_t runtime_superclass_id = meta && superclass_id == 0u
            ? C_CLASS : superclass_id;
        fixture->class_storage[class_id - 1u] = (StClassDescriptor) {
            .class_id = class_id,
            .superclass_id = runtime_superclass_id,
            .metaclass_id = meta ? class_id : class_id + 1u,
            .default_shape_id = class_id,
            .flags = meta ? ST_CLASS_METACLASS : 0u,
            .name = names[pair],
            .name_length = strlen(names[pair])
        };
        fixture->classes[class_id - 1u] =
            &fixture->class_storage[class_id - 1u];
        if (!meta && (class_id == C_METHOD_DICTIONARY
                      || class_id == C_SYMBOL
                      || class_id == C_COMPILED_METHOD))
            fixture->class_storage[class_id - 1u].flags |= ST_CLASS_ABSTRACT;
        fixture->entities[class_id - 1u] = (st_image_entity_metadata_t) {
            .id = class_id,
            .kind = meta ? ST_CLASS_GRAPH_METACLASS : ST_CLASS_GRAPH_CLASS,
            .superclass_id = superclass_id,
            .metaclass_id = meta ? 0u : class_id + 1u,
            .instance_class_id = meta ? class_id - 1u : 0u,
            .name = names[pair]
        };
        fixture->entity_runtime_ids[class_id - 1u] = class_id;
    }
    fixture->entities[C_CLASS - 1u].instance_slot_count = 9u;
    fixture->entities[C_META_CLASS - 1u].instance_slot_offset = 9u;
    fixture->entities[C_META_CLASS - 1u].instance_slot_count = 5u;
    for (size_t index = 0u; index < 9u; index++)
        fixture->slots[index] = (st_image_slot_metadata_t) {
            .declaring_class = C_CLASS,
            .slot = (uint32_t)index,
            .kind = ST_CLASS_GRAPH_INSTANCE_SLOT,
            .name = class_slot_names[index]
        };
    for (size_t index = 0u; index < 5u; index++)
        fixture->slots[9u + index] = (st_image_slot_metadata_t) {
            .declaring_class = C_META_CLASS,
            .slot = (uint32_t)index,
            .kind = ST_CLASS_GRAPH_INSTANCE_SLOT,
            .name = meta_slot_names[index]
        };

    for (uint32_t class_id = 1u; class_id < C_LIMIT; class_id++) {
        bool meta = (class_id & 1u) == 0u;
        size_t fixed = meta ? 9u : 0u;
        st_indexed_format_t format = ST_INDEXED_NONE;
        const uint64_t *bitmap = &bitmap9;
        if (!meta && class_id == C_CLASS) fixed = 9u;
        if (!meta && class_id == C_META_CLASS) {
            fixed = 5u;
            bitmap = &bitmap5;
        }
        if (!meta && (class_id == C_ARRAY
                      || class_id == C_METHOD_DICTIONARY))
            format = ST_INDEXED_VALUES;
        if (!meta && (class_id == C_STRING || class_id == C_SYMBOL))
            format = ST_INDEXED_UINT8;
        if (!meta && class_id == C_COMPILED_METHOD) {
            fixed = 3u;
            bitmap = &bitmap3;
        }
        set_shape(fixture, class_id, class_id, fixed, format, bitmap);
    }
    set_shape(fixture, S_STRING16, C_STRING, 0u, ST_INDEXED_UINT16, NULL);
    set_shape(fixture, S_STRING32, C_STRING, 0u, ST_INDEXED_UINT32, NULL);
    set_shape(fixture, S_SYMBOL16, C_SYMBOL, 0u, ST_INDEXED_UINT16, NULL);
    set_shape(fixture, S_SYMBOL32, C_SYMBOL, 0u, ST_INDEXED_UINT32, NULL);
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->classes, C_LIMIT - 1u, fixture->shapes, S_LIMIT - 1u
    };

    static const char *const method_names[2] = { "yourself", "identity" };
    for (size_t index = 0u; index < 2u; index++) {
        fixture->runtime_methods[index] = (StMethodDescriptor) {
            .abi_version = ST_METHOD_ABI_VERSION,
            .selector_id = (uint32_t)index + 1u,
            .owner_class_id = C_OBJECT
        };
        fixture->bindings[index] = (StMethodBinding) {
            &fixture->runtime_methods[index], dummy_method, 1u
        };
        CHECK(st_method_entry_init(
            &fixture->entries[index], &fixture->bindings[index]));
        fixture->method_slots[index] = (st_method_slot_t) {
            (uint32_t)index + 1u, &fixture->entries[index]
        };
        fixture->selectors[index] = (st_image_selector_metadata_t) {
            .id = (uint32_t)index + 1u,
            .kind = 1u,
            .hash = (uint64_t)index + 1u,
            .spelling = method_names[index]
        };
    }
    fixture->class_storage[C_OBJECT - 1u].method_slots =
        fixture->method_slots;
    fixture->class_storage[C_OBJECT - 1u].method_slot_count = 2u;

    /* The metadata traversal is deliberately grouped/non-ID-ordered. The
     * descriptor pointer remains canonical through method ID - 1. */
    fixture->methods[0] = (st_image_method_metadata_t) {
        .id = 2u, .owner = C_OBJECT, .instance_class = C_OBJECT,
        .selector_id = 2u, .selector = "identity",
        .runtime_descriptor = &fixture->runtime_methods[1]
    };
    fixture->methods[1] = (st_image_method_metadata_t) {
        .id = 1u, .owner = C_OBJECT, .instance_class = C_OBJECT,
        .selector_id = 1u, .selector = "yourself",
        .runtime_descriptor = &fixture->runtime_methods[0]
    };
    for (uint32_t shape_id = 1u; shape_id < S_LIMIT; shape_id++) {
        const StShapeDescriptor *shape = fixture->shapes[shape_id - 1u];
        uint32_t recipe = ST_IMAGE_LAYOUT_FIXED_POINTERS;
        if (shape->indexed_format == ST_INDEXED_VALUES)
            recipe = ST_IMAGE_LAYOUT_INDEXED_VALUES;
        else if (shape->indexed_format == ST_INDEXED_UINT8)
            recipe = ST_IMAGE_LAYOUT_INDEXED_UINT8;
        else if (shape->indexed_format == ST_INDEXED_UINT16)
            recipe = ST_IMAGE_LAYOUT_INDEXED_UINT16;
        else if (shape->indexed_format == ST_INDEXED_UINT32)
            recipe = ST_IMAGE_LAYOUT_INDEXED_UINT32;
        fixture->layouts[shape_id - 1u] =
            (st_image_runtime_layout_metadata_t) {
                .graph_entity_id = shape->class_id,
                .runtime_class_id = shape->class_id,
                .runtime_shape_id = shape_id,
                .recipe = recipe,
                .flags = shape_id < C_LIMIT
                    ? ST_IMAGE_RUNTIME_LAYOUT_DEFAULT : 0u
            };
    }
    uint16_t endian_probe = 1u;
    fixture->metadata = (st_image_metadata_descriptor_t) {
        .magic = ST_IMAGE_METADATA_MAGIC,
        .abi_version = ST_IMAGE_METADATA_ABI_VERSION,
        .flags = ST_IMAGE_METADATA_FLAG_TYPED_RELOCATIONS
            | ST_IMAGE_METADATA_FLAG_METHOD_CODE
            | ST_IMAGE_METADATA_FLAG_RUNTIME_METHODS
            | ST_IMAGE_METADATA_FLAG_RUNTIME_DESCRIPTORS,
        .pointer_size = sizeof(void *),
        .endian = *(uint8_t *)&endian_probe != 0u
            ? ANVIL_ENDIAN_LITTLE : ANVIL_ENDIAN_BIG,
        .entity_count = C_LIMIT - 1u,
        .class_count = (C_LIMIT - 1u) / 2u,
        .metaclass_count = (C_LIMIT - 1u) / 2u,
        .method_count = 2u,
        .selector_count = 2u,
        .instance_slot_count = 14u,
        .runtime_class_count = C_LIMIT - 1u,
        .runtime_shape_count = S_LIMIT - 1u,
        .runtime_layout_count = S_LIMIT - 1u,
        .entities = fixture->entities,
        .methods = fixture->methods,
        .selectors = fixture->selectors,
        .instance_slots = fixture->slots,
        .strings = "x",
        .runtime_methods = fixture->runtime_methods,
        .entity_runtime_class_ids = fixture->entity_runtime_ids,
        .runtime_layouts = fixture->layouts,
        .runtime_descriptors = &fixture->descriptors
    };
}

static bool fixture_init(fixture_t *fixture)
{
    build_fixture(fixture);
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors)
          == ST_RUNTIME_OK);
    if (st_heap_init(&fixture->heap, &fixture->descriptors,
                     (st_runtime_allocator_t){0}) != ST_HEAP_OK)
        return false;
    if (st_image_runtime_init(
            &fixture->image, &(st_image_runtime_options_t) {
                .descriptors = &fixture->descriptors,
                .borrowed_heap = &fixture->heap
            }) != ST_IMAGE_RUNTIME_OK)
        return false;
    if (st_lookup_context_init(&fixture->lookup, &fixture->descriptors,
                               (st_lookup_allocator_t){0}) != ST_LOOKUP_FOUND)
        return false;
    if (st_symbol_intern_context_init(
            &fixture->symbols, &(st_symbol_intern_options_t) {
                .image = &fixture->image,
                .string_class_id = C_STRING,
                .string_uint8_shape_id = C_STRING,
                .string_uint16_shape_id = S_STRING16,
                .string_uint32_shape_id = S_STRING32,
                .symbol_class_id = C_SYMBOL,
                .symbol_uint8_shape_id = C_SYMBOL,
                .symbol_uint16_shape_id = S_SYMBOL16,
                .symbol_uint32_shape_id = S_SYMBOL32
            }) != ST_SYMBOL_INTERN_OK)
        return false;
    return true;
}

static st_aot_bootstrap_options_t bootstrap_options(fixture_t *fixture)
{
    return (st_aot_bootstrap_options_t) {
        .metadata = &fixture->metadata,
        .image = &fixture->image,
        .symbols = &fixture->symbols,
        .lookup = &fixture->lookup,
        .object_entity_id = C_OBJECT,
        .class_object_layout_entity_id = C_CLASS,
        .metaclass_entity_id = C_META_CLASS,
        .integer_entity_id = C_INTEGER,
        .small_integer_entity_id = C_SMALL_INTEGER,
        .array_class_id = C_ARRAY,
        .array_shape_id = C_ARRAY,
        .method_dictionary_class_id = C_METHOD_DICTIONARY,
        .method_dictionary_shape_id = C_METHOD_DICTIONARY,
        .symbol_class_id = C_SYMBOL,
        .compiled_method_class_id = C_COMPILED_METHOD,
        .compiled_method_shape_id = C_COMPILED_METHOD
    };
}

static void fixture_destroy(fixture_t *fixture)
{
    st_aot_bootstrap_context_destroy(&fixture->bootstrap);
    st_symbol_intern_context_destroy(&fixture->symbols);
    st_lookup_context_destroy(&fixture->lookup);
    st_image_runtime_destroy(&fixture->image);
    st_heap_destroy(&fixture->heap);
}

static void test_real_managed_bootstrap_and_gc(void)
{
    fixture_t fixture;
    CHECK(fixture_init(&fixture));
    st_aot_bootstrap_options_t options = bootstrap_options(&fixture);
    st_aot_bootstrap_status_t bootstrap_status =
        st_aot_bootstrap_context_init(&fixture.bootstrap, &options);
    if (bootstrap_status != ST_AOT_BOOTSTRAP_OK)
        fprintf(stderr, "bootstrap status: %s objects=%zu\n",
                st_aot_bootstrap_status_string(bootstrap_status),
                st_heap_object_count(&fixture.heap));
    CHECK(bootstrap_status == ST_AOT_BOOTSTRAP_OK);
    if (bootstrap_status != ST_AOT_BOOTSTRAP_OK) {
        fixture_destroy(&fixture);
        return;
    }
    size_t class_count = 0u, selector_count = 0u;
    const st_value_t *classes = st_aot_bootstrap_class_objects(
        &fixture.bootstrap, &class_count);
    const st_value_t *selectors = st_aot_bootstrap_selector_symbols(
        &fixture.bootstrap, &selector_count);
    CHECK(classes != NULL && class_count == C_LIMIT - 1u
          && selectors != NULL && selector_count == 2u);
    st_object_view_t object_class;
    st_object_view_t object_meta;
    st_object_view_t small_integer;
    CHECK(st_heap_object_view(&fixture.heap, classes[C_OBJECT - 1u],
                              &object_class) == ST_HEAP_OK
          && object_class.class_descriptor->class_id == C_OBJECT_META
          && object_class.shape_descriptor->fixed_word_count == 9u);
    CHECK(st_heap_object_view(&fixture.heap, classes[C_OBJECT_META - 1u],
                              &object_meta) == ST_HEAP_OK
          && object_meta.class_descriptor->class_id == C_OBJECT_META
          && ((st_value_t *)object_meta.fixed_words)[4] == classes[0]);
    CHECK(st_heap_object_view(&fixture.heap, classes[C_SMALL_INTEGER - 1u],
                              &small_integer) == ST_HEAP_OK
          && ((st_value_t *)small_integer.fixed_words)[0]
             == classes[C_INTEGER - 1u]);
    st_value_t method_dictionary =
        ((st_value_t *)object_class.fixed_words)[2];
    st_object_view_t dictionary;
    CHECK(st_heap_object_view(&fixture.heap, method_dictionary, &dictionary)
          == ST_HEAP_OK
          && dictionary.class_descriptor->class_id == C_METHOD_DICTIONARY
          && dictionary.indexed_length == 4u
          && ((st_value_t *)dictionary.indexed_elements)[0] == selectors[0]);
    st_value_t mirror = ST_VALUE_INVALID;
    CHECK(st_reflection_lookup_selector(
              &fixture.bootstrap.reflection, classes[0], selectors[0],
              &mirror) == ST_REFLECTION_PRIMITIVE_OK
          && mirror == ((st_value_t *)dictionary.indexed_elements)[1]);
    st_heap_indexed_access_t indexed_access[S_LIMIT - 1u];
    for (size_t index = 0u; index < S_LIMIT - 1u; index++) {
        st_indexed_format_t format = fixture.shapes[index]->indexed_format;
        indexed_access[index] = format == ST_INDEXED_NONE
            ? ST_HEAP_INDEXED_ACCESS_NONE
            : format == ST_INDEXED_VALUES
                ? ST_HEAP_INDEXED_ACCESS_VALUES
                : ST_HEAP_INDEXED_ACCESS_UNSIGNED_INTEGER;
    }
    st_heap_primitive_context_t heap_primitives = {0};
    CHECK(st_heap_primitive_context_init(
              &heap_primitives, &(st_heap_primitive_options_t) {
                  .heap = &fixture.heap,
                  .immediate_classes = {
                      C_SMALL_INTEGER, C_OBJECT, C_OBJECT, C_OBJECT, C_OBJECT
                  },
                  .class_objects = classes,
                  .class_object_count = class_count,
                  .indexed_access = indexed_access,
                  .indexed_access_count = S_LIMIT - 1u
              }) == ST_HEAP_PRIMITIVE_OK);
    st_value_t forbidden = st_value_nil();
    CHECK(st_heap_primitive_execute(
              &heap_primitives, ST_INTRINSIC_BEHAVIOR_NEW,
              classes[C_SYMBOL - 1u], NULL, 0u, &forbidden)
          == ST_HEAP_PRIMITIVE_ERR_ABSTRACT_CLASS
          && forbidden == ST_VALUE_INVALID);
    CHECK(st_heap_primitive_execute(
              &heap_primitives, ST_INTRINSIC_BEHAVIOR_NEW,
              classes[C_METHOD_DICTIONARY - 1u], NULL, 0u, &forbidden)
          == ST_HEAP_PRIMITIVE_ERR_ABSTRACT_CLASS
          && forbidden == ST_VALUE_INVALID);
    st_heap_primitive_context_destroy(&heap_primitives);
    st_heap_collection_stats_t stats;
    CHECK(st_image_runtime_collect(&fixture.image, NULL, &stats)
          == ST_IMAGE_RUNTIME_OK
          && st_heap_contains(&fixture.heap, classes[0])
          && st_heap_contains(&fixture.heap, method_dictionary)
          && st_heap_contains(&fixture.heap, mirror));
    fixture_destroy(&fixture);
}

static void test_rejects_abi_and_hierarchy_mismatch(void)
{
    fixture_t fixture;
    CHECK(fixture_init(&fixture));
    st_aot_bootstrap_options_t options = bootstrap_options(&fixture);
    fixture.metadata.abi_version--;
    CHECK(st_aot_bootstrap_context_init(&fixture.bootstrap, &options)
          == ST_AOT_BOOTSTRAP_ERR_INVALID_METADATA
          && !fixture.bootstrap.initialized);
    fixture.metadata.abi_version = ST_IMAGE_METADATA_ABI_VERSION;
    fixture.entities[C_SMALL_INTEGER - 1u].superclass_id = C_OBJECT;
    CHECK(st_aot_bootstrap_context_init(&fixture.bootstrap, &options)
          == ST_AOT_BOOTSTRAP_ERR_INVALID_DESCRIPTOR
          && !fixture.bootstrap.initialized);
    fixture_destroy(&fixture);
}

static void test_oom_has_zero_external_publication(void)
{
    for (size_t fail_on = 1u; fail_on <= 10u; fail_on++) {
        fixture_t fixture;
        allocation_t allocation = { .fail_on = fail_on };
        CHECK(fixture_init(&fixture));
        st_aot_bootstrap_options_t options = bootstrap_options(&fixture);
        options.allocator = (st_primitive_allocator_t) {
            tracked_allocate, tracked_deallocate, &allocation
        };
        size_t symbols_before = st_symbol_intern_count(&fixture.symbols);
        CHECK(st_aot_bootstrap_context_init(&fixture.bootstrap, &options)
              == ST_AOT_BOOTSTRAP_ERR_OUT_OF_MEMORY
              && !fixture.bootstrap.initialized
              && fixture.bootstrap.state == NULL
              && st_symbol_intern_count(&fixture.symbols) == symbols_before
              && allocation.live == 0u);
        st_heap_collection_stats_t stats;
        CHECK(st_image_runtime_collect(&fixture.image, NULL, &stats)
              == ST_IMAGE_RUNTIME_OK);
        fixture_destroy(&fixture);
    }
}

int main(void)
{
    test_real_managed_bootstrap_and_gc();
    test_rejects_abi_and_hierarchy_mismatch();
    test_oom_has_zero_external_publication();
    if (failures != 0u) {
        fprintf(stderr, "smalltalk AOT bootstrap: %u failure(s)\n", failures);
        return 1;
    }
    puts("smalltalk AOT bootstrap: PASS (ABI5 class/metaclass objects, canonical Symbols, eager mirrors, precise GC)");
    return 0;
}
