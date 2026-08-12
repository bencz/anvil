#include "st_lookup.h"

#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                       \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_ANIMAL,
    CLASS_DOG,
    CLASS_CAT,
    CLASS_BIRD,
    CLASS_FISH,
    CLASS_HORSE,
    CLASS_METACLASS,
    CLASS_COUNT
};

enum {
    SELECTOR_SPEAK = 1,
    SELECTOR_LOW = 2,
    SELECTOR_MIDDLE = 17,
    SELECTOR_HIGH = 33
};

static st_value_t return_true(StFrame *frame)
{
    (void)frame;
    return st_value_true();
}

static st_value_t return_false(StFrame *frame)
{
    (void)frame;
    return st_value_false();
}

typedef struct {
    StMethodDescriptor methods[5];
    StMethodBinding bindings[5];
    StMethodEntry entries[5];
    st_method_slot_t object_slots[3];
    st_method_slot_t animal_slots[1];
    st_method_slot_t dog_slots[1];
    StClassDescriptor classes_storage[CLASS_COUNT - 1];
    StShapeDescriptor shapes_storage[CLASS_COUNT - 1];
    const StClassDescriptor *classes[CLASS_COUNT - 1];
    const StShapeDescriptor *shapes[CLASS_COUNT - 1];
    st_runtime_descriptors_t descriptors;
} fixture_t;

static const char *const class_names[CLASS_COUNT - 1] = {
    "Object", "Animal", "Dog", "Cat", "Bird", "Fish", "Horse",
    "Class"
};

static void init_method(fixture_t *fixture, size_t index,
                        uint32_t selector_id, uint32_t owner_class_id,
                        st_method_code_t code)
{
    fixture->methods[index] = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = selector_id,
        .owner_class_id = owner_class_id,
        .code_size = 1
    };
    fixture->bindings[index] = (StMethodBinding) {
        .descriptor = &fixture->methods[index],
        .code = code,
        .version = 1
    };
    CHECK(st_method_entry_init(&fixture->entries[index],
                               &fixture->bindings[index]));
}

static void fixture_init(fixture_t *fixture)
{
    size_t index;
    memset(fixture, 0, sizeof(*fixture));
    init_method(fixture, 0, SELECTOR_LOW, CLASS_OBJECT, return_true);
    init_method(fixture, 1, SELECTOR_MIDDLE, CLASS_OBJECT, return_false);
    init_method(fixture, 2, SELECTOR_HIGH, CLASS_OBJECT, return_true);
    init_method(fixture, 3, SELECTOR_SPEAK, CLASS_ANIMAL, return_false);
    init_method(fixture, 4, SELECTOR_SPEAK, CLASS_DOG, return_true);

    fixture->object_slots[0] = (st_method_slot_t) {
        SELECTOR_LOW, &fixture->entries[0]
    };
    fixture->object_slots[1] = (st_method_slot_t) {
        SELECTOR_MIDDLE, &fixture->entries[1]
    };
    fixture->object_slots[2] = (st_method_slot_t) {
        SELECTOR_HIGH, &fixture->entries[2]
    };
    fixture->animal_slots[0] = (st_method_slot_t) {
        SELECTOR_SPEAK, &fixture->entries[3]
    };
    fixture->dog_slots[0] = (st_method_slot_t) {
        SELECTOR_SPEAK, &fixture->entries[4]
    };

    for (index = 0; index < CLASS_COUNT - 1; index++) {
        uint32_t class_id = (uint32_t)index + 1;
        fixture->classes_storage[index] = (StClassDescriptor) {
            .class_id = class_id,
            .superclass_id = class_id == CLASS_OBJECT ||
                             class_id == CLASS_METACLASS
                ? 0 : (class_id == CLASS_ANIMAL ? CLASS_OBJECT
                                                  : CLASS_ANIMAL),
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = class_id,
            .flags = class_id == CLASS_METACLASS ? ST_CLASS_METACLASS : 0,
            .name = class_names[index],
            .name_length = strlen(class_names[index])
        };
        fixture->shapes_storage[index] = (StShapeDescriptor) {
            .shape_id = class_id,
            .class_id = class_id,
            .allocation_alignment = 8,
            .minimum_allocation_size = 24,
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->classes_storage[index];
        fixture->shapes[index] = &fixture->shapes_storage[index];
    }
    fixture->classes_storage[CLASS_OBJECT - 1].method_slots =
        fixture->object_slots;
    fixture->classes_storage[CLASS_OBJECT - 1].method_slot_count = 3;
    fixture->classes_storage[CLASS_ANIMAL - 1].method_slots =
        fixture->animal_slots;
    fixture->classes_storage[CLASS_ANIMAL - 1].method_slot_count = 1;
    fixture->classes_storage[CLASS_DOG - 1].method_slots =
        fixture->dog_slots;
    fixture->classes_storage[CLASS_DOG - 1].method_slot_count = 1;
    fixture->descriptors = (st_runtime_descriptors_t) {
        .classes = fixture->classes,
        .class_count = CLASS_COUNT - 1,
        .shapes = fixture->shapes,
        .shape_count = CLASS_COUNT - 1
    };
    CHECK(st_runtime_descriptors_validate(&fixture->descriptors) ==
          ST_RUNTIME_OK);
}

static void test_inherited_override_super_and_binary_search(void)
{
    fixture_t fixture;
    st_lookup_context_t context = {0};
    st_lookup_result_t result;
    fixture_init(&fixture);
    CHECK(st_lookup_context_init(&context, &fixture.descriptors,
                                 (st_lookup_allocator_t){0}) ==
          ST_LOOKUP_FOUND);
    CHECK(st_lookup_inherited(&context, CLASS_CAT, SELECTOR_SPEAK, &result) ==
          ST_LOOKUP_FOUND);
    CHECK(result.entry == &fixture.entries[3]);
    CHECK(result.defining_class_id == CLASS_ANIMAL);
    CHECK(result.binding->code(NULL) == st_value_false());

    CHECK(st_lookup_inherited(&context, CLASS_DOG, SELECTOR_SPEAK, &result) ==
          ST_LOOKUP_FOUND);
    CHECK(result.entry == &fixture.entries[4]);
    CHECK(result.defining_class_id == CLASS_DOG);
    CHECK(result.binding->code(NULL) == st_value_true());

    CHECK(st_lookup_super(&context, CLASS_DOG, SELECTOR_SPEAK, &result) ==
          ST_LOOKUP_FOUND);
    CHECK(result.entry == &fixture.entries[3]);
    CHECK(st_lookup_super(&context, CLASS_OBJECT, SELECTOR_LOW, &result) ==
          ST_LOOKUP_NOT_FOUND);

    CHECK(st_lookup_inherited(&context, CLASS_HORSE, SELECTOR_LOW, &result) ==
          ST_LOOKUP_FOUND);
    CHECK(result.entry == &fixture.entries[0]);
    CHECK(st_lookup_inherited(&context, CLASS_HORSE, SELECTOR_MIDDLE,
                              &result) == ST_LOOKUP_FOUND);
    CHECK(result.entry == &fixture.entries[1]);
    CHECK(st_lookup_inherited(&context, CLASS_HORSE, SELECTOR_HIGH, &result) ==
          ST_LOOKUP_FOUND);
    CHECK(result.entry == &fixture.entries[2]);
    CHECK(st_lookup_inherited(&context, CLASS_HORSE, 16, &result) ==
          ST_LOOKUP_NOT_FOUND);
    CHECK(result.entry == NULL && result.binding == NULL);
    st_lookup_context_destroy(&context);
}

typedef struct {
    unsigned calls;
} miss_counter_t;

static st_lookup_status_t counting_miss(
    void *user, const st_lookup_context_t *context,
    uint32_t receiver_class_id, uint32_t lookup_start_class_id,
    st_selector_id_t selector_id, st_lookup_result_t *result_out)
{
    miss_counter_t *counter = user;
    counter->calls++;
    return st_lookup_default_miss(NULL, context, receiver_class_id,
                                  lookup_start_class_id, selector_id,
                                  result_out);
}

static void test_pic_hits_collisions_and_explicit_miss(void)
{
    fixture_t fixture;
    st_lookup_context_t context = {0};
    st_send_site_t site = {0};
    st_send_site_t absent = {0};
    st_send_site_t super_site = {0};
    st_lookup_result_t result;
    miss_counter_t counter = {0};
    bool hit;
    const uint32_t receivers[5] = {
        CLASS_ANIMAL, CLASS_DOG, CLASS_CAT, CLASS_BIRD, CLASS_FISH
    };
    fixture_init(&fixture);
    CHECK(st_lookup_context_init(&context, &fixture.descriptors,
                                 (st_lookup_allocator_t){0}) ==
          ST_LOOKUP_FOUND);
    CHECK(st_send_site_init(&site, SELECTOR_SPEAK, 0));
    for (size_t index = 0; index < 5; index++) {
        CHECK(st_send_site_resolve(&context, &site, receivers[index],
                                   counting_miss, &counter, &result, &hit) ==
              ST_LOOKUP_FOUND);
        CHECK(!hit);
    }
    CHECK(counter.calls == 5);
    /* The fifth receiver replaces the first of four ways (bit-mask victim
     * selection), while the most recently populated way remains a hit. */
    CHECK(st_send_site_resolve(&context, &site, CLASS_FISH, counting_miss,
                               &counter, &result, &hit) == ST_LOOKUP_FOUND);
    CHECK(hit && counter.calls == 5);
    CHECK(st_send_site_resolve(&context, &site, CLASS_ANIMAL, counting_miss,
                               &counter, &result, &hit) == ST_LOOKUP_FOUND);
    CHECK(!hit && counter.calls == 6);

    st_send_site_clear(&site);
    CHECK(st_send_site_resolve(&context, &site, CLASS_FISH, counting_miss,
                               &counter, &result, &hit) == ST_LOOKUP_FOUND);
    CHECK(!hit && counter.calls == 7);

    CHECK(st_send_site_init(&absent, 999, 0));
    CHECK(st_send_site_resolve(&context, &absent, CLASS_DOG, NULL, NULL,
                               &result, &hit) == ST_LOOKUP_NOT_FOUND);
    CHECK(!hit && result.entry == NULL && result.binding == NULL);

    CHECK(st_send_site_init(&super_site, SELECTOR_SPEAK, CLASS_DOG));
    CHECK(st_send_site_resolve(&context, &super_site, CLASS_DOG, NULL, NULL,
                               &result, &hit) == ST_LOOKUP_FOUND);
    CHECK(!hit && result.entry == &fixture.entries[3]);
    CHECK(st_send_site_resolve(&context, &super_site, CLASS_DOG, NULL, NULL,
                               &result, &hit) == ST_LOOKUP_FOUND);
    CHECK(hit);
    CHECK(st_send_site_resolve(&context, &super_site, CLASS_CAT, NULL, NULL,
                               &result, &hit) ==
          ST_LOOKUP_ERR_INVALID_ARGUMENT);
    st_lookup_context_destroy(&context);
}

static void test_replacement_version_and_epoch_invalidation(void)
{
    fixture_t fixture;
    st_lookup_context_t context = {0};
    st_send_site_t site = {0};
    st_lookup_result_t result;
    miss_counter_t counter = {0};
    StMethodBinding replacement;
    StMethodBinding direct_replacement;
    const StMethodBinding *old = NULL;
    uint32_t object_epoch;
    uint32_t animal_epoch;
    uint32_t cat_epoch;
    bool hit;
    fixture_init(&fixture);
    CHECK(st_lookup_context_init(&context, &fixture.descriptors,
                                 (st_lookup_allocator_t){0}) ==
          ST_LOOKUP_FOUND);
    CHECK(st_send_site_init(&site, SELECTOR_SPEAK, 0));
    CHECK(st_send_site_resolve(&context, &site, CLASS_CAT, counting_miss,
                               &counter, &result, &hit) == ST_LOOKUP_FOUND);
    CHECK(!hit && result.binding == &fixture.bindings[3]);
    CHECK(st_send_site_resolve(&context, &site, CLASS_CAT, counting_miss,
                               &counter, &result, &hit) == ST_LOOKUP_FOUND);
    CHECK(hit && counter.calls == 1);

    CHECK(st_lookup_context_class_epoch(&context, CLASS_OBJECT,
                                        &object_epoch));
    CHECK(st_lookup_context_class_epoch(&context, CLASS_ANIMAL,
                                        &animal_epoch));
    CHECK(st_lookup_context_class_epoch(&context, CLASS_CAT, &cat_epoch));
    replacement = fixture.bindings[3];
    replacement.code = return_true;
    replacement.version = 2;
    CHECK(st_lookup_publish_binding(&context, &fixture.entries[3],
                                    &replacement, &old) == ST_LOOKUP_FOUND);
    CHECK(old == &fixture.bindings[3]);
    {
        uint32_t epoch;
        CHECK(st_lookup_context_class_epoch(&context, CLASS_OBJECT, &epoch));
        CHECK(epoch == object_epoch);
        CHECK(st_lookup_context_class_epoch(&context, CLASS_ANIMAL, &epoch));
        CHECK(epoch == animal_epoch + 1);
        CHECK(st_lookup_context_class_epoch(&context, CLASS_CAT, &epoch));
        CHECK(epoch == cat_epoch + 1);
    }
    CHECK(st_send_site_resolve(&context, &site, CLASS_CAT, counting_miss,
                               &counter, &result, &hit) == ST_LOOKUP_FOUND);
    CHECK(!hit && counter.calls == 2 && result.binding == &replacement);
    CHECK(result.binding->code(NULL) == st_value_true());
    CHECK(st_lookup_publish_binding(&context, &fixture.entries[3],
                                    &replacement, NULL) ==
          ST_LOOKUP_ERR_VERSION_CONFLICT);
    {
        StMethodEntry foreign_entry;
        CHECK(st_method_entry_init(&foreign_entry, &fixture.bindings[3]));
        CHECK(st_lookup_publish_binding(&context, &foreign_entry,
                                        &replacement, NULL) ==
              ST_LOOKUP_ERR_INVALID_ARGUMENT);
    }

    /* Even a direct entry publication which omits the eager epoch bump cannot
     * produce a stale hit: the cached binding pointer is reconfirmed. */
    direct_replacement = replacement;
    direct_replacement.code = return_false;
    direct_replacement.version = 3;
    CHECK(st_method_entry_publish(&fixture.entries[3], &direct_replacement,
                                  NULL));
    CHECK(st_send_site_resolve(&context, &site, CLASS_CAT, counting_miss,
                               &counter, &result, &hit) == ST_LOOKUP_FOUND);
    CHECK(!hit && counter.calls == 3 &&
          result.binding == &direct_replacement);
    st_lookup_context_destroy(&context);
}

typedef struct {
    bool fail;
    bool misalign;
    size_t allocations;
    size_t deallocations;
    alignas(max_align_t) unsigned char bytes[256];
} allocator_state_t;

static void *test_allocate(void *user, size_t size)
{
    allocator_state_t *state = user;
    state->allocations++;
    if (state->fail || size + 1 > sizeof(state->bytes)) return NULL;
    return state->bytes + (state->misalign ? 1 : 0);
}

static void test_deallocate(void *user, void *pointer)
{
    allocator_state_t *state = user;
    CHECK(pointer != NULL);
    state->deallocations++;
}

static st_lookup_allocator_t allocator_for(allocator_state_t *state)
{
    return (st_lookup_allocator_t) {
        .allocate = test_allocate,
        .deallocate = test_deallocate,
        .user = state
    };
}

static st_lookup_status_t invalid_callback(
    void *user, const st_lookup_context_t *context,
    uint32_t receiver_class_id, uint32_t lookup_start_class_id,
    st_selector_id_t selector_id, st_lookup_result_t *result_out)
{
    fixture_t *fixture = user;
    (void)context;
    (void)receiver_class_id;
    (void)lookup_start_class_id;
    (void)selector_id;
    result_out->entry = &fixture->entries[0];
    result_out->binding = &fixture->bindings[0];
    result_out->defining_class_id = CLASS_OBJECT;
    return ST_LOOKUP_FOUND;
}

static st_lookup_status_t injected_result_callback(
    void *user, const st_lookup_context_t *context,
    uint32_t receiver_class_id, uint32_t lookup_start_class_id,
    st_selector_id_t selector_id, st_lookup_result_t *result_out)
{
    (void)context;
    (void)receiver_class_id;
    (void)lookup_start_class_id;
    (void)selector_id;
    *result_out = *(const st_lookup_result_t *)user;
    return ST_LOOKUP_FOUND;
}

static void test_faults_invalid_descriptors_callbacks_and_epoch_wrap(void)
{
    fixture_t fixture;
    st_lookup_context_t context = {0};
    st_lookup_context_t wrap_context = {0};
    st_send_site_t site = {0};
    st_lookup_result_t result;
    bool hit;
    allocator_state_t allocator_state = { .fail = true };
    fixture_init(&fixture);
    CHECK(st_lookup_context_init(&context, &fixture.descriptors,
                                 allocator_for(&allocator_state)) ==
          ST_LOOKUP_ERR_OUT_OF_MEMORY);
    CHECK(allocator_state.allocations == 1 &&
          allocator_state.deallocations == 0);
    allocator_state.fail = false;
    allocator_state.misalign = true;
    CHECK(st_lookup_context_init(&context, &fixture.descriptors,
                                 allocator_for(&allocator_state)) ==
          ST_LOOKUP_ERR_BAD_ALIGNMENT);
    CHECK(allocator_state.deallocations == 1);
    CHECK(st_lookup_context_init(
              &context, &fixture.descriptors,
              (st_lookup_allocator_t){ .allocate = test_allocate }) ==
          ST_LOOKUP_ERR_INVALID_ARGUMENT);

    {
        StClassDescriptor malformed = fixture.classes_storage[CLASS_OBJECT - 1];
        const StClassDescriptor *saved = fixture.classes[CLASS_OBJECT - 1];
        st_method_slot_t bad_slots[3] = {
            fixture.object_slots[1], fixture.object_slots[0],
            fixture.object_slots[2]
        };
        malformed.method_slots = bad_slots;
        fixture.classes[CLASS_OBJECT - 1] = &malformed;
        CHECK(st_lookup_context_init(&context, &fixture.descriptors,
                                     (st_lookup_allocator_t){0}) ==
              ST_LOOKUP_ERR_INVALID_DESCRIPTOR);
        fixture.classes[CLASS_OBJECT - 1] = saved;
    }
    {
        StClassDescriptor malformed = fixture.classes_storage[CLASS_CAT - 1];
        const StClassDescriptor *saved = fixture.classes[CLASS_CAT - 1];
        StMethodEntry noncanonical_entry;
        st_method_slot_t flattened_slot;
        CHECK(st_method_entry_init(&noncanonical_entry,
                                   &fixture.bindings[3]));
        flattened_slot = (st_method_slot_t) {
            SELECTOR_SPEAK, &noncanonical_entry
        };
        malformed.method_slots = &flattened_slot;
        malformed.method_slot_count = 1;
        fixture.classes[CLASS_CAT - 1] = &malformed;
        /* The base runtime accepts an otherwise valid flattened slot, but the
         * lookup layer requires the exact stable entry owned by Animal. */
        CHECK(st_runtime_descriptors_validate(&fixture.descriptors) ==
              ST_RUNTIME_OK);
        CHECK(st_lookup_context_init(&context, &fixture.descriptors,
                                     (st_lookup_allocator_t){0}) ==
              ST_LOOKUP_ERR_INVALID_DESCRIPTOR);
        fixture.classes[CLASS_CAT - 1] = saved;
    }
    CHECK(st_lookup_context_init(&context, &fixture.descriptors,
                                 (st_lookup_allocator_t){0}) ==
          ST_LOOKUP_FOUND);
    CHECK(st_send_site_init(&site, SELECTOR_SPEAK, 0));
    CHECK(st_send_site_resolve(&context, &site, CLASS_CAT, invalid_callback,
                               &fixture, &result, &hit) ==
          ST_LOOKUP_ERR_CALLBACK_RESULT);
    CHECK(result.entry == NULL && result.binding == NULL);
    {
        StMethodDescriptor foreign_descriptor = fixture.methods[3];
        StMethodBinding foreign_binding = {
            .descriptor = &foreign_descriptor,
            .code = return_true,
            .version = 91
        };
        StMethodEntry foreign_entry;
        st_lookup_result_t injected;
        CHECK(st_method_entry_init(&foreign_entry, &foreign_binding));
        injected = (st_lookup_result_t) {
            .entry = &foreign_entry,
            .binding = &foreign_binding,
            .defining_class_id = CLASS_ANIMAL
        };
        /* Every scalar field is coherent and the owner is an ancestor, but
         * the entry is not the exact descriptor-graph slot. */
        CHECK(st_send_site_resolve(&context, &site, CLASS_CAT,
                                   injected_result_callback, &injected,
                                   &result, &hit) ==
              ST_LOOKUP_ERR_CALLBACK_RESULT);
        CHECK(result.entry == NULL && result.binding == NULL);
    }
    CHECK(!st_send_site_init(&site, SELECTOR_SPEAK, 0));
    CHECK(!st_send_site_init(&(st_send_site_t){0}, 0, 0));
    st_lookup_context_destroy(&context);

    CHECK(st_lookup_context_init_with_epoch(
              &wrap_context, &fixture.descriptors,
              (st_lookup_allocator_t){0}, UINT32_MAX) == ST_LOOKUP_FOUND);
    CHECK(st_lookup_invalidate_class(&wrap_context, CLASS_ANIMAL) ==
          ST_LOOKUP_ERR_EPOCH_EXHAUSTED);
    {
        uint32_t epoch = 0;
        CHECK(st_lookup_context_class_epoch(&wrap_context, CLASS_CAT, &epoch));
        CHECK(epoch == UINT32_MAX);
    }
    {
        StMethodBinding replacement = fixture.bindings[3];
        replacement.version = 2;
        CHECK(st_lookup_publish_binding(&wrap_context, &fixture.entries[3],
                                        &replacement, NULL) ==
              ST_LOOKUP_ERR_EPOCH_EXHAUSTED);
        CHECK(st_method_entry_load(&fixture.entries[3]) ==
              &fixture.bindings[3]);
    }
    st_lookup_context_destroy(&wrap_context);
}

typedef struct {
    st_lookup_context_t *context;
    st_send_site_t *site;
    StMethodEntry *entry;
    StMethodBinding *bindings;
    size_t binding_count;
    _Atomic(bool) done;
    _Atomic(unsigned) errors;
    _Atomic(unsigned) misses;
} concurrent_state_t;

static st_lookup_status_t concurrent_miss(
    void *user, const st_lookup_context_t *context,
    uint32_t receiver_class_id, uint32_t lookup_start_class_id,
    st_selector_id_t selector_id, st_lookup_result_t *result_out)
{
    concurrent_state_t *state = user;
    atomic_fetch_add_explicit(&state->misses, 1, memory_order_relaxed);
    return st_lookup_default_miss(NULL, context, receiver_class_id,
                                  lookup_start_class_id, selector_id,
                                  result_out);
}

static void *concurrent_writer(void *argument)
{
    concurrent_state_t *state = argument;
    for (size_t index = 1; index < state->binding_count; index++) {
        if (st_lookup_publish_binding(state->context, state->entry,
                                      &state->bindings[index], NULL) !=
            ST_LOOKUP_FOUND)
            atomic_fetch_add_explicit(&state->errors, 1,
                                      memory_order_relaxed);
    }
    atomic_store_explicit(&state->done, true, memory_order_release);
    return NULL;
}

static void *concurrent_reader(void *argument)
{
    concurrent_state_t *state = argument;
    do {
        st_lookup_result_t result;
        bool hit;
        st_lookup_status_t status = st_send_site_resolve(
            state->context, state->site, CLASS_DOG, concurrent_miss, state,
            &result, &hit);
        (void)hit;
        if (status != ST_LOOKUP_FOUND || !result.binding ||
            result.entry != state->entry ||
            result.binding->code(NULL) !=
                ((result.binding->version & 1u) != 0
                    ? st_value_true() : st_value_false()))
            atomic_fetch_add_explicit(&state->errors, 1,
                                      memory_order_relaxed);
    } while (!atomic_load_explicit(&state->done, memory_order_acquire));
    return NULL;
}

static void test_concurrent_pic_readers_and_writer(void)
{
    enum { BINDING_COUNT = 2048, READER_COUNT = 4 };
    fixture_t fixture;
    st_lookup_context_t context = {0};
    st_send_site_t site = {0};
    StMethodBinding *bindings = malloc(sizeof(*bindings) * BINDING_COUNT);
    concurrent_state_t state;
    pthread_t writer;
    pthread_t readers[READER_COUNT];
    fixture_init(&fixture);
    CHECK(bindings != NULL);
    if (!bindings) return;
    for (size_t index = 0; index < BINDING_COUNT; index++) {
        bindings[index] = (StMethodBinding) {
            .descriptor = &fixture.methods[4],
            .code = ((index + 1) & 1u) != 0 ? return_true : return_false,
            .version = index + 1
        };
    }
    CHECK(st_method_entry_init(&fixture.entries[4], &bindings[0]));
    CHECK(st_lookup_context_init(&context, &fixture.descriptors,
                                 (st_lookup_allocator_t){0}) ==
          ST_LOOKUP_FOUND);
    CHECK(st_send_site_init(&site, SELECTOR_SPEAK, 0));
    state = (concurrent_state_t) {
        .context = &context,
        .site = &site,
        .entry = &fixture.entries[4],
        .bindings = bindings,
        .binding_count = BINDING_COUNT
    };
    atomic_init(&state.done, false);
    atomic_init(&state.errors, 0);
    atomic_init(&state.misses, 0);
    for (size_t index = 0; index < READER_COUNT; index++)
        CHECK(pthread_create(&readers[index], NULL, concurrent_reader,
                             &state) == 0);
    CHECK(pthread_create(&writer, NULL, concurrent_writer, &state) == 0);
    CHECK(pthread_join(writer, NULL) == 0);
    for (size_t index = 0; index < READER_COUNT; index++)
        CHECK(pthread_join(readers[index], NULL) == 0);
    CHECK(atomic_load_explicit(&state.errors, memory_order_relaxed) == 0);
    CHECK(atomic_load_explicit(&state.misses, memory_order_relaxed) != 0);
    CHECK(st_method_entry_load(&fixture.entries[4])->version == BINDING_COUNT);
    st_lookup_context_destroy(&context);
    free(bindings);
}

int main(void)
{
    test_inherited_override_super_and_binary_search();
    test_pic_hits_collisions_and_explicit_miss();
    test_replacement_version_and_epoch_invalidation();
    test_faults_invalid_descriptors_callbacks_and_epoch_wrap();
    test_concurrent_pic_readers_and_writer();
    if (failures != 0) {
        fprintf(stderr, "smalltalk lookup tests: %u failure(s)\n", failures);
        return 1;
    }
    puts("smalltalk lookup tests: ok");
    return 0;
}
