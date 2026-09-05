#include "st_fiber.h"
#include "st_closure_bridge.h"
#include "st_control_bridge.h"
#include "st_image_runtime.h"
#include "st_socket.h"

#include <errno.h>
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "fiber test: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

enum { OBJECT = 1, BLOCK, CELL, NIL, FALSE_CLASS, TRUE_CLASS, INTEGER, CHARACTER, BYTES, META, CLASS_COUNT = META };

typedef struct {
    StClassDescriptor classes[CLASS_COUNT];
    StShapeDescriptor shapes[CLASS_COUNT];
    const StClassDescriptor *class_pointers[CLASS_COUNT];
    const StShapeDescriptor *shape_pointers[CLASS_COUNT];
    st_runtime_descriptors_t descriptors;
    st_heap_t heap;
    st_image_runtime_t image;
    st_lookup_context_t lookup;
    st_control_thread_t control;
    st_aot_thread_t thread;
    st_aot_closure_context_t closures;
    st_fiber_context_t *fibers;
    st_socket_context_t *sockets;
    st_aot_block_descriptor_t blocks[4];
    const st_aot_block_descriptor_t *block_pointers[4];
    st_value_t failed_exception;
    st_value_t sleeping_object;
    st_value_t ensure_object;
    unsigned steps;
    unsigned ensures;
} fixture_t;

static fixture_t fixture;
static const uint64_t live_roots = 7u;
static const uint64_t no_fixed_pointers = 0u;
static const uint64_t cell_pointers = 1u;
static const st_root_map_t block_map = {1u, 3u, 1u, &live_roots};
static const st_root_map_t main_map = {.safepoint_id = 1u};
static const st_aot_capture_descriptor_t socket_capture = {.kind = ST_AOT_CAPTURE_VALUE};
static const StMethodDescriptor block_method = {
    .abi_version = ST_METHOD_ABI_VERSION, .selector_id = 1u, .owner_class_id = BLOCK, .frame_root_capacity = 3u, .flags = ST_METHOD_CAN_UNWIND, .root_maps = &block_map, .root_map_count = 1u};
static const StMethodDescriptor main_method = {
    .abi_version = ST_METHOD_ABI_VERSION, .selector_id = 1u, .owner_class_id = OBJECT, .flags = ST_METHOD_CAN_UNWIND, .root_maps = &main_map, .root_map_count = 1u};

static void ensure_alive(void *user, StFrame *frame, st_control_thread_t *control)
{
    st_value_t *value = user;

    require(control == ((st_aot_thread_t *)frame->thread)->control, "ensure control belongs to fiber");
    require(st_heap_contains(&fixture.heap, *value), "ensure-only root survives another fiber's collection");
    fixture.ensures++;
}

static st_value_t worker(StFrame *frame)
{
    frame->safepoint_id = 1u;

    st_control_scope_t scope = {0};
    st_control_ensure_t ensure = {0};
    st_value_t ensure_root;
    st_value_t result;
    st_aot_thread_t *thread = frame->thread;

    require(thread != &fixture.thread && thread->control != &fixture.control, "independent logical thread");
    require(st_aot_control_scope_enter(frame, &scope, 0u) == ST_CONTROL_OK, "enter worker scope");
    require(st_heap_allocate(&fixture.heap, OBJECT, OBJECT, 0u, 0u, 0u, &frame->roots[1]) == ST_HEAP_OK, "allocate shadow root");
    require(st_heap_allocate(&fixture.heap, OBJECT, OBJECT, 0u, 0u, 0u, &ensure_root) == ST_HEAP_OK, "allocate ensure root");
    fixture.sleeping_object = frame->roots[1];
    fixture.ensure_object = ensure_root;

    st_control_ensure_init(&ensure);

    require(st_control_ensure_push_with_roots(thread->control, &scope, &ensure, ensure_alive, &ensure_root, &ensure_root, 1u) == ST_CONTROL_OK, "register precise ensure roots");
    require(fesetround(FE_DOWNWARD) == 0, "worker rounding mode");
    fixture.steps++;

    for (unsigned index = 0u; index < 1000u; index++) {
        volatile double preserved = 3.25 + (double)index;

        errno = EDOM;
        require(st_fiber_yield(frame) == ST_FIBER_OK, "yield worker");
        require(errno == EDOM, "errno survives context transfer");
        require(fegetround() == FE_DOWNWARD, "FP control survives context transfer");
        require(preserved == 3.25 + (double)index, "stack locals survive context transfer");
        require(st_heap_contains(&fixture.heap, frame->roots[1]), "shadow root remains live after resume");
    }

    require(st_fiber_sleep(frame, 2u) == ST_FIBER_OK, "timer wakes worker");
    fixture.steps++;
    require(st_aot_control_scope_leave(frame, &scope, st_value_true(), &result) == ST_CONTROL_OK, "worker ensure runs on exit");
    return result;
}

static st_value_t collector(StFrame *frame)
{
    frame->safepoint_id = 1u;

    st_control_scope_t scope = {0};
    st_value_t result;

    require(st_aot_control_scope_enter(frame, &scope, 0u) == ST_CONTROL_OK, "enter collector scope");
    require(fixture.steps == 1u, "FIFO starts worker before collector");
    require(fesetround(FE_UPWARD) == 0, "collector rounding mode");

    for (unsigned index = 0u; index < 1000u; index++) {
        st_heap_collection_stats_t stats;
        st_value_t garbage;

        require(st_heap_allocate(&fixture.heap, OBJECT, OBJECT, 0u, 0u, 0u, &garbage) == ST_HEAP_OK, "allocate unreachable garbage");
        require(st_image_runtime_collect(&fixture.image, frame, &stats) == ST_IMAGE_RUNTIME_OK, "collect while worker suspended");
        require(stats.reclaimed_objects == 1u && !st_heap_contains(&fixture.heap, garbage), "collector really reclaims garbage");
        require(st_heap_contains(&fixture.heap, fixture.sleeping_object), "suspended shadow root marked");
        require(st_heap_contains(&fixture.heap, fixture.ensure_object), "suspended control root marked");
        errno = ERANGE;
        require(st_fiber_yield(frame) == ST_FIBER_OK, "yield collector");
        require(errno == ERANGE && fegetround() == FE_UPWARD, "collector machine state isolated");
    }

    require(st_aot_control_scope_leave(frame, &scope, st_value_false(), &result) == ST_CONTROL_OK, "leave collector scope");
    return result;
}

static st_value_t echo_connection(StFrame *frame)
{
    frame->safepoint_id = 1u;

    st_control_scope_t scope = {0};
    st_value_t captured;
    st_value_t result;
    int64_t handle;
    int error;
    size_t received = 0u;

    require(st_aot_control_scope_enter(frame, &scope, 0u) == ST_CONTROL_OK, "enter echo scope");
    require(st_aot_closure_capture_load(frame, frame->receiver, 0u, &captured) == ST_AOT_CLOSURE_OK, "load connection handle");
    require(st_value_to_small_integer(captured, &handle), "decode connection handle");
    require(st_heap_allocate(&fixture.heap, BYTES, BYTES, 257u, 257u, 0u, &frame->roots[1]) == ST_HEAP_OK, "allocate receive buffer");

    while (received < 257u) {
        size_t count;

        require(st_socket_receive(frame, (uint64_t)handle, frame->roots[1], received, 257u - received, 15000u, &count, &error) == ST_SOCKET_OK, "receive through native event reactor");
        require(count != 0u, "peer sends complete payload");
        received += count;
    }

    st_heap_collection_stats_t stats;

    require(st_image_runtime_collect(&fixture.image, frame, &stats) == ST_IMAGE_RUNTIME_OK, "GC with overlapped buffers in suspended fibers");

    size_t sent = 0u;

    while (sent < received) {
        size_t count;

        require(st_socket_send(frame, (uint64_t)handle, frame->roots[1], sent, received - sent, 15000u, &count, &error) == ST_SOCKET_OK, "send through native event reactor");
        require(count != 0u, "send makes progress");
        sent += count;
    }

    size_t count;

    require(st_socket_receive(frame, (uint64_t)handle, frame->roots[1], 0u, 1u, 2u, &count, &error) == ST_SOCKET_TIMEOUT, "timeout drains pending receive before releasing buffer");
    require(st_socket_close(frame, (uint64_t)handle, &error) == ST_SOCKET_OK, "close after cancellation completion");
    require(st_socket_close(frame, (uint64_t)handle, &error) == ST_SOCKET_INVALID_HANDLE, "reject stale socket handle");
    require(st_aot_control_scope_leave(frame, &scope, st_value_true(), &result) == ST_CONTROL_OK, "leave echo scope");
    return result;
}

static bool match_exception(void *user, uint32_t raised, uint32_t caught)
{
    (void)user;
    return raised == caught;
}

static st_value_t fail_detached(StFrame *frame)
{
    st_control_scope_t scope = {0};
    st_value_t result;
    st_aot_thread_t *thread = frame->thread;

    frame->safepoint_id = 1u;
    require(st_aot_control_scope_enter(frame, &scope, 0u) == ST_CONTROL_OK, "enter failing fiber scope");
    require(st_heap_allocate(&fixture.heap, OBJECT, OBJECT, 0u, 0u, 0u, &frame->roots[1]) == ST_HEAP_OK, "allocate exception object");
    fixture.failed_exception = frame->roots[1];
    require(st_control_exception_signal(thread->control, frame->roots[1], OBJECT, match_exception, NULL) == ST_CONTROL_OK, "raise detached exception");
    require(st_aot_control_scope_leave(frame, &scope, st_value_nil(), &result) == ST_CONTROL_OK, "unwind failing fiber scope");
    return result;
}

static void initialize(void)
{
    for (uint32_t index = 0u; index < CLASS_COUNT; index++) {
        uint32_t id = index + 1u;

        fixture.classes[index] = (StClassDescriptor){.class_id = id,
                                                     .superclass_id = id == OBJECT || id == META ? 0u : OBJECT,
                                                     .metaclass_id = META,
                                                     .default_shape_id = id,
                                                     .flags = id == META                  ? ST_CLASS_METACLASS
                                                              : id == BLOCK || id == CELL ? ST_CLASS_ABSTRACT
                                                                                          : 0u,
                                                     .name = "Test",
                                                     .name_length = 4u};
        fixture.shapes[index] = (StShapeDescriptor){.shape_id = id, .class_id = id, .allocation_alignment = 8u, .minimum_allocation_size = 24u};
        fixture.class_pointers[index] = &fixture.classes[index];
        fixture.shape_pointers[index] = &fixture.shapes[index];
    }

    fixture.shapes[BLOCK - 1u] = (StShapeDescriptor){BLOCK, BLOCK, 8u, 56u, 4u, ST_INDEXED_VALUES, &no_fixed_pointers, 1u};
    fixture.shapes[CELL - 1u] = (StShapeDescriptor){CELL, CELL, 8u, 32u, 1u, ST_INDEXED_NONE, &cell_pointers, 1u};
    fixture.shapes[BYTES - 1u].indexed_format = ST_INDEXED_UINT8;
    fixture.descriptors = (st_runtime_descriptors_t){fixture.class_pointers, CLASS_COUNT, fixture.shape_pointers, CLASS_COUNT};

    require(st_heap_init(&fixture.heap, &fixture.descriptors, (st_runtime_allocator_t){0}) == ST_HEAP_OK, "initialize heap");
    require(st_image_runtime_init(&fixture.image, &(st_image_runtime_options_t){.descriptors = &fixture.descriptors, .borrowed_heap = &fixture.heap}) == ST_IMAGE_RUNTIME_OK, "initialize image");
    require(st_lookup_context_init(&fixture.lookup, &fixture.descriptors, (st_lookup_allocator_t){0}) == ST_LOOKUP_FOUND, "initialize lookup");
    require(st_control_thread_init(&fixture.control, &fixture.thread, (st_control_allocator_t){0}) == ST_CONTROL_OK, "initialize control");

    const st_method_code_t codes[] = {worker, collector, echo_connection, fail_detached};

    for (size_t index = 0u; index < 4u; index++) {
        fixture.blocks[index] = (st_aot_block_descriptor_t){.abi_version = ST_AOT_BLOCK_ABI_VERSION, .code = codes[index], .method = &block_method};
        fixture.block_pointers[index] = &fixture.blocks[index];
    }

    fixture.blocks[2].capture_count = 1u;
    fixture.blocks[2].capture_descriptor_count = 1u;
    fixture.blocks[2].captures = &socket_capture;

    require(st_aot_closure_context_init(&fixture.closures, &(st_aot_closure_options_t){.heap = &fixture.heap,
                                                                                       .closure_class_id = BLOCK,
                                                                                       .closure_shape_id = BLOCK,
                                                                                       .cell_class_id = CELL,
                                                                                       .cell_shape_id = CELL,
                                                                                       .descriptors = fixture.block_pointers,
                                                                                       .descriptor_count = 4u}) == ST_AOT_CLOSURE_OK,
            "initialize closures");

    const uint32_t immediates[] = {NIL, FALSE_CLASS, TRUE_CLASS, INTEGER, CHARACTER};

    require(st_aot_thread_init(&fixture.thread, &fixture.lookup, immediates, NULL, &fixture.control, &fixture.closures, NULL, NULL, NULL, NULL), "initialize AOT thread");
    require(st_aot_thread_image_attach(&fixture.thread, &fixture.image), "attach image");
    require(st_fiber_context_create(&fixture.thread, 1024u * 1024u, &fixture.fibers) == ST_FIBER_OK, "initialize scheduler");
    require(st_socket_context_create(&fixture.thread, &fixture.sockets) == ST_SOCKET_OK, "initialize sockets");
}

static void network_test(StFrame *frame, size_t clients)
{
    uint64_t listener;
    uint16_t port;
    int error;
    uint64_t *ids = calloc(clients, sizeof(*ids));

    require(ids != NULL, "allocate requested client workload");

    require(st_socket_listen(frame, 0u, &listener, &error) == ST_SOCKET_OK, "listen on an ephemeral loopback port");
    require(st_socket_port(frame, listener, &port, &error) == ST_SOCKET_OK && port != 0u, "query selected port");
    printf("PORT %u\n", (unsigned)port);
    fflush(stdout);

    for (size_t index = 0u; index < clients; index++) {
        uint64_t connection;
        st_value_t capture;
        st_value_t block;

        require(st_socket_accept(frame, listener, 15000u, &connection, &error) == ST_SOCKET_OK, "accept concurrent client");
        require(st_value_from_small_integer((int64_t)connection, &capture), "encode opaque socket handle");
        require(st_aot_closure_create(frame, &fixture.blocks[2], frame->receiver, &capture, 1u, &block) == ST_AOT_CLOSURE_OK, "create connection fiber block");
        require(st_fiber_spawn(frame, block, &ids[index]) == ST_FIBER_OK, "spawn connection fiber");
    }

    require(st_socket_close(frame, listener, &error) == ST_SOCKET_OK, "close listener");
    require(st_fiber_run(frame) == ST_FIBER_OK, "complete concurrent connections");

    for (size_t index = 0u; index < clients; index++) {
        st_value_t result;

        require(st_fiber_join(frame, ids[index], &result) == ST_FIBER_OK && result == st_value_true(), "join connection fiber");
    }

    free(ids);
    printf("Smalltalk sockets: PASS (%zu concurrent clients, fragmented echo, precise GC, cancellation, stale handles)\n", clients);
}

int main(int argc, char **argv)
{
    initialize();

    StFrame frame = {.thread = &fixture.thread, .method = &main_method, .receiver = st_value_nil(), .safepoint_id = 1u};
    st_control_scope_t scope = {0};
    uint64_t ids[2];

    require(st_aot_control_scope_enter(&frame, &scope, 0u) == ST_CONTROL_OK, "enter main scope");

    bool network = argc == 3 && strcmp(argv[1], "--network") == 0;

    if (network) {
        char *end;
        errno = 0;
        unsigned long long clients = strtoull(argv[2], &end, 10);

        require(argv[2][0] >= '0' && argv[2][0] <= '9' && *end == '\0'
                    && errno == 0 && clients != 0u && clients <= SIZE_MAX / sizeof(uint64_t), "valid client workload");
        network_test(&frame, (size_t)clients);
    } else {
        for (size_t index = 0u; index < 2u; index++) {
            st_value_t block;

            require(st_aot_closure_create(&frame, &fixture.blocks[index], frame.receiver, NULL, 0u, &block) == ST_AOT_CLOSURE_OK, "create actual managed block");
            require(st_fiber_spawn(&frame, block, &ids[index]) == ST_FIBER_OK, "spawn managed block");
        }

        require(st_fiber_context_destroy(fixture.fibers) == ST_FIBER_BUSY, "refuse to destroy live stacks");
        require(st_fiber_run(&frame) == ST_FIBER_OK, "run all fibers");
        require(fixture.steps == 2u && fixture.ensures == 1u, "worker completed and ensure ran exactly once");

        st_value_t result;

        require(st_fiber_join(&frame, ids[0], &result) == ST_FIBER_OK && result == st_value_true(), "join worker result");
        require(st_fiber_join(&frame, ids[0], &result) == ST_FIBER_NOT_FOUND, "result consumed once");
        require(st_fiber_join(&frame, ids[1], &result) == ST_FIBER_OK && result == st_value_false(), "join collector result");
        require(st_fiber_join(&frame, UINT32_MAX, &result) == ST_FIBER_NOT_FOUND, "reject unknown handle");

        st_value_t block;
        uint64_t detached;
        size_t reclaimed;
        st_control_pending_info_t pending;
        st_control_handler_t handler = {0};

        st_control_handler_init(&handler);
        require(st_control_handler_push(&fixture.control, &scope, &handler, OBJECT, NULL, 0u) == ST_CONTROL_OK, "install detached failure handler");

        require(st_aot_closure_create(&frame, &fixture.blocks[3], frame.receiver, NULL, 0u, &block) == ST_AOT_CLOSURE_OK, "create failing managed block");
        require(st_fiber_spawn(&frame, block, &detached) == ST_FIBER_OK, "spawn failing fiber");
        require(st_fiber_detach(&frame, detached) == ST_FIBER_OK, "detach failing fiber");
        require(st_fiber_yield(&frame) == ST_FIBER_OK, "execute failing fiber");
        require(st_fiber_collect(&frame, &reclaimed) == ST_FIBER_EXCEPTION, "report detached failure at maintenance safepoint");
        require(st_control_pending_get(&fixture.control, &pending) == ST_CONTROL_OK
                    && pending.kind == ST_CONTROL_PENDING_EXCEPTION && pending.value == fixture.failed_exception, "preserve actual detached exception");
        require(st_heap_contains(&fixture.heap, pending.value), "exception remains rooted until delivery");
        require(st_control_handler_consume_exception(&fixture.control, &handler, &result) == ST_CONTROL_OK, "handle detached failure");
        require(st_fiber_collect(&frame, &reclaimed) == ST_FIBER_OK, "reap consumed fibers");
        require(!st_heap_contains(&fixture.heap, fixture.failed_exception), "release handled exception");
    }

    st_value_t result;

    require(st_aot_control_scope_leave(&frame, &scope, st_value_nil(), &result) == ST_CONTROL_OK, "leave main scope");
    require(st_socket_context_destroy(fixture.sockets) == ST_SOCKET_OK, "release socket table");
    require(st_fiber_context_destroy(fixture.fibers) == ST_FIBER_OK, "destroy scheduler and release dead closures");
    require(st_aot_closure_context_destroy(&fixture.closures) == ST_AOT_CLOSURE_OK, "no retained closures");
    require(st_control_thread_destroy(&fixture.control) == ST_CONTROL_OK, "no leaked home tokens");
    st_aot_thread_destroy(&fixture.thread);
    st_lookup_context_destroy(&fixture.lookup);
    st_image_runtime_destroy(&fixture.image);
    st_heap_destroy(&fixture.heap);
    if (!network)
        puts("Smalltalk fibers: PASS (2000 yields, precise suspended GC, ensure roots, FP/errno, timers, joins, detached errors, cleanup)");
    return EXIT_SUCCESS;
}
