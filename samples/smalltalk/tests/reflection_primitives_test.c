#include "st_reflection_primitive_bridge.h"
#include "st_reflection_primitives.h"
#include "st_heap_primitives.h"
#include "st_send_bridge.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                  \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_PARENT,
    CLASS_CHILD,
    CLASS_SYMBOL,
    CLASS_METACLASS,
    CLASS_COMPILED_METHOD,
    CLASS_COUNT
};

enum {
    SELECTOR_INHERITED = 1,
    SELECTOR_OWN,
    SELECTOR_MISSING,
    SELECTOR_COUNT = SELECTOR_MISSING
};

typedef struct {
    size_t calls;
    size_t fail_on;
    size_t live;
} allocation_t;

typedef struct {
    StClassDescriptor class_storage[CLASS_COUNT - 1u];
    StShapeDescriptor shape_storage[CLASS_COUNT - 1u];
    const StClassDescriptor *classes[CLASS_COUNT - 1u];
    const StShapeDescriptor *shapes[CLASS_COUNT - 1u];
    st_runtime_descriptors_t descriptors;
    uint64_t compiled_method_bitmap;
    StMethodDescriptor method_descriptors[2];
    StMethodBinding method_bindings[2];
    StMethodEntry method_entries[2];
    st_method_slot_t parent_slots[1];
    st_method_slot_t child_slots[1];
} descriptors_t;

typedef struct {
    descriptors_t descriptors;
    allocation_t heap_allocation;
    st_heap_t heap;
    st_value_t class_objects[CLASS_COUNT - 1u];
    st_value_t selector_symbols[SELECTOR_COUNT];
    st_image_runtime_entry_t roots[(CLASS_COUNT - 1u) + SELECTOR_COUNT];
    st_image_runtime_t image;
    st_lookup_context_t lookup;
    st_reflection_context_t reflection;
} fixture_t;

static void *tracked_heap_allocate(void *user, size_t alignment, size_t size)
{
    allocation_t *allocation = user;
    void *result;
    allocation->calls++;
    if (allocation->fail_on != 0u
            && allocation->calls == allocation->fail_on)
        return NULL;
    result = aligned_alloc(alignment, size);
    if (result != NULL) {
        allocation->live++;
    }
    return result;
}

static void tracked_heap_deallocate(void *user, void *pointer,
                                    size_t alignment, size_t size)
{
    allocation_t *allocation = user;
    (void)alignment;
    (void)size;
    if (pointer != NULL) {
        CHECK(allocation->live != 0u);
        allocation->live--;
        free(pointer);
    }
}

static st_value_t leaf_method(StFrame *frame)
{
    (void)frame;
    return st_value_nil();
}

static void descriptors_init(descriptors_t *fixture)
{
    static const char *const names[CLASS_COUNT - 1u] = {
        "Object", "Parent", "Child", "Symbol", "Metaclass",
        "CompiledMethod"
    };
    memset(fixture, 0, sizeof(*fixture));

    fixture->method_descriptors[0] = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = SELECTOR_INHERITED,
        .owner_class_id = CLASS_PARENT,
        .arity = 0u,
        .source_name = "Parent.st",
        .source_name_length = sizeof("Parent.st") - 1u
    };
    fixture->method_descriptors[1] = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = SELECTOR_OWN,
        .owner_class_id = CLASS_CHILD,
        .arity = 1u,
        .source_name = "Child.st",
        .source_name_length = sizeof("Child.st") - 1u
    };
    for (size_t index = 0u; index < 2u; index++) {
        fixture->method_bindings[index] = (StMethodBinding) {
            &fixture->method_descriptors[index], leaf_method, 1u
        };
        CHECK(st_method_entry_init(&fixture->method_entries[index],
                                   &fixture->method_bindings[index]));
    }
    fixture->parent_slots[0] = (st_method_slot_t) {
        SELECTOR_INHERITED, &fixture->method_entries[0]
    };
    fixture->child_slots[0] = (st_method_slot_t) {
        SELECTOR_OWN, &fixture->method_entries[1]
    };

    for (uint32_t id = 1u; id < CLASS_COUNT; id++) {
        size_t index = (size_t)id - 1u;
        uint32_t superclass = id == CLASS_OBJECT || id == CLASS_METACLASS
            ? 0u : CLASS_OBJECT;
        if (id == CLASS_CHILD) {
            superclass = CLASS_PARENT;
        }
        fixture->class_storage[index] = (StClassDescriptor) {
            .class_id = id,
            .superclass_id = superclass,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = id,
            .flags = id == CLASS_METACLASS
                ? ST_CLASS_METACLASS
                : id == CLASS_COMPILED_METHOD ? ST_CLASS_ABSTRACT : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        fixture->shape_storage[index] = (StShapeDescriptor) {
            .shape_id = id,
            .class_id = id,
            .allocation_alignment = 8u,
            .minimum_allocation_size = 24u,
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->class_storage[index];
        fixture->shapes[index] = &fixture->shape_storage[index];
    }
    fixture->class_storage[CLASS_PARENT - 1u].method_slots =
        fixture->parent_slots;
    fixture->class_storage[CLASS_PARENT - 1u].method_slot_count = 1u;
    fixture->class_storage[CLASS_CHILD - 1u].method_slots =
        fixture->child_slots;
    fixture->class_storage[CLASS_CHILD - 1u].method_slot_count = 1u;

    fixture->shape_storage[CLASS_SYMBOL - 1u].indexed_format =
        ST_INDEXED_UINT8;
    fixture->compiled_method_bitmap = UINT64_C(0x7);
    fixture->shape_storage[CLASS_COMPILED_METHOD - 1u]
        .minimum_allocation_size = 48u;
    fixture->shape_storage[CLASS_COMPILED_METHOD - 1u]
        .fixed_word_count = 3u;
    fixture->shape_storage[CLASS_COMPILED_METHOD - 1u]
        .fixed_pointer_bitmap = &fixture->compiled_method_bitmap;
    fixture->shape_storage[CLASS_COMPILED_METHOD - 1u]
        .fixed_pointer_bitmap_word_count = 1u;

    fixture->descriptors = (st_runtime_descriptors_t) {
        fixture->classes,
        CLASS_COUNT - 1u,
        fixture->shapes,
        CLASS_COUNT - 1u
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors)
          == ST_RUNTIME_OK);
}

static st_value_t allocate_object(st_heap_t *heap, uint32_t class_id,
                                  uint32_t shape_id, size_t indexed_length,
                                  st_header_flags_t flags)
{
    st_value_t value = ST_VALUE_INVALID;
    CHECK(st_heap_allocate(heap, class_id, shape_id, indexed_length,
                           indexed_length, flags, &value) == ST_HEAP_OK);
    return value;
}

static void write_symbol(st_heap_t *heap, st_value_t symbol,
                         const char *spelling)
{
    st_object_view_t view;
    size_t length = strlen(spelling);
    CHECK(st_heap_object_view(heap, symbol, &view) == ST_HEAP_OK);
    CHECK(view.indexed_length == length
          && view.shape_descriptor->indexed_format == ST_INDEXED_UINT8);
    memcpy(view.indexed_elements, spelling, length);
}

static st_reflection_context_options_t reflection_options(
    fixture_t *fixture, st_primitive_allocator_t allocator)
{
    return (st_reflection_context_options_t) {
        .image = &fixture->image,
        .lookup = &fixture->lookup,
        .class_objects_by_id = fixture->class_objects,
        .class_object_count = CLASS_COUNT - 1u,
        .selector_symbols_by_id = fixture->selector_symbols,
        .selector_symbol_count = SELECTOR_COUNT,
        .symbol_class_id = CLASS_SYMBOL,
        .compiled_method_class_id = CLASS_COMPILED_METHOD,
        .compiled_method_shape_id = CLASS_COMPILED_METHOD,
        .allocator = allocator
    };
}

static bool fixture_init(fixture_t *fixture)
{
    st_runtime_allocator_t heap_allocator;
    st_image_runtime_options_t image_options;
    st_reflection_context_options_t reflection_init_options;
    st_reflection_primitive_status_t reflection_status;
    size_t root_index = 0u;
    static const char *const selectors[SELECTOR_COUNT] = {
        "inherited", "own:", "missing"
    };
    memset(fixture, 0, sizeof(*fixture));
    descriptors_init(&fixture->descriptors);
    heap_allocator = (st_runtime_allocator_t) {
        tracked_heap_allocate,
        tracked_heap_deallocate,
        &fixture->heap_allocation
    };
    if (st_heap_init(&fixture->heap, &fixture->descriptors.descriptors,
                     heap_allocator) != ST_HEAP_OK)
        return false;

    for (size_t index = 0u; index < CLASS_COUNT - 1u; index++) {
        fixture->class_objects[index] = allocate_object(
            &fixture->heap, CLASS_METACLASS, CLASS_METACLASS, 0u, 0u);
        fixture->roots[root_index] = (st_image_runtime_entry_t) {
            (uint32_t)root_index + 1u,
            fixture->class_objects[index]
        };
        root_index++;
    }
    for (size_t index = 0u; index < SELECTOR_COUNT; index++) {
        size_t length = strlen(selectors[index]);
        fixture->selector_symbols[index] = allocate_object(
            &fixture->heap, CLASS_SYMBOL, CLASS_SYMBOL, length,
            ST_HEADER_IMMUTABLE);
        write_symbol(&fixture->heap, fixture->selector_symbols[index],
                     selectors[index]);
        fixture->roots[root_index] = (st_image_runtime_entry_t) {
            (uint32_t)root_index + 1u,
            fixture->selector_symbols[index]
        };
        root_index++;
    }

    image_options = (st_image_runtime_options_t) {
        .descriptors = &fixture->descriptors.descriptors,
        .borrowed_heap = &fixture->heap,
        .globals = fixture->roots,
        .global_count = root_index
    };
    if (st_image_runtime_init(&fixture->image, &image_options)
            != ST_IMAGE_RUNTIME_OK)
        return false;
    if (st_lookup_context_init(
            &fixture->lookup, &fixture->descriptors.descriptors,
            (st_lookup_allocator_t) {0}) != ST_LOOKUP_FOUND)
        return false;
    reflection_init_options = reflection_options(
        fixture, (st_primitive_allocator_t) {0});
    reflection_status = st_reflection_context_init(
        &fixture->reflection, &reflection_init_options);
    CHECK(reflection_status == ST_REFLECTION_PRIMITIVE_OK);
    return reflection_status == ST_REFLECTION_PRIMITIVE_OK;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_reflection_context_destroy(&fixture->reflection);
    st_lookup_context_destroy(&fixture->lookup);
    st_image_runtime_destroy(&fixture->image);
    st_heap_destroy(&fixture->heap);
    CHECK(fixture->heap_allocation.live == 0u);
}

static void check_compiled_method(fixture_t *fixture, st_value_t method,
                                  st_value_t selector,
                                  uint32_t defining_class_id,
                                  int64_t expected_arity)
{
    st_object_view_t view;
    int64_t arity = -1;
    CHECK(st_heap_object_view(&fixture->heap, method, &view) == ST_HEAP_OK);
    CHECK(view.class_descriptor->class_id == CLASS_COMPILED_METHOD);
    CHECK(view.shape_descriptor->fixed_word_count == 3u);
    CHECK(view.shape_descriptor->fixed_pointer_bitmap_word_count == 1u);
    CHECK(view.shape_descriptor->fixed_pointer_bitmap[0] == UINT64_C(0x7));
    CHECK(view.indexed_length == 0u
          && view.shape_descriptor->indexed_format == ST_INDEXED_NONE);
    st_value_t *slots = view.fixed_words;
    CHECK(slots[0] == selector);
    CHECK(slots[1] == fixture->class_objects[defining_class_id - 1u]);
    CHECK(st_value_to_small_integer(slots[2], &arity));
    CHECK(arity == expected_arity);
    for (size_t index = 0u; index < 3u; index++) {
        CHECK(st_value_has_valid_encoding(slots[index]));
    }
}

static void test_lookup_own_inherited_and_miss(void)
{
    fixture_t fixture;
    st_value_t own = ST_VALUE_INVALID;
    st_value_t inherited = ST_VALUE_INVALID;
    st_value_t result = ST_VALUE_INVALID;
    CHECK(fixture_init(&fixture));

    CHECK(st_reflection_lookup_selector(
              &fixture.reflection,
              fixture.class_objects[CLASS_CHILD - 1u],
              fixture.selector_symbols[SELECTOR_OWN - 1u], &own)
          == ST_REFLECTION_PRIMITIVE_OK);
    check_compiled_method(&fixture, own,
                          fixture.selector_symbols[SELECTOR_OWN - 1u],
                          CLASS_CHILD, 1);
    CHECK(st_reflection_lookup_selector(
              &fixture.reflection,
              fixture.class_objects[CLASS_CHILD - 1u],
              fixture.selector_symbols[SELECTOR_OWN - 1u], &result)
          == ST_REFLECTION_PRIMITIVE_OK);
    CHECK(result == own);
    st_value_t eager = ST_VALUE_INVALID;
    CHECK(st_reflection_compiled_method_for_entry(
              &fixture.reflection,
              &fixture.descriptors.method_entries[1], &eager)
          == ST_REFLECTION_PRIMITIVE_OK);
    CHECK(eager == own);

    st_heap_indexed_access_t indexed_access[CLASS_COUNT - 1u] = {0};
    indexed_access[CLASS_SYMBOL - 1u] =
        ST_HEAP_INDEXED_ACCESS_CHARACTER;
    st_heap_primitive_context_t heap_primitives = {0};
    st_heap_primitive_options_t heap_options = {
        .heap = &fixture.heap,
        .immediate_classes = {
            .small_integer_class_id = CLASS_OBJECT,
            .character_class_id = CLASS_OBJECT,
            .nil_class_id = CLASS_OBJECT,
            .false_class_id = CLASS_OBJECT,
            .true_class_id = CLASS_OBJECT
        },
        .class_objects = fixture.class_objects,
        .class_object_count = CLASS_COUNT - 1u,
        .indexed_access = indexed_access,
        .indexed_access_count = CLASS_COUNT - 1u
    };
    st_value_t forbidden_instance = ST_VALUE_INVALID;
    CHECK(st_heap_primitive_context_init(
              &heap_primitives, &heap_options) == ST_HEAP_PRIMITIVE_OK);
    CHECK(st_heap_primitive_execute(
              &heap_primitives, ST_INTRINSIC_BEHAVIOR_NEW,
              fixture.class_objects[CLASS_COMPILED_METHOD - 1u],
              NULL, 0u, &forbidden_instance)
          == ST_HEAP_PRIMITIVE_ERR_ABSTRACT_CLASS);
    CHECK(forbidden_instance == ST_VALUE_INVALID);
    st_heap_primitive_context_destroy(&heap_primitives);

    CHECK(st_reflection_lookup_selector(
              &fixture.reflection,
              fixture.class_objects[CLASS_CHILD - 1u],
              fixture.selector_symbols[SELECTOR_INHERITED - 1u], &inherited)
          == ST_REFLECTION_PRIMITIVE_OK);
    check_compiled_method(
        &fixture, inherited,
        fixture.selector_symbols[SELECTOR_INHERITED - 1u], CLASS_PARENT, 0);

    CHECK(st_reflection_lookup_selector(
              &fixture.reflection,
              fixture.class_objects[CLASS_CHILD - 1u],
              fixture.selector_symbols[SELECTOR_MISSING - 1u], &result)
          == ST_REFLECTION_PRIMITIVE_OK);
    CHECK(result == st_value_nil());

    StMethodBinding replacement = {
        &fixture.descriptors.method_descriptors[1], leaf_method, 2u
    };
    CHECK(st_lookup_publish_binding(
              &fixture.lookup, &fixture.descriptors.method_entries[1],
              &replacement, NULL) == ST_LOOKUP_FOUND);
    CHECK(st_reflection_lookup_selector(
              &fixture.reflection,
              fixture.class_objects[CLASS_CHILD - 1u],
              fixture.selector_symbols[SELECTOR_OWN - 1u], &result)
          == ST_REFLECTION_PRIMITIVE_OK);
    CHECK(result != own);
    check_compiled_method(&fixture, result,
                          fixture.selector_symbols[SELECTOR_OWN - 1u],
                          CLASS_CHILD, 1);

    CHECK(st_reflection_lookup_selector(
              &fixture.reflection,
              fixture.class_objects[CLASS_OBJECT - 1u],
              fixture.selector_symbols[SELECTOR_OWN - 1u], &result)
          == ST_REFLECTION_PRIMITIVE_OK);
    CHECK(result == st_value_nil());
    fixture_destroy(&fixture);
}

typedef struct {
    fixture_t *fixture;
    _Atomic bool *start;
    st_reflection_primitive_status_t status;
    st_value_t result;
} concurrent_lookup_t;

static void *run_concurrent_lookup(void *argument)
{
    concurrent_lookup_t *lookup = argument;
    lookup->status = ST_REFLECTION_PRIMITIVE_OK;
    lookup->result = ST_VALUE_INVALID;
    while (!atomic_load_explicit(lookup->start, memory_order_acquire)) {
        sched_yield();
    }
    for (size_t iteration = 0u; iteration < 256u; iteration++) {
        st_value_t result = ST_VALUE_INVALID;
        st_reflection_primitive_status_t status =
            st_reflection_lookup_selector(
                &lookup->fixture->reflection,
                lookup->fixture->class_objects[CLASS_CHILD - 1u],
                lookup->fixture->selector_symbols[SELECTOR_OWN - 1u],
                &result);
        if (status != ST_REFLECTION_PRIMITIVE_OK
                || (lookup->result != ST_VALUE_INVALID
                    && lookup->result != result)) {
            lookup->status = status == ST_REFLECTION_PRIMITIVE_OK
                ? ST_REFLECTION_PRIMITIVE_ERR_BAD_OBJECT : status;
            return NULL;
        }
        lookup->result = result;
    }
    return NULL;
}

static void test_concurrent_identity_after_replacement(void)
{
    enum { THREAD_COUNT = 8 };
    fixture_t fixture;
    pthread_t threads[THREAD_COUNT];
    concurrent_lookup_t lookups[THREAD_COUNT];
    _Atomic bool start = false;
    StMethodBinding replacement;

    CHECK(fixture_init(&fixture));
    replacement = (StMethodBinding) {
        &fixture.descriptors.method_descriptors[1], leaf_method, 2u
    };
    CHECK(st_lookup_publish_binding(
              &fixture.lookup, &fixture.descriptors.method_entries[1],
              &replacement, NULL) == ST_LOOKUP_FOUND);

    size_t created = 0u;
    for (; created < THREAD_COUNT; created++) {
        lookups[created] = (concurrent_lookup_t) {
            &fixture, &start, ST_REFLECTION_PRIMITIVE_OK, ST_VALUE_INVALID
        };
        if (pthread_create(
                &threads[created], NULL, run_concurrent_lookup,
                &lookups[created]) != 0) {
            break;
        }
    }
    CHECK(created == THREAD_COUNT);
    atomic_store_explicit(&start, true, memory_order_release);
    for (size_t index = 0u; index < created; index++) {
        CHECK(pthread_join(threads[index], NULL) == 0);
    }
    if (created == THREAD_COUNT) {
        for (size_t index = 0u; index < THREAD_COUNT; index++) {
            CHECK(lookups[index].status == ST_REFLECTION_PRIMITIVE_OK);
            CHECK(lookups[index].result == lookups[0].result);
        }
        check_compiled_method(
            &fixture, lookups[0].result,
            fixture.selector_symbols[SELECTOR_OWN - 1u], CLASS_CHILD, 1);
    }
    fixture_destroy(&fixture);
}

static void test_invalid_values_and_oom(void)
{
    fixture_t fixture;
    st_value_t result = ST_VALUE_INVALID;
    st_value_t foreign_symbol;
    CHECK(fixture_init(&fixture));

    CHECK(st_reflection_lookup_selector(
              &fixture.reflection, st_value_true(),
              fixture.selector_symbols[0], &result)
          == ST_REFLECTION_PRIMITIVE_ERR_TYPE_MISMATCH);
    CHECK(result == ST_VALUE_INVALID);
    CHECK(st_reflection_lookup_selector(
              &fixture.reflection, fixture.class_objects[0],
              st_value_nil(), &result)
          == ST_REFLECTION_PRIMITIVE_ERR_TYPE_MISMATCH);
    CHECK(st_reflection_lookup_selector(
              &fixture.reflection, fixture.class_objects[0],
              UINT64_C(0x1000), &result)
          == ST_REFLECTION_PRIMITIVE_ERR_TYPE_MISMATCH);

    foreign_symbol = allocate_object(
        &fixture.heap, CLASS_SYMBOL, CLASS_SYMBOL, 7u,
        ST_HEADER_IMMUTABLE);
    write_symbol(&fixture.heap, foreign_symbol, "foreign");
    CHECK(st_reflection_lookup_selector(
              &fixture.reflection, fixture.class_objects[0],
              foreign_symbol, &result) == ST_REFLECTION_PRIMITIVE_OK);
    CHECK(result == st_value_nil());

    StMethodBinding replacement = {
        &fixture.descriptors.method_descriptors[1], leaf_method, 2u
    };
    CHECK(st_lookup_publish_binding(
              &fixture.lookup, &fixture.descriptors.method_entries[1],
              &replacement, NULL) == ST_LOOKUP_FOUND);
    fixture.heap_allocation.fail_on = fixture.heap_allocation.calls + 1u;
    CHECK(st_reflection_lookup_selector(
              &fixture.reflection,
              fixture.class_objects[CLASS_CHILD - 1u],
              fixture.selector_symbols[SELECTOR_OWN - 1u], &result)
          == ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY);
    CHECK(result == ST_VALUE_INVALID);
    fixture.heap_allocation.fail_on = 0u;
    fixture_destroy(&fixture);
}

typedef struct {
    size_t calls;
    size_t fail_on;
    size_t live;
} context_allocation_t;

static void *context_allocate(void *user, size_t size)
{
    context_allocation_t *allocation = user;
    allocation->calls++;
    if (allocation->calls == allocation->fail_on) {
        return NULL;
    }
    void *pointer = malloc(size);
    if (pointer != NULL) {
        allocation->live++;
    }
    return pointer;
}

static void context_deallocate(void *user, void *pointer)
{
    context_allocation_t *allocation = user;
    if (pointer != NULL) {
        CHECK(allocation->live != 0u);
        allocation->live--;
        free(pointer);
    }
}

static void test_context_transaction_and_root_contract(void)
{
    fixture_t fixture;
    CHECK(fixture_init(&fixture));
    st_reflection_context_destroy(&fixture.reflection);

    for (size_t fail_on = 1u; fail_on <= 8u; fail_on++) {
        context_allocation_t allocation = { 0u, fail_on, 0u };
        st_reflection_context_t context = {0};
        st_primitive_allocator_t allocator = {
            context_allocate, context_deallocate, &allocation
        };
        st_reflection_context_options_t options = reflection_options(
            &fixture, allocator);
        CHECK(st_reflection_context_init(&context, &options)
              == ST_REFLECTION_PRIMITIVE_ERR_OUT_OF_MEMORY);
        CHECK(!context.initialized && context.state == NULL);
        CHECK(allocation.live == 0u);
    }

    st_value_t saved = fixture.selector_symbols[SELECTOR_MISSING - 1u];
    fixture.selector_symbols[SELECTOR_MISSING - 1u] = allocate_object(
        &fixture.heap, CLASS_SYMBOL, CLASS_SYMBOL, 8u,
        ST_HEADER_IMMUTABLE);
    write_symbol(&fixture.heap,
                 fixture.selector_symbols[SELECTOR_MISSING - 1u],
                 "unrooted");
    st_reflection_context_t context = {0};
    st_reflection_context_options_t options = reflection_options(
        &fixture, (st_primitive_allocator_t) {0});
    CHECK(st_reflection_context_init(&context, &options)
          == ST_REFLECTION_PRIMITIVE_ERR_UNROOTED_BOOTSTRAP_OBJECT);
    CHECK(!context.initialized && context.state == NULL);
    fixture.selector_symbols[SELECTOR_MISSING - 1u] = saved;

    fixture.descriptors.class_storage[CLASS_COMPILED_METHOD - 1u].flags = 0u;
    options = reflection_options(
        &fixture, (st_primitive_allocator_t) {0});
    CHECK(st_reflection_context_init(&context, &options)
          == ST_REFLECTION_PRIMITIVE_ERR_INVALID_DESCRIPTOR);
    CHECK(!context.initialized && context.state == NULL);
    fixture.descriptors.class_storage[CLASS_COMPILED_METHOD - 1u].flags =
        ST_CLASS_ABSTRACT;

    options = reflection_options(
        &fixture, (st_primitive_allocator_t) {0});
    CHECK(st_reflection_context_init(&fixture.reflection, &options)
          == ST_REFLECTION_PRIMITIVE_OK);
    fixture_destroy(&fixture);
}

static void test_bridge_and_thread_attachment(void)
{
    fixture_t fixture;
    st_aot_thread_t thread = {0};
    uint32_t immediate[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_OBJECT, CLASS_PARENT, CLASS_CHILD, CLASS_SYMBOL,
        CLASS_COMPILED_METHOD
    };
    StMethodDescriptor caller_method = {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = SELECTOR_OWN,
        .owner_class_id = CLASS_CHILD,
        .arity = 1u,
        .source_name = "Bridge.st",
        .source_name_length = sizeof("Bridge.st") - 1u
    };
    st_value_t argument;
    StFrame frame;
    st_value_t result = ST_VALUE_INVALID;
    uint32_t detail = UINT32_MAX;
    CHECK(fixture_init(&fixture));
    CHECK(st_aot_thread_init(&thread, &fixture.lookup, immediate,
                             NULL, NULL, NULL, NULL, NULL, NULL, NULL));
    CHECK(!st_aot_thread_reflection_attach(&thread, &fixture.reflection));
    CHECK(st_aot_thread_image_attach(&thread, &fixture.image));
    CHECK(st_aot_thread_reflection_attach(&thread, &fixture.reflection));
    CHECK(!st_aot_thread_reflection_attach(&thread, &fixture.reflection));

    argument = fixture.selector_symbols[SELECTOR_INHERITED - 1u];
    memset(&frame, 0, sizeof(frame));
    frame.thread = &thread;
    frame.method = &caller_method;
    frame.receiver = fixture.class_objects[CLASS_CHILD - 1u];
    frame.argv = &argument;
    frame.argc = 1u;
    CHECK(st_aot_behavior_lookup_selector_primitive_execute(
              &frame, frame.receiver, &argument, 1u, &result, &detail)
          == ST_REFLECTION_PRIMITIVE_OK);
    CHECK(detail == 0u);
    check_compiled_method(
        &fixture, result, argument, CLASS_PARENT, 0);

    CHECK(st_aot_behavior_lookup_selector_primitive_execute(
              &frame, frame.receiver, NULL, 0u, &result, &detail)
          == ST_REFLECTION_PRIMITIVE_ERR_WRONG_ARITY);
    CHECK(result == ST_VALUE_INVALID && detail == 0u);
    CHECK(st_aot_thread_reflection_detach(&thread, &fixture.reflection));
    CHECK(!st_aot_thread_reflection_detach(&thread, &fixture.reflection));
    CHECK(st_aot_thread_image_detach(&thread, &fixture.image));
    st_aot_thread_destroy(&thread);
    fixture_destroy(&fixture);
}

static void test_gc_traces_only_managed_fields(void)
{
    fixture_t fixture;
    st_value_t method = ST_VALUE_INVALID;
    st_heap_collection_stats_t stats;
    CHECK(fixture_init(&fixture));
    CHECK(st_reflection_lookup_selector(
              &fixture.reflection,
              fixture.class_objects[CLASS_CHILD - 1u],
              fixture.selector_symbols[SELECTOR_INHERITED - 1u], &method)
          == ST_REFLECTION_PRIMITIVE_OK);
    st_reflection_context_destroy(&fixture.reflection);
    st_lookup_context_destroy(&fixture.lookup);
    st_image_runtime_destroy(&fixture.image);

    CHECK(st_heap_collect(&fixture.heap, NULL, &method, 1u, &stats)
          == ST_HEAP_OK);
    CHECK(stats.marked_objects == 3u);
    CHECK(st_heap_contains(&fixture.heap, method));
    CHECK(st_heap_contains(
        &fixture.heap,
        fixture.selector_symbols[SELECTOR_INHERITED - 1u]));
    CHECK(st_heap_contains(
        &fixture.heap, fixture.class_objects[CLASS_PARENT - 1u]));
    CHECK(!st_heap_contains(
        &fixture.heap, fixture.class_objects[CLASS_CHILD - 1u]));

    st_heap_destroy(&fixture.heap);
    CHECK(fixture.heap_allocation.live == 0u);
}

static void test_catalog(void)
{
    size_t count = 0u;
    const st_primitive_spec_t *specs = st_reflection_primitive_specs(&count);
    CHECK(specs != NULL && count == 1u);
    CHECK(strcmp(specs[0].name, "BehaviorLookupSelectorPrimitive") == 0);
    CHECK(specs[0].method_arity == 1u);
    CHECK(specs[0].failure_policy == ST_PRIMITIVE_FALL_THROUGH);
    CHECK(specs[0].implementation_kind == ST_PRIMITIVE_RUNTIME_SYMBOL);
    CHECK(specs[0].intrinsic_id == ST_PRIMITIVE_INVALID_INTRINSIC_ID);
    CHECK(strcmp(specs[0].runtime_symbol,
                 "st_aot_behavior_lookup_selector_primitive_execute") == 0);
}

int main(void)
{
    test_lookup_own_inherited_and_miss();
    test_concurrent_identity_after_replacement();
    test_invalid_values_and_oom();
    test_context_transaction_and_root_contract();
    test_bridge_and_thread_attachment();
    test_gc_traces_only_managed_fields();
    test_catalog();

    if (failures != 0u) {
        fprintf(stderr, "reflection primitives: %u failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("reflection primitives: PASS");
    return EXIT_SUCCESS;
}
