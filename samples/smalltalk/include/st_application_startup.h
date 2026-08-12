#ifndef ANVIL_SMALLTALK_APPLICATION_STARTUP_H
#define ANVIL_SMALLTALK_APPLICATION_STARTUP_H

#include "st_application_launch.h"
#include "st_stream_primitives.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST_APPLICATION_STARTUP_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_APPLICATION_STARTUP_OK = 0,
    ST_APPLICATION_STARTUP_ERR_INVALID_ARGUMENT,
    ST_APPLICATION_STARTUP_ERR_INVALID_LAUNCH,
    ST_APPLICATION_STARTUP_ERR_INVALID_METADATA,
    ST_APPLICATION_STARTUP_ERR_OUT_OF_MEMORY,
    ST_APPLICATION_STARTUP_ERR_IMAGE,
    ST_APPLICATION_STARTUP_ERR_LOOKUP,
    ST_APPLICATION_STARTUP_ERR_SYMBOLS,
    ST_APPLICATION_STARTUP_ERR_BOOTSTRAP,
    ST_APPLICATION_STARTUP_ERR_PRIMITIVES,
    ST_APPLICATION_STARTUP_ERR_CONTROL,
    ST_APPLICATION_STARTUP_ERR_CLOSURES,
    ST_APPLICATION_STARTUP_ERR_THREAD,
    ST_APPLICATION_STARTUP_ERR_DNU,
    ST_APPLICATION_STARTUP_ERR_ENTRY,
    ST_APPLICATION_STARTUP_ERR_UNHANDLED_EXCEPTION,
    ST_APPLICATION_STARTUP_ERR_ESCAPED_CONTROL,
    ST_APPLICATION_STARTUP_ERR_INVALID_RESULT,
    ST_APPLICATION_STARTUP_ERR_BUSY,
    ST_APPLICATION_STARTUP_ERR_CLEANUP
} st_application_startup_status_t;

typedef void *(*st_application_startup_allocate_fn)(void *user, size_t size);
typedef void (*st_application_startup_deallocate_fn)(
    void *user, void *pointer);

typedef struct {
    st_application_startup_allocate_fn allocate;
    st_application_startup_deallocate_fn deallocate;
    void *user;
} st_application_startup_allocator_t;

typedef struct {
    const st_application_launch_descriptor_t *launch;

    /* NULL selects the runtime's checked write(2) implementation. */
    st_stream_write_fn write_bytes;
    void *write_user;

    st_runtime_allocator_t heap_allocator;
    st_application_startup_allocator_t allocator;
} st_application_startup_options_t;

typedef struct st_application_startup_state st_application_startup_state_t;
typedef struct {
    uint32_t abi_version;
    bool initialized;
    bool running;
    st_application_startup_status_t status;
    uint32_t detail;
    st_application_startup_state_t *state;
} st_application_startup_context_t;

/* Authenticates the target-native launch/metadata pair, creates the image
 * heap and every required runtime sidecar, bootstraps all String literals and
 * the Transcript stdout stream, and installs the real DNU policy. The launch
 * descriptor and all linked AOT metadata/code are borrowed image-lifetime
 * storage. Initialization is transactional. */
st_application_startup_status_t st_application_startup_context_init(
    st_application_startup_context_t *context,
    const st_application_startup_options_t *options);

/* Allocates a real entry-class receiver, resolves the configured selector via
 * the immutable method tables, builds the exact root frame and invokes AOT
 * code. An unhandled Smalltalk exception and an escaped non-local return are
 * distinct failures. On every failure result_out remains ST_VALUE_INVALID. */
st_application_startup_status_t st_application_startup_run(
    st_application_startup_context_t *context, st_value_t *result_out);

/* Converts the conventional application result to a process exit code. Only
 * a non-negative SmallInteger in [0,255] is accepted. */
st_application_startup_status_t st_application_startup_exit_code(
    st_value_t result, int *exit_code_out);

/* Destroys a quiescent context. A final precise collection releases closure
 * home tokens before their sidecars disappear. Cleanup failures are reported
 * and never hidden behind a successful return. */
st_application_startup_status_t st_application_startup_context_destroy(
    st_application_startup_context_t *context);

const char *st_application_startup_status_string(
    st_application_startup_status_t status);

#ifdef __cplusplus
}
#endif

#endif
