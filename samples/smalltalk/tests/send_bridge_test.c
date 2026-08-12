#include "st_send_bridge.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                         \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

enum {
    CLASS_OBJECT = 1,
    CLASS_NIL,
    CLASS_FALSE,
    CLASS_TRUE,
    CLASS_SMALL_INTEGER,
    CLASS_CHARACTER,
    CLASS_METACLASS,
    CLASS_COUNT
};

enum { SELECTOR_VALUE = 1 };

typedef struct {
    StMethodDescriptor caller_method;
    StMethodDescriptor leaf_method;
    StMethodDescriptor rooted_method;
    StMethodDescriptor mapped_method;
    st_root_map_t target_map;
    uint64_t target_bitmap;
    StMethodBinding leaf_binding;
    StMethodBinding rooted_binding;
    StMethodBinding mapped_binding;
    StMethodEntry leaf_entry;
    st_method_slot_t true_slots[1];
    StClassDescriptor classes_storage[CLASS_COUNT - 1];
    StShapeDescriptor shapes_storage[CLASS_COUNT - 1];
    const StClassDescriptor *classes[CLASS_COUNT - 1];
    const StShapeDescriptor *shapes[CLASS_COUNT - 1];
    st_runtime_descriptors_t descriptors;
    st_lookup_context_t lookup;
    st_aot_thread_t thread;
    StFrame frame;
    st_send_site_t site;
    unsigned failure_calls;
    bool failure_must_not_run;
    bool return_bogus_object;
} fixture_t;

static st_value_t leaf_false(StFrame *frame)
{
    CHECK(frame != NULL);
    CHECK(frame == NULL || frame->argc == 0u);
    return st_value_false();
}

static bool object_class(void *user, st_value_t value, uint32_t *class_id_out)
{
    (void)user;
    if (value != UINT64_C(0x1000) || class_id_out == NULL) return false;
    *class_id_out = CLASS_OBJECT;
    return true;
}

static st_value_t failure_policy(
    void *user, StFrame *caller, const st_send_site_t *site,
    st_value_t receiver, const st_value_t *argv, uint32_t argc,
    st_aot_send_status_t status)
{
    fixture_t *fixture = user;
    (void)caller;
    (void)site;
    (void)receiver;
    (void)argv;
    (void)argc;
    (void)status;
    if (fixture->failure_must_not_run) _exit(99);
    fixture->failure_calls++;
    return fixture->return_bogus_object ? UINT64_C(0x8) : st_value_false();
}

static void init_descriptor(StMethodDescriptor *descriptor,
                            uint32_t selector, uint32_t owner,
                            uint32_t root_capacity, uint32_t flags)
{
    *descriptor = (StMethodDescriptor) {
        .abi_version = ST_METHOD_ABI_VERSION,
        .selector_id = selector,
        .owner_class_id = owner,
        .frame_root_capacity = root_capacity,
        .flags = flags,
        .code_size = 1u
    };
}

static bool fixture_init(fixture_t *fixture)
{
    static const char *const names[CLASS_COUNT - 1] = {
        "Object", "UndefinedObject", "False", "True", "SmallInteger",
        "Character", "Class"
    };
    const uint32_t immediate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_SMALL_INTEGER,
        CLASS_CHARACTER
    };
    memset(fixture, 0, sizeof(*fixture));
    init_descriptor(&fixture->caller_method, 99u, CLASS_OBJECT, 0u, 0u);
    init_descriptor(&fixture->leaf_method, SELECTOR_VALUE, CLASS_TRUE, 0u, 0u);
    init_descriptor(&fixture->rooted_method, SELECTOR_VALUE, CLASS_TRUE, 1u,
                    0u);
    init_descriptor(&fixture->mapped_method, SELECTOR_VALUE, CLASS_TRUE, 1u,
                    0u);
    fixture->target_bitmap = UINT64_C(1);
    fixture->target_map = (st_root_map_t) {
        .safepoint_id = 1u,
        .root_count = 1u,
        .bitmap_word_count = 1u,
        .live_root_bitmap = &fixture->target_bitmap
    };
    fixture->mapped_method.root_maps = &fixture->target_map;
    fixture->mapped_method.root_map_count = 1u;
    fixture->leaf_binding = (StMethodBinding) {
        .descriptor = &fixture->leaf_method,
        .code = leaf_false,
        .version = 1u
    };
    fixture->rooted_binding = (StMethodBinding) {
        .descriptor = &fixture->rooted_method,
        .code = leaf_false,
        .version = 2u
    };
    fixture->mapped_binding = (StMethodBinding) {
        .descriptor = &fixture->mapped_method,
        .code = leaf_false,
        .version = 3u
    };
    if (!st_method_entry_init(&fixture->leaf_entry,
                              &fixture->leaf_binding)) return false;
    fixture->true_slots[0] = (st_method_slot_t) {
        SELECTOR_VALUE, &fixture->leaf_entry
    };
    for (uint32_t id = 1u; id < CLASS_COUNT; id++) {
        size_t index = (size_t)id - 1u;
        fixture->classes_storage[index] = (StClassDescriptor) {
            .class_id = id,
            .superclass_id = id == CLASS_OBJECT || id == CLASS_METACLASS
                ? 0u : CLASS_OBJECT,
            .metaclass_id = CLASS_METACLASS,
            .default_shape_id = id,
            .flags = id == CLASS_METACLASS ? ST_CLASS_METACLASS : 0u,
            .name = names[index],
            .name_length = strlen(names[index])
        };
        fixture->shapes_storage[index] = (StShapeDescriptor) {
            .shape_id = id,
            .class_id = id,
            .allocation_alignment = 8u,
            .minimum_allocation_size = 24u,
            .indexed_format = ST_INDEXED_NONE
        };
        fixture->classes[index] = &fixture->classes_storage[index];
        fixture->shapes[index] = &fixture->shapes_storage[index];
    }
    fixture->classes_storage[CLASS_TRUE - 1u].method_slots =
        fixture->true_slots;
    fixture->classes_storage[CLASS_TRUE - 1u].method_slot_count = 1u;
    fixture->descriptors = (st_runtime_descriptors_t) {
        .classes = fixture->classes,
        .class_count = CLASS_COUNT - 1u,
        .shapes = fixture->shapes,
        .shape_count = CLASS_COUNT - 1u
    };
    if (st_runtime_descriptors_validate(&fixture->descriptors)
            != ST_RUNTIME_OK
            || st_lookup_context_init(&fixture->lookup,
                                      &fixture->descriptors,
                                      (st_lookup_allocator_t){0})
                != ST_LOOKUP_FOUND
            || !st_aot_thread_init(&fixture->thread, &fixture->lookup,
                                   immediate_ids, NULL, NULL, NULL,
                                   object_class, fixture,
                                   failure_policy, fixture)
            || !st_send_site_init(&fixture->site, SELECTOR_VALUE, 0u))
        return false;
    fixture->frame = (StFrame) {
        .thread = &fixture->thread,
        .method = &fixture->caller_method,
        .receiver = st_value_true()
    };
    return true;
}

static void fixture_destroy(fixture_t *fixture)
{
    st_aot_thread_destroy(&fixture->thread);
    st_lookup_context_destroy(&fixture->lookup);
}

static void expect_child_abort(
    fixture_t *fixture, st_aot_send_status_t status,
    bool invalid_frame, bool bogus_result)
{
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        fixture->failure_must_not_run = invalid_frame
            || status != ST_AOT_SEND_ERR_NOT_FOUND;
        fixture->return_bogus_object = bogus_result;
        if (invalid_frame) fixture->frame.argc = 1u;
        (void)st_aot_send_failure(
            &fixture->frame, &fixture->site, st_value_true(), NULL, 0u,
            status);
        _exit(98);
    }
    int child_status = 0;
    CHECK(waitpid(pid, &child_status, 0) == pid);
    CHECK(WIFSIGNALED(child_status));
    CHECK(!WIFSIGNALED(child_status)
          || WTERMSIG(child_status) == SIGABRT);
}

static void test_resolve_validation_and_provenance(void)
{
    fixture_t fixture;
    st_aot_send_target_t target;
    CHECK(fixture_init(&fixture));
    if (!fixture.thread.initialized) return;

    CHECK(st_aot_frame_validate(&fixture.frame, 0u) == ST_AOT_SEND_OK);
    CHECK(st_aot_send_resolve(&fixture.frame, &fixture.site, st_value_true(),
                              0u, &target) == ST_AOT_SEND_OK);
    CHECK(target.code == leaf_false);
    CHECK(target.descriptor == &fixture.leaf_method);
    CHECK(target.code(&fixture.frame) == st_value_false());

    fixture.frame.argc = 1u;
    CHECK(st_aot_send_resolve(&fixture.frame, &fixture.site, st_value_true(),
                              0u, &target)
          == ST_AOT_SEND_ERR_INVALID_FRAME);
    CHECK(target.code == NULL && target.descriptor == NULL);
    fixture.frame.argc = 0u;
    fixture.caller_method.abi_version = 0u;
    CHECK(st_aot_send_resolve(&fixture.frame, &fixture.site, st_value_true(),
                              0u, &target)
          == ST_AOT_SEND_ERR_INVALID_FRAME);
    fixture.caller_method.abi_version = ST_METHOD_ABI_VERSION;

    CHECK(st_aot_send_resolve(&fixture.frame, &fixture.site, UINT64_C(0x1000),
                              0u, &target) == ST_AOT_SEND_ERR_NOT_FOUND);
    CHECK(st_aot_send_resolve(&fixture.frame, &fixture.site, UINT64_C(0x8),
                              0u, &target)
          == ST_AOT_SEND_ERR_INVALID_RECEIVER);

    CHECK(st_method_entry_publish(&fixture.leaf_entry,
                                  &fixture.rooted_binding, NULL));
    st_send_site_clear(&fixture.site);
    CHECK(st_aot_send_resolve(&fixture.frame, &fixture.site, st_value_true(),
                              0u, &target)
          == ST_AOT_SEND_OK);
    CHECK(target.frame_root_capacity == 1u);

    CHECK(st_method_entry_publish(&fixture.leaf_entry,
                                  &fixture.mapped_binding, NULL));
    st_send_site_clear(&fixture.site);
    CHECK(st_aot_send_resolve(&fixture.frame, &fixture.site, st_value_true(),
                              0u, &target)
          == ST_AOT_SEND_OK);
    CHECK(target.frame_root_capacity == 1u);

    StMethodDescriptor unwind_method = fixture.rooted_method;
    unwind_method.flags = ST_METHOD_CAN_UNWIND;
    StMethodBinding unwind_binding = {
        &unwind_method, leaf_false, 4u
    };
    CHECK(st_method_entry_publish(&fixture.leaf_entry,
                                  &unwind_binding, NULL));
    st_send_site_clear(&fixture.site);
    CHECK(st_aot_send_resolve(&fixture.frame, &fixture.site, st_value_true(),
                              0u, &target)
          == ST_AOT_SEND_ERR_UNSUPPORTED_TARGET_FRAME);

    st_value_t initialized[3] = {
        st_value_true(), st_value_false(),
        ((st_value_t)'X' << ST_VALUE_TAG_BITS) | ST_VALUE_TAG_CHARACTER
    };
    CHECK(st_aot_frame_roots_initialize(NULL, 0u) == ST_AOT_SEND_OK);
    CHECK(st_aot_frame_roots_initialize(initialized, 3u) == ST_AOT_SEND_OK);
    CHECK(initialized[0] == st_value_nil()
          && initialized[1] == st_value_nil()
          && initialized[2] == st_value_nil());
    CHECK(st_aot_frame_roots_initialize(
              initialized, ST_AOT_MAX_DYNAMIC_ROOTS + 1u)
          == ST_AOT_SEND_ERR_INVALID_ARGUMENT);
    CHECK(st_aot_frame_roots_initialize(NULL, 1u)
          == ST_AOT_SEND_ERR_INVALID_ARGUMENT);

    CHECK(st_aot_send_failure(&fixture.frame, &fixture.site, st_value_true(),
                              NULL, 0u, ST_AOT_SEND_ERR_NOT_FOUND)
          == st_value_false());
    CHECK(fixture.failure_calls == 1u);
    expect_child_abort(
        &fixture, ST_AOT_SEND_ERR_INVALID_FRAME, true, false);
    expect_child_abort(
        &fixture, ST_AOT_SEND_ERR_NOT_FOUND, false, true);
    expect_child_abort(
        &fixture, ST_AOT_SEND_ERR_LOOKUP, false, false);
    expect_child_abort(
        &fixture, ST_AOT_SEND_ERR_ARITY, false, false);
    expect_child_abort(
        &fixture, ST_AOT_SEND_ERR_INVALID_TARGET, false, false);
    expect_child_abort(
        &fixture, ST_AOT_SEND_ERR_UNSUPPORTED_TARGET_FRAME, false, false);
    fixture_destroy(&fixture);
}

static void test_root_map_contract_and_thread_configuration(void)
{
    fixture_t fixture;
    uint64_t bitmap = UINT64_C(1);
    st_root_map_t map = {
        .safepoint_id = 1u,
        .root_count = 1u,
        .bitmap_word_count = 1u,
        .live_root_bitmap = &bitmap
    };
    st_value_t roots[2] = { st_value_true(), st_value_nil() };
    uint32_t duplicate_ids[ST_AOT_IMMEDIATE_CLASS_COUNT] = {
        CLASS_NIL, CLASS_FALSE, CLASS_TRUE, CLASS_TRUE, CLASS_CHARACTER
    };
    st_aot_thread_t duplicate_thread = {0};
    CHECK(fixture_init(&fixture));
    if (!fixture.thread.initialized) return;
    fixture.caller_method.frame_root_capacity = 2u;
    fixture.caller_method.root_maps = &map;
    fixture.caller_method.root_map_count = 1u;
    fixture.frame.roots = roots;
    fixture.frame.root_count = 2u;
    fixture.frame.safepoint_id = 1u;
    CHECK(st_method_descriptor_is_valid(&fixture.caller_method));
    CHECK(st_aot_frame_validate(&fixture.frame, 2u) == ST_AOT_SEND_OK);
    fixture.frame.root_count = 1u;
    CHECK(st_aot_frame_validate(&fixture.frame, 1u)
          == ST_AOT_SEND_ERR_INVALID_FRAME);
    fixture.frame.root_count = 2u;
    fixture.frame.safepoint_id = 2u;
    CHECK(st_aot_frame_validate(&fixture.frame, 0u)
          == ST_AOT_SEND_ERR_INVALID_FRAME);
    CHECK(!st_aot_thread_init(&duplicate_thread, &fixture.lookup,
                              duplicate_ids, NULL, NULL, NULL,
                              object_class, &fixture,
                              failure_policy, &fixture));
    unsigned char image_storage = 0u;
    unsigned char stream_storage = 0u;
    unsigned char string_storage = 0u;
    st_image_runtime_t *image =
        (st_image_runtime_t *)(void *)&image_storage;
    st_stream_primitive_context_t *streams =
        (st_stream_primitive_context_t *)(void *)&stream_storage;
    st_string_primitive_context_t *strings =
        (st_string_primitive_context_t *)(void *)&string_storage;
    CHECK(st_aot_thread_image_attach(&fixture.thread, image));
    CHECK(!st_aot_thread_image_attach(&fixture.thread, image));
    CHECK(!st_aot_thread_image_detach(&fixture.thread,
          (const st_image_runtime_t *)(uintptr_t)8u));
    CHECK(st_aot_thread_image_detach(&fixture.thread, image));
    CHECK(st_aot_thread_streams_attach(&fixture.thread, streams));
    CHECK(!st_aot_thread_streams_attach(&fixture.thread, streams));
    CHECK(!st_aot_thread_streams_detach(
          &fixture.thread,
          (const st_stream_primitive_context_t *)(uintptr_t)8u));
    CHECK(st_aot_thread_streams_detach(&fixture.thread, streams));
    CHECK(st_aot_thread_strings_attach(&fixture.thread, strings));
    CHECK(!st_aot_thread_strings_attach(&fixture.thread, strings));
    CHECK(!st_aot_thread_strings_detach(
          &fixture.thread,
          (const st_string_primitive_context_t *)(uintptr_t)8u));
    CHECK(st_aot_thread_strings_detach(&fixture.thread, strings));
    fixture_destroy(&fixture);
}

int main(void)
{
    test_resolve_validation_and_provenance();
    test_root_map_contract_and_thread_configuration();
    if (failures != 0u) {
        fprintf(stderr, "smalltalk AOT send bridge: %u failure(s)\n",
                failures);
        return 1;
    }
    puts("smalltalk AOT send bridge: ok");
    return 0;
}
