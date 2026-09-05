#include "st_dnu.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                 \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct {
    size_t calls;
    size_t fail_on;
    size_t live_blocks;
} heap_allocator_t;

typedef struct {
    size_t calls;
    size_t fail_on;
    size_t live_blocks;
} root_allocator_t;

static void *heap_allocate(void *user, size_t alignment, size_t size)
{
    heap_allocator_t *allocator = user;
    allocator->calls++;
    if (allocator->calls == allocator->fail_on) {
        return NULL;
    }
    void *pointer = aligned_alloc(alignment, size);
    if (pointer != NULL) {
        allocator->live_blocks++;
    }
    return pointer;
}

static void heap_deallocate(
    void *user, void *pointer, size_t alignment, size_t size)
{
    heap_allocator_t *allocator = user;
    (void)alignment;
    (void)size;
    CHECK(pointer != NULL);
    CHECK(allocator->live_blocks != 0u);
    if (allocator->live_blocks != 0u) {
        allocator->live_blocks--;
    }
    free(pointer);
}

static void *root_allocate(void *user, size_t size)
{
    root_allocator_t *allocator = user;
    allocator->calls++;
    if (allocator->calls == allocator->fail_on) {
        return NULL;
    }
    void *pointer = malloc(size);
    if (pointer != NULL) {
        allocator->live_blocks++;
    }
    return pointer;
}

static void root_deallocate(void *user, void *pointer)
{
    root_allocator_t *allocator = user;
    CHECK(pointer != NULL);
    CHECK(allocator->live_blocks != 0u);
    if (allocator->live_blocks != 0u) {
        allocator->live_blocks--;
    }
    free(pointer);
}

enum {
    C_OBJECT = 1,
    C_OBJECT_META,
    C_CLASS,
    C_CLASS_META,
    C_META_CLASS,
    C_META_CLASS_META,
    C_INTEGER,
    C_INTEGER_META,
    C_SMALL_INTEGER,
    C_SMALL_INTEGER_META,
    C_ARRAY,
    C_ARRAY_META,
    C_METHOD_DICTIONARY,
    C_METHOD_DICTIONARY_META,
    C_STRING,
    C_STRING_META,
    C_SYMBOL,
    C_SYMBOL_META,
    C_COMPILED_METHOD,
    C_COMPILED_METHOD_META,
    C_MESSAGE,
    C_MESSAGE_META,
    C_RECEIVER,
    C_RECEIVER_META,
    C_SUB_RECEIVER,
    C_SUB_RECEIVER_META,
    C_MNU,
    C_MNU_META,
    C_LIMIT
};

enum {
    S_STRING16 = C_LIMIT,
    S_STRING32,
    S_SYMBOL16,
    S_SYMBOL32,
    S_LIMIT
};

enum {
    SELECTOR_DNU = 1,
    SELECTOR_MISSING,
    SELECTOR_UNARY,
    SELECTOR_COUNT = SELECTOR_UNARY
};

typedef struct fixture fixture_t;

struct fixture {
    StClassDescriptor class_storage[C_LIMIT - 1u];
    StShapeDescriptor shape_storage[S_LIMIT - 1u];
    const StClassDescriptor *classes[C_LIMIT - 1u];
    const StShapeDescriptor *shapes[S_LIMIT - 1u];
    st_runtime_descriptors_t descriptors;

    st_image_entity_metadata_t entities[C_LIMIT - 1u];
    uint32_t entity_runtime_ids[C_LIMIT - 1u];
    st_image_runtime_layout_metadata_t layouts[S_LIMIT - 1u];
    st_image_slot_metadata_t slots[18];
    st_image_selector_metadata_t selectors[SELECTOR_COUNT];
    st_image_method_metadata_t methods[2];
    StMethodDescriptor runtime_methods[2];
    StMethodBinding bindings[2];
    StMethodEntry entries[2];
    st_method_slot_t object_slots[1];
    st_method_slot_t subclass_slots[1];
    st_image_metadata_descriptor_t metadata;

    heap_allocator_t heap_allocator;
    root_allocator_t root_allocator;
    st_heap_t heap;
    st_image_runtime_t image;
    st_lookup_context_t lookup;
    st_symbol_intern_context_t symbols;
    st_aot_bootstrap_context_t bootstrap;
    st_control_thread_t control;
    st_aot_thread_t thread;
    st_dnu_context_t dnu;

    uint64_t caller_bitmap;
    st_root_map_t caller_root_map;
    StMethodDescriptor caller_method;
    st_value_t caller_roots[1];
    StFrame caller;
    st_control_scope_t caller_scope;
    st_send_site_t missing_site;
    st_value_t receiver;
    st_value_t argument;
    st_value_t expected_receiver;
    st_value_t expected_argument;
    st_value_t last_message;
    st_value_t last_exception;
    unsigned object_dnu_calls;
    unsigned subclass_dnu_calls;
    bool recurse;
};

static size_t minimum_size(size_t fixed_words)
{
    return (sizeof(st_heap_object_t)
        + fixed_words * sizeof(st_value_t) + 7u) & ~(size_t)7u;
}

static void set_shape(
    fixture_t *fixture, uint32_t shape_id, uint32_t class_id,
    size_t fixed_words, st_indexed_format_t indexed_format,
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
    fixture->shapes[shape_id - 1u] =
        &fixture->shape_storage[shape_id - 1u];
}

static bool class_is_or_inherits(
    void *user, uint32_t class_id, uint32_t ancestor_id)
{
    const st_runtime_descriptors_t *descriptors = user;
    size_t hops = 0u;
    while (class_id != 0u && hops++ < descriptors->class_count) {
        const StClassDescriptor *descriptor = st_runtime_class(
            descriptors, class_id);
        if (descriptor == NULL) {
            return false;
        }
        if (class_id == ancestor_id) {
            return true;
        }
        class_id = descriptor->superclass_id;
    }
    return false;
}

static bool object_class(
    void *user, st_value_t value, uint32_t *class_id_out)
{
    fixture_t *fixture = user;
    st_object_view_t view;
    if (class_id_out == NULL
            || st_heap_object_view(&fixture->heap, value, &view)
                != ST_HEAP_OK) {
        return false;
    }
    *class_id_out = view.class_descriptor->class_id;
    return true;
}

static st_value_t execute_dnu(fixture_t *fixture, StFrame *frame,
                              bool subclass)
{
    st_value_t selector = ST_VALUE_INVALID;
    st_value_t arguments = ST_VALUE_INVALID;
    st_value_t argument = ST_VALUE_INVALID;
    size_t selector_count = 0u;
    const st_value_t *selectors = st_aot_bootstrap_selector_symbols(
        &fixture->bootstrap, &selector_count);

    CHECK(frame != NULL);
    CHECK(frame == NULL || frame->receiver == fixture->expected_receiver);
    CHECK(frame == NULL || frame->argc == 1u);
    if (frame == NULL || frame->argc != 1u) {
        return st_value_nil();
    }
    fixture->last_message = frame->argv[0];
    CHECK(st_heap_fixed_reference_load(
              &fixture->heap, fixture->last_message, 0u, &selector)
          == ST_HEAP_OK);
    CHECK(st_heap_fixed_reference_load(
              &fixture->heap, fixture->last_message, 1u, &arguments)
          == ST_HEAP_OK);
    CHECK(selectors != NULL && selector_count == SELECTOR_COUNT);
    CHECK(selectors != NULL && selector == selectors[SELECTOR_MISSING - 1u]);
    CHECK(st_heap_indexed_reference_load(
              &fixture->heap, arguments, 0u, &argument)
          == ST_HEAP_OK);
    CHECK(argument == fixture->expected_argument);
    if (subclass) {
        fixture->subclass_dnu_calls++;
    } else {
        fixture->object_dnu_calls++;
    }

    if (fixture->recurse) {
        (void)st_aot_send_failure(
            frame, &fixture->missing_site, frame->receiver,
            &fixture->expected_argument, 1u, ST_AOT_SEND_ERR_NOT_FOUND);
        abort();
    }

    st_value_t exception = ST_VALUE_INVALID;
    CHECK(st_heap_allocate(
              &fixture->heap, C_MNU, C_MNU, 0u, 0u, 0u, &exception)
          == ST_HEAP_OK);
    CHECK(st_heap_fixed_reference_store(
              &fixture->heap, exception, 0u, frame->receiver)
          == ST_HEAP_OK);
    CHECK(st_heap_fixed_reference_store(
              &fixture->heap, exception, 1u, fixture->last_message)
          == ST_HEAP_OK);
    fixture->last_exception = exception;

    st_control_scope_t scope;
    st_control_scope_init(&scope);
    CHECK(st_control_scope_enter(&fixture->control, &scope, frame)
          == ST_CONTROL_OK);
    CHECK(st_control_exception_signal(
              &fixture->control, exception, C_MNU,
              class_is_or_inherits, &fixture->descriptors)
          == ST_CONTROL_OK);
    st_control_leave_result_t leave;
    CHECK(st_control_scope_leave(
              &fixture->control, &scope, exception, &leave)
          == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_EXCEPTION_PROPAGATE);
    CHECK(leave.value == exception);
    return leave.value;
}

static st_value_t object_dnu(StFrame *frame)
{
    fixture_t *fixture = ((st_aot_thread_t *)frame->thread)->object_class_user;
    return execute_dnu(fixture, frame, false);
}

static st_value_t subclass_dnu(StFrame *frame)
{
    fixture_t *fixture = ((st_aot_thread_t *)frame->thread)->object_class_user;
    return execute_dnu(fixture, frame, true);
}

static void build_descriptors(fixture_t *fixture)
{
    static const char *const names[(C_LIMIT - 1u) / 2u] = {
        "Object", "Class", "MetaClass", "Integer", "SmallInteger",
        "Array", "MethodDictionary", "String", "Symbol",
        "CompiledMethod", "Message", "Receiver", "SubReceiver",
        "MessageNotUnderstood"
    };
    static const uint32_t normal_supers[(C_LIMIT - 1u) / 2u] = {
        0u, C_OBJECT, C_OBJECT, C_OBJECT, C_INTEGER,
        C_OBJECT, C_OBJECT, C_OBJECT, C_STRING, C_OBJECT,
        C_OBJECT, C_OBJECT, C_RECEIVER, C_OBJECT
    };
    static const uint64_t bitmap9 = UINT64_C(0x1ff);
    static const uint64_t bitmap5 = UINT64_C(0x1f);
    static const uint64_t bitmap3 = UINT64_C(0x7);
    static const uint64_t bitmap2 = UINT64_C(0x3);

    for (uint32_t class_id = 1u; class_id < C_LIMIT; class_id++) {
        size_t pair = (class_id - 1u) / 2u;
        bool meta = (class_id & 1u) == 0u;
        uint32_t normal_super = normal_supers[pair];
        uint32_t superclass = meta
            ? normal_super == 0u ? 0u : normal_super + 1u
            : normal_super;
        fixture->class_storage[class_id - 1u] = (StClassDescriptor) {
            .class_id = class_id,
            .superclass_id = meta && normal_super == 0u ? C_CLASS : superclass,
            .metaclass_id = meta ? class_id : class_id + 1u,
            .default_shape_id = class_id,
            .flags = meta ? ST_CLASS_METACLASS : 0u,
            .name = names[pair],
            .name_length = strlen(names[pair])
        };
        if (!meta && (class_id == C_METHOD_DICTIONARY
                || class_id == C_SYMBOL
                || class_id == C_COMPILED_METHOD)) {
            fixture->class_storage[class_id - 1u].flags |= ST_CLASS_ABSTRACT;
        }
        fixture->classes[class_id - 1u] =
            &fixture->class_storage[class_id - 1u];
        fixture->entities[class_id - 1u] = (st_image_entity_metadata_t) {
            .id = class_id,
            .kind = meta ? ST_CLASS_GRAPH_METACLASS : ST_CLASS_GRAPH_CLASS,
            .superclass_id = superclass,
            .metaclass_id = meta ? 0u : class_id + 1u,
            .instance_class_id = meta ? class_id - 1u : 0u,
            .name = names[pair]
        };
        fixture->entity_runtime_ids[class_id - 1u] = class_id;

        size_t fixed_words = meta ? 9u : 0u;
        st_indexed_format_t indexed_format = ST_INDEXED_NONE;
        const uint64_t *bitmap = &bitmap9;
        if (!meta && class_id == C_CLASS) {
            fixed_words = 9u;
        } else if (!meta && class_id == C_META_CLASS) {
            fixed_words = 5u;
            bitmap = &bitmap5;
        } else if (!meta && (class_id == C_ARRAY
                || class_id == C_METHOD_DICTIONARY)) {
            indexed_format = ST_INDEXED_VALUES;
        } else if (!meta && (class_id == C_STRING
                || class_id == C_SYMBOL)) {
            indexed_format = ST_INDEXED_UINT8;
        } else if (!meta && class_id == C_COMPILED_METHOD) {
            fixed_words = 3u;
            bitmap = &bitmap3;
        } else if (!meta && (class_id == C_MESSAGE || class_id == C_MNU)) {
            fixed_words = 2u;
            bitmap = &bitmap2;
        }
        set_shape(
            fixture, class_id, class_id, fixed_words, indexed_format, bitmap);
    }
    set_shape(fixture, S_STRING16, C_STRING, 0u, ST_INDEXED_UINT16, NULL);
    set_shape(fixture, S_STRING32, C_STRING, 0u, ST_INDEXED_UINT32, NULL);
    set_shape(fixture, S_SYMBOL16, C_SYMBOL, 0u, ST_INDEXED_UINT16, NULL);
    set_shape(fixture, S_SYMBOL32, C_SYMBOL, 0u, ST_INDEXED_UINT32, NULL);
    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->classes, C_LIMIT - 1u,
        fixture->shapes, S_LIMIT - 1u
    };
}

static void build_slots(fixture_t *fixture)
{
    static const char *const class_slots[9] = {
        "superClass", "subClasses", "methodDictionary",
        "instanceVariables", "name", "comment", "category",
        "classVariables", "namespace"
    };
    static const char *const metaclass_slots[5] = {
        "superClass", "subClasses", "methodDictionary",
        "instanceVariables", "instanceClass"
    };
    static const char *const message_slots[2] = {
        "selector", "arguments"
    };
    static const char *const mnu_slots[2] = {
        "receiver", "message"
    };

    fixture->entities[C_CLASS - 1u].instance_slot_count = 9u;
    fixture->entities[C_META_CLASS - 1u].instance_slot_offset = 9u;
    fixture->entities[C_META_CLASS - 1u].instance_slot_count = 5u;
    fixture->entities[C_MESSAGE - 1u].instance_slot_offset = 14u;
    fixture->entities[C_MESSAGE - 1u].instance_slot_count = 2u;
    fixture->entities[C_MNU - 1u].instance_slot_offset = 16u;
    fixture->entities[C_MNU - 1u].instance_slot_count = 2u;

    for (size_t index = 0u; index < 9u; index++) {
        fixture->slots[index] = (st_image_slot_metadata_t) {
            .declaring_class = C_CLASS,
            .slot = (uint32_t)index,
            .kind = ST_CLASS_GRAPH_INSTANCE_SLOT,
            .name = class_slots[index]
        };
    }
    for (size_t index = 0u; index < 5u; index++) {
        fixture->slots[9u + index] = (st_image_slot_metadata_t) {
            .declaring_class = C_META_CLASS,
            .slot = (uint32_t)index,
            .kind = ST_CLASS_GRAPH_INSTANCE_SLOT,
            .name = metaclass_slots[index]
        };
    }
    for (size_t index = 0u; index < 2u; index++) {
        fixture->slots[14u + index] = (st_image_slot_metadata_t) {
            .declaring_class = C_MESSAGE,
            .slot = (uint32_t)index,
            .kind = ST_CLASS_GRAPH_INSTANCE_SLOT,
            .name = message_slots[index]
        };
        fixture->slots[16u + index] = (st_image_slot_metadata_t) {
            .declaring_class = C_MNU,
            .slot = (uint32_t)index,
            .kind = ST_CLASS_GRAPH_INSTANCE_SLOT,
            .name = mnu_slots[index]
        };
    }
}

static void build_methods(fixture_t *fixture)
{
    static const char *const spellings[SELECTOR_COUNT] = {
        "doesNotUnderstand:", "missing:", "unaryMissing"
    };
    static const uint32_t arities[SELECTOR_COUNT] = { 1u, 1u, 0u };
    for (size_t index = 0u; index < SELECTOR_COUNT; index++) {
        fixture->selectors[index] = (st_image_selector_metadata_t) {
            .id = (uint32_t)index + 1u,
            .kind = arities[index] == 0u ? 1u : 3u,
            .hash = (uint64_t)index + 1u,
            .arity = arities[index],
            .spelling = spellings[index]
        };
    }

    fixture->runtime_methods[0] = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = SELECTOR_DNU,
        .owner_class_id = C_OBJECT,
        .arity = 1u,
        .frame_root_capacity = 1u,
        .flags = ST_METHOD_CAN_UNWIND
    };
    fixture->runtime_methods[1] = fixture->runtime_methods[0];
    fixture->runtime_methods[1].owner_class_id = C_SUB_RECEIVER;
    fixture->bindings[0] = (StMethodBinding) {
        &fixture->runtime_methods[0], object_dnu, 1u
    };
    fixture->bindings[1] = (StMethodBinding) {
        &fixture->runtime_methods[1], subclass_dnu, 1u
    };
    CHECK(st_method_entry_init(&fixture->entries[0], &fixture->bindings[0]));
    CHECK(st_method_entry_init(&fixture->entries[1], &fixture->bindings[1]));
    fixture->object_slots[0] = (st_method_slot_t) {
        SELECTOR_DNU, &fixture->entries[0]
    };
    fixture->subclass_slots[0] = (st_method_slot_t) {
        SELECTOR_DNU, &fixture->entries[1]
    };
    fixture->class_storage[C_OBJECT - 1u].method_slots = fixture->object_slots;
    fixture->class_storage[C_OBJECT - 1u].method_slot_count = 1u;
    fixture->class_storage[C_SUB_RECEIVER - 1u].method_slots =
        fixture->subclass_slots;
    fixture->class_storage[C_SUB_RECEIVER - 1u].method_slot_count = 1u;

    for (size_t index = 0u; index < 2u; index++) {
        fixture->methods[index] = (st_image_method_metadata_t) {
            .id = (uint32_t)index + 1u,
            .owner = index == 0u ? C_OBJECT : C_SUB_RECEIVER,
            .instance_class = index == 0u ? C_OBJECT : C_SUB_RECEIVER,
            .selector_id = SELECTOR_DNU,
            .arity = 1u,
            .selector = spellings[0],
            .runtime_descriptor = &fixture->runtime_methods[index]
        };
    }
}

static void build_metadata(fixture_t *fixture)
{
    for (uint32_t shape_id = 1u; shape_id < S_LIMIT; shape_id++) {
        const StShapeDescriptor *shape = fixture->shapes[shape_id - 1u];
        uint32_t recipe = ST_IMAGE_LAYOUT_FIXED_POINTERS;
        if (shape->indexed_format == ST_INDEXED_VALUES) {
            recipe = ST_IMAGE_LAYOUT_INDEXED_VALUES;
        } else if (shape->indexed_format == ST_INDEXED_UINT8) {
            recipe = ST_IMAGE_LAYOUT_INDEXED_UINT8;
        } else if (shape->indexed_format == ST_INDEXED_UINT16) {
            recipe = ST_IMAGE_LAYOUT_INDEXED_UINT16;
        } else if (shape->indexed_format == ST_INDEXED_UINT32) {
            recipe = ST_IMAGE_LAYOUT_INDEXED_UINT32;
        }
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

    uint16_t endian_probe = UINT16_C(1);
    fixture->metadata = (st_image_metadata_descriptor_t) {
        .magic = ST_IMAGE_METADATA_MAGIC,
        .abi_version = ST_IMAGE_METADATA_ABI_VERSION,
        .flags = ST_IMAGE_METADATA_FLAG_TYPED_RELOCATIONS
            | ST_IMAGE_METADATA_FLAG_METHOD_CODE
            | ST_IMAGE_METADATA_FLAG_RUNTIME_METHODS
            | ST_IMAGE_METADATA_FLAG_RUNTIME_DESCRIPTORS,
        .pointer_size = sizeof(void *),
        .endian = *(const uint8_t *)&endian_probe != 0u
            ? ANVIL_ENDIAN_LITTLE : ANVIL_ENDIAN_BIG,
        .entity_count = C_LIMIT - 1u,
        .class_count = (C_LIMIT - 1u) / 2u,
        .metaclass_count = (C_LIMIT - 1u) / 2u,
        .method_count = 2u,
        .selector_count = SELECTOR_COUNT,
        .instance_slot_count = 18u,
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

static st_dnu_context_options_t dnu_options(fixture_t *fixture)
{
    return (st_dnu_context_options_t) {
        .metadata = &fixture->metadata,
        .image = &fixture->image,
        .lookup = &fixture->lookup,
        .bootstrap = &fixture->bootstrap,
        .message_entity_id = C_MESSAGE,
        .message_class_id = C_MESSAGE,
        .message_shape_id = C_MESSAGE,
        .message_selector_slot = 0u,
        .message_arguments_slot = 1u,
        .array_entity_id = C_ARRAY,
        .array_class_id = C_ARRAY,
        .array_shape_id = C_ARRAY,
        .does_not_understand_selector_id = SELECTOR_DNU,
        .allocator = {
            root_allocate, root_deallocate, &fixture->root_allocator
        }
    };
}

static bool fixture_init(fixture_t *fixture, uint32_t receiver_class)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->heap_allocator.fail_on = SIZE_MAX;
    fixture->root_allocator.fail_on = SIZE_MAX;
    build_descriptors(fixture);
    build_slots(fixture);
    build_methods(fixture);
    build_metadata(fixture);
    st_runtime_status_t runtime_status =
        st_runtime_descriptors_validate(&fixture->descriptors);
    if (runtime_status != ST_RUNTIME_OK) {
        fprintf(stderr, "DNU fixture runtime: %s\n",
                st_runtime_status_string(runtime_status));
        return false;
    }
    if (st_heap_init(
            &fixture->heap, &fixture->descriptors,
            (st_runtime_allocator_t) {
                heap_allocate, heap_deallocate, &fixture->heap_allocator
            }) != ST_HEAP_OK) {
        return false;
    }
    if (st_image_runtime_init(
            &fixture->image,
            &(st_image_runtime_options_t) {
                .descriptors = &fixture->descriptors,
                .borrowed_heap = &fixture->heap
            }) != ST_IMAGE_RUNTIME_OK) {
        return false;
    }
    if (st_lookup_context_init(
            &fixture->lookup, &fixture->descriptors,
            (st_lookup_allocator_t) {0}) != ST_LOOKUP_FOUND) {
        return false;
    }
    if (st_symbol_intern_context_init(
            &fixture->symbols,
            &(st_symbol_intern_options_t) {
                .image = &fixture->image,
                .string_class_id = C_STRING,
                .string_uint8_shape_id = C_STRING,
                .string_uint16_shape_id = S_STRING16,
                .string_uint32_shape_id = S_STRING32,
                .symbol_class_id = C_SYMBOL,
                .symbol_uint8_shape_id = C_SYMBOL,
                .symbol_uint16_shape_id = S_SYMBOL16,
                .symbol_uint32_shape_id = S_SYMBOL32
            }) != ST_SYMBOL_INTERN_OK) {
        return false;
    }
    st_aot_bootstrap_options_t bootstrap = bootstrap_options(fixture);
    st_aot_bootstrap_status_t bootstrap_status =
        st_aot_bootstrap_context_init(&fixture->bootstrap, &bootstrap);
    if (bootstrap_status != ST_AOT_BOOTSTRAP_OK) {
        fprintf(stderr, "DNU fixture bootstrap: %s\n",
                st_aot_bootstrap_status_string(bootstrap_status));
        return false;
    }

    if (st_heap_allocate(
            &fixture->heap, receiver_class, receiver_class,
            0u, 0u, 0u, &fixture->receiver) != ST_HEAP_OK
            || !st_value_from_small_integer(42, &fixture->argument)) {
        return false;
    }
    fixture->expected_receiver = fixture->receiver;
    fixture->expected_argument = fixture->argument;

    fixture->caller_bitmap = UINT64_C(1);
    fixture->caller_root_map = (st_root_map_t) {
        .safepoint_id = 7u,
        .root_count = 1u,
        .bitmap_word_count = 1u,
        .live_root_bitmap = &fixture->caller_bitmap
    };
    fixture->caller_method = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = SELECTOR_UNARY,
        .owner_class_id = receiver_class,
        .frame_root_capacity = 1u,
        .flags = ST_METHOD_CAN_UNWIND,
        .root_maps = &fixture->caller_root_map,
        .root_map_count = 1u
    };
    fixture->caller_roots[0] = st_value_nil();
    fixture->caller = (StFrame) {
        .thread = &fixture->thread,
        .method = &fixture->caller_method,
        .receiver = fixture->receiver,
        .roots = fixture->caller_roots,
        .root_count = 1u,
        .safepoint_id = 7u
    };

    uint32_t immediate[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        C_OBJECT, C_INTEGER, C_SMALL_INTEGER, C_ARRAY, C_STRING
    };
    if (st_control_thread_init(
            &fixture->control, &fixture->thread,
            (st_control_allocator_t) {0}) != ST_CONTROL_OK
            || !st_aot_thread_init(
                &fixture->thread, &fixture->lookup, immediate,
                NULL, &fixture->control, NULL,
                object_class, fixture, NULL, NULL)
            || !st_aot_thread_image_attach(
                &fixture->thread, &fixture->image)) {
        return false;
    }
    st_dnu_context_options_t dnu = dnu_options(fixture);
    st_dnu_status_t dnu_status = st_dnu_context_init(&fixture->dnu, &dnu);
    if (dnu_status != ST_DNU_OK) {
        fprintf(stderr, "DNU fixture context: %s\n",
                st_dnu_status_string(dnu_status));
        return false;
    }
    dnu_status = st_dnu_context_attach(&fixture->dnu, &fixture->thread);
    if (dnu_status != ST_DNU_OK) {
        fprintf(stderr, "DNU fixture attach: %s\n",
                st_dnu_status_string(dnu_status));
        return false;
    }
    if (!st_send_site_init(
            &fixture->missing_site, SELECTOR_MISSING, 0u)) {
        return false;
    }
    st_control_scope_init(&fixture->caller_scope);
    return st_control_scope_enter(
        &fixture->control, &fixture->caller_scope, &fixture->caller)
        == ST_CONTROL_OK;
}

#ifndef ST_DNU_AOT_HARNESS

static void clear_pending_and_leave(fixture_t *fixture, st_value_t result)
{
    st_control_leave_result_t leave;
    CHECK(st_control_scope_leave(
              &fixture->control, &fixture->caller_scope, result, &leave)
          == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_EXCEPTION_PROPAGATE);
    CHECK(leave.value == result);
    CHECK(st_control_pending_clear(&fixture->control) == ST_CONTROL_OK);
}

#endif

static void fixture_destroy(fixture_t *fixture)
{
    if (fixture->caller_scope._st_state != 0u
            && fixture->control._st_top_scope == &fixture->caller_scope) {
        st_control_leave_result_t ignored;
        (void)st_control_scope_leave(
            &fixture->control, &fixture->caller_scope,
            st_value_nil(), &ignored);
    }
    if (fixture->control._st_pending_kind != ST_CONTROL_PENDING_NONE) {
        (void)st_control_pending_clear(&fixture->control);
    }
    if (fixture->dnu.attached_thread != NULL) {
        CHECK(st_dnu_context_detach(&fixture->dnu, &fixture->thread)
              == ST_DNU_OK);
    }
    if (fixture->dnu.initialized) {
        CHECK(st_dnu_context_destroy(&fixture->dnu) == ST_DNU_OK);
    }
    if (fixture->thread.image != NULL) {
        CHECK(st_aot_thread_image_detach(
            &fixture->thread, &fixture->image));
    }
    if (fixture->thread.initialized) {
        st_aot_thread_destroy(&fixture->thread);
    }
    if (fixture->control._st_abi_version == ST_CONTROL_ABI_VERSION) {
        CHECK(st_control_thread_destroy(&fixture->control) == ST_CONTROL_OK);
    }
    if (fixture->bootstrap.initialized) {
        st_aot_bootstrap_context_destroy(&fixture->bootstrap);
    }
    if (fixture->symbols.initialized) {
        st_symbol_intern_context_destroy(&fixture->symbols);
    }
    if (fixture->lookup.initialized) {
        st_lookup_context_destroy(&fixture->lookup);
    }
    if (fixture->image.initialized) {
        st_image_runtime_destroy(&fixture->image);
    }
    if (fixture->heap.state != NULL) {
        st_heap_destroy(&fixture->heap);
    }
    CHECK(fixture->root_allocator.live_blocks == 0u);
    CHECK(fixture->heap_allocator.live_blocks == 0u);
}

#ifndef ST_DNU_AOT_HARNESS

static void test_exact_message_and_cooperative_signal(uint32_t class_id)
{
    fixture_t fixture;
    CHECK(fixture_init(&fixture, class_id));
    if (!fixture.dnu.initialized) {
        fixture_destroy(&fixture);
        return;
    }
    st_value_t result = st_aot_send_failure(
        &fixture.caller, &fixture.missing_site, fixture.receiver,
        &fixture.argument, 1u, ST_AOT_SEND_ERR_NOT_FOUND);
    CHECK(result == fixture.last_exception);
    CHECK(st_value_kind(result) == ST_VALUE_OBJECT);
    CHECK(class_id == C_SUB_RECEIVER
        ? fixture.subclass_dnu_calls == 1u
            && fixture.object_dnu_calls == 0u
        : fixture.object_dnu_calls == 1u
            && fixture.subclass_dnu_calls == 0u);
    st_control_pending_info_t pending;
    CHECK(st_control_pending_get(&fixture.control, &pending)
          == ST_CONTROL_OK);
    CHECK(pending.kind == ST_CONTROL_PENDING_EXCEPTION);
    CHECK(pending.value == result);
    CHECK(pending.exception_class_id == C_MNU);

    st_value_t exception_receiver = ST_VALUE_INVALID;
    st_value_t exception_message = ST_VALUE_INVALID;
    CHECK(st_heap_fixed_reference_load(
              &fixture.heap, result, 0u, &exception_receiver)
          == ST_HEAP_OK);
    CHECK(st_heap_fixed_reference_load(
              &fixture.heap, result, 1u, &exception_message)
          == ST_HEAP_OK);
    CHECK(exception_receiver == fixture.receiver);
    CHECK(exception_message == fixture.last_message);
    clear_pending_and_leave(&fixture, result);
    fixture_destroy(&fixture);
}

static void expect_fatal_child(unsigned mode)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        fixture_t fixture;
        if (!fixture_init(&fixture, C_RECEIVER)) {
            _exit(90);
        }
        if (mode == 0u) {
            fixture.class_storage[C_OBJECT - 1u].method_slot_count = 0u;
            st_send_site_clear(&fixture.dnu.does_not_understand_site);
        } else if (mode == 1u) {
            fixture.recurse = true;
        } else {
            fixture.root_allocator.fail_on =
                fixture.root_allocator.calls + 1u;
        }
        (void)st_aot_send_failure(
            &fixture.caller, &fixture.missing_site, fixture.receiver,
            &fixture.argument, 1u, ST_AOT_SEND_ERR_NOT_FOUND);
        _exit(91);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status));
    CHECK(!WIFSIGNALED(status) || WTERMSIG(status) == SIGABRT);
}

static void test_oom_is_unpublished_and_collectable(void)
{
    fixture_t fixture;
    CHECK(fixture_init(&fixture, C_RECEIVER));
    if (!fixture.dnu.initialized) {
        fixture_destroy(&fixture);
        return;
    }
    size_t objects_before = st_heap_object_count(&fixture.heap);
    fixture.heap_allocator.fail_on = fixture.heap_allocator.calls + 1u;
    st_value_t message = st_value_nil();
    CHECK(st_dnu_message_create(
              &fixture.dnu, &fixture.caller, &fixture.missing_site,
              fixture.receiver, &fixture.argument, 1u, &message)
          == ST_DNU_ERR_OUT_OF_MEMORY);
    CHECK(message == ST_VALUE_INVALID);
    CHECK(st_heap_object_count(&fixture.heap) == objects_before);

    /* The first retry grows the exact-base registry before allocating the
     * Array. Fail the following Message allocation, after that growth and the
     * Array object allocation have both succeeded. */
    fixture.heap_allocator.fail_on = fixture.heap_allocator.calls + 3u;
    CHECK(st_dnu_message_create(
              &fixture.dnu, &fixture.caller, &fixture.missing_site,
              fixture.receiver, &fixture.argument, 1u, &message)
          == ST_DNU_ERR_OUT_OF_MEMORY);
    CHECK(message == ST_VALUE_INVALID);
    CHECK(st_heap_object_count(&fixture.heap) == objects_before + 1u);
    fixture.heap_allocator.fail_on = SIZE_MAX;
    st_heap_collection_stats_t stats;
    CHECK(st_image_runtime_collect(
              &fixture.image, &fixture.caller, &stats)
          == ST_IMAGE_RUNTIME_OK);
    CHECK(stats.reclaimed_objects >= 1u);
    CHECK(st_heap_object_count(&fixture.heap) <= objects_before);

    st_control_leave_result_t leave;
    CHECK(st_control_scope_leave(
              &fixture.control, &fixture.caller_scope,
              st_value_nil(), &leave) == ST_CONTROL_OK);
    CHECK(leave.kind == ST_CONTROL_LEAVE_NORMAL);
    fixture_destroy(&fixture);
}

static void test_layout_and_lifecycle_rejections(void)
{
    fixture_t fixture;
    CHECK(fixture_init(&fixture, C_RECEIVER));
    if (!fixture.dnu.initialized) {
        fixture_destroy(&fixture);
        return;
    }
    CHECK(st_dnu_context_destroy(&fixture.dnu) == ST_DNU_ERR_CONFLICT);
    CHECK(st_dnu_context_detach(&fixture.dnu, &fixture.thread) == ST_DNU_OK);
    CHECK(st_dnu_context_destroy(&fixture.dnu) == ST_DNU_OK);
    st_dnu_context_options_t options = dnu_options(&fixture);
    options.array_entity_id = C_MESSAGE;
    CHECK(st_dnu_context_init(&fixture.dnu, &options)
          == ST_DNU_ERR_INVALID_LAYOUT);
    options.array_entity_id = C_ARRAY;
    options.message_arguments_slot = 0u;
    CHECK(st_dnu_context_init(&fixture.dnu, &options)
          == ST_DNU_ERR_INVALID_ARGUMENT);

    st_control_leave_result_t leave;
    CHECK(st_control_scope_leave(
              &fixture.control, &fixture.caller_scope,
              st_value_nil(), &leave) == ST_CONTROL_OK);
    fixture_destroy(&fixture);
}

int main(void)
{
    test_exact_message_and_cooperative_signal(C_RECEIVER);
    test_exact_message_and_cooperative_signal(C_SUB_RECEIVER);
    /* Valgrind cannot observe cleanup after an intentional SIGABRT. The
     * ordinary gate always executes these children; leak-only invocations may
     * skip them explicitly and validate all returning lifecycle paths. */
    if (getenv("ST_DNU_SKIP_FATAL_CHILDREN") == NULL) {
        expect_fatal_child(0u);
        expect_fatal_child(1u);
        expect_fatal_child(2u);
    }
    test_oom_is_unpublished_and_collectable();
    test_layout_and_lifecycle_rejections();
    if (failures != 0u) {
        fprintf(stderr, "Smalltalk DNU: %u failure(s)\n", failures);
        return 1;
    }
    puts("Smalltalk DNU: PASS (canonical Message, PIC override, cooperative MNU, fatal invariants, OOM rollback)");
    return 0;
}

#else

#ifndef GENERATED_ROOT_CAPACITY
#error "GENERATED_ROOT_CAPACITY must match the lowered AOT method"
#endif

#ifndef GENERATED_SAFEPOINT_COUNT
#error "GENERATED_SAFEPOINT_COUNT must match the lowered AOT method"
#endif

extern st_value_t st_DnuProbe_run(StFrame *frame);

int main(void)
{
    fixture_t fixture;
    if (!fixture_init(&fixture, C_RECEIVER)) {
        return 1;
    }

    st_control_leave_result_t initial_leave;
    if (st_control_scope_leave(
            &fixture.control, &fixture.caller_scope,
            st_value_nil(), &initial_leave) != ST_CONTROL_OK
            || initial_leave.kind != ST_CONTROL_LEAVE_NORMAL) {
        return 2;
    }

    uint64_t live_bitmap = GENERATED_ROOT_CAPACITY == 64u
        ? UINT64_MAX
        : (UINT64_C(1) << GENERATED_ROOT_CAPACITY) - UINT64_C(1);
    st_root_map_t root_maps[GENERATED_SAFEPOINT_COUNT];
    for (uint32_t index = 0u; index < GENERATED_SAFEPOINT_COUNT; index++) {
        root_maps[index] = (st_root_map_t) {
            .safepoint_id = index + 1u,
            .root_count = GENERATED_ROOT_CAPACITY,
            .bitmap_word_count = 1u,
            .live_root_bitmap = &live_bitmap
        };
    }
    StMethodDescriptor method = {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = SELECTOR_UNARY,
        .owner_class_id = C_RECEIVER,
        .arity = 1u,
        .frame_root_capacity = GENERATED_ROOT_CAPACITY,
        .flags = ST_METHOD_CAN_UNWIND,
        .code_size = 1u,
        .root_maps = root_maps,
        .root_map_count = GENERATED_SAFEPOINT_COUNT
    };
    if (!st_method_descriptor_is_valid(&method)) {
        return 3;
    }
    st_value_t roots[GENERATED_ROOT_CAPACITY];
    for (uint32_t index = 0u; index < GENERATED_ROOT_CAPACITY; index++) {
        roots[index] = st_value_nil();
    }
    st_value_t arguments[1] = { fixture.receiver };
    StFrame frame = {
        .thread = &fixture.thread,
        .method = &method,
        .receiver = fixture.receiver,
        .argv = arguments,
        .roots = roots,
        .argc = 1u,
        .root_count = GENERATED_ROOT_CAPACITY
    };
    fixture.expected_receiver = fixture.receiver;
    fixture.expected_argument = fixture.argument;
    st_value_t result = st_DnuProbe_run(&frame);
    st_control_pending_info_t pending;
    if (result != fixture.last_exception
            || fixture.object_dnu_calls != 1u
            || fixture.subclass_dnu_calls != 0u
            || st_control_pending_get(&fixture.control, &pending)
                != ST_CONTROL_OK
            || pending.kind != ST_CONTROL_PENDING_EXCEPTION
            || pending.value != result
            || pending.exception_class_id != C_MNU) {
        return 4;
    }
    if (st_control_pending_clear(&fixture.control) != ST_CONTROL_OK) {
        return 5;
    }
    fixture_destroy(&fixture);
    return failures == 0u ? 0 : 6;
}

#endif
