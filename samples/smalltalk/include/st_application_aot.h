#ifndef ANVIL_SMALLTALK_APPLICATION_AOT_H
#define ANVIL_SMALLTALK_APPLICATION_AOT_H

#include "st_artifact_bundle.h"
#include "st_application_launch.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_APPLICATION_AOT_MATRIX_VERSION UINT32_C(1)
#define ST_APPLICATION_AOT_PROFILE_COUNT 11u
#define ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT 6u
#define ST_APPLICATION_AOT_DIAGNOSTIC_CAPACITY 512u

typedef enum {
    ST_APPLICATION_AOT_OK = 0,
    ST_APPLICATION_AOT_ERR_INVALID_ARGUMENT,
    ST_APPLICATION_AOT_ERR_OUT_OF_MEMORY,
    ST_APPLICATION_AOT_ERR_OVERFLOW,
    ST_APPLICATION_AOT_ERR_SOURCE,
    ST_APPLICATION_AOT_ERR_CLASS_GRAPH,
    ST_APPLICATION_AOT_ERR_SELECTORS,
    ST_APPLICATION_AOT_ERR_PRIMITIVES,
    ST_APPLICATION_AOT_ERR_ROLE,
    ST_APPLICATION_AOT_ERR_COMPILE,
    ST_APPLICATION_AOT_ERR_LAUNCH,
    ST_APPLICATION_AOT_ERR_ARTIFACT
} st_application_aot_status_t;

typedef enum {
    ST_APPLICATION_PROFILE_READY = 0,
    ST_APPLICATION_PROFILE_UNSUPPORTED
} st_application_profile_state_t;

typedef enum {
    ST_APPLICATION_AOT_STAGE_NONE = 0,
    ST_APPLICATION_AOT_STAGE_SOURCE,
    ST_APPLICATION_AOT_STAGE_CLASS_GRAPH,
    ST_APPLICATION_AOT_STAGE_SELECTORS,
    ST_APPLICATION_AOT_STAGE_PRIMITIVES,
    ST_APPLICATION_AOT_STAGE_ROLES,
    ST_APPLICATION_AOT_STAGE_COMPILE,
    ST_APPLICATION_AOT_STAGE_LAUNCH,
    ST_APPLICATION_AOT_STAGE_ARTIFACT,
    ST_APPLICATION_AOT_STAGE_MATRIX
} st_application_aot_stage_t;

typedef void *(*st_application_aot_allocate_fn)(void *user, size_t size);
typedef void (*st_application_aot_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_application_aot_allocate_fn allocate;
    st_application_aot_deallocate_fn deallocate;
    void *user;
} st_application_aot_allocator_t;

typedef struct {
    const char *image_directory;
    const char *application_directory;
    const char *application_name;
    const char *entry_class_name;
    const char *entry_selector;
    anvil_opt_level_t optimization;
    st_application_aot_allocator_t allocator;
} st_application_aot_options_t;

typedef struct {
    anvil_arch_t target;
    anvil_abi_t abi;
    anvil_syntax_t syntax;
    anvil_opt_level_t optimization;
    st_application_profile_state_t state;
    const char *reason;
    st_artifact_bundle_t bundle;
} st_application_aot_profile_t;

typedef struct {
    st_application_aot_status_t status;
    st_application_aot_stage_t failed_stage;
    anvil_arch_t failed_target;
    st_source_load_status_t source_status;
    st_class_graph_status_t graph_status;
    st_selector_status_t selector_status;
    st_primitive_status_t primitive_status;
    st_aot_compile_status_t compile_status;
    st_application_launch_status_t launch_status;
    st_artifact_bundle_status_t artifact_status;
    char diagnostic[ST_APPLICATION_AOT_DIAGNOSTIC_CAPACITY];

    st_application_aot_profile_t profiles[ST_APPLICATION_AOT_PROFILE_COUNT];
    size_t profile_count;
    char *matrix_manifest;
    size_t matrix_manifest_length;
    st_application_aot_allocator_t allocator;
} st_application_aot_result_t;

void st_application_aot_result_init(st_application_aot_result_t *result);
void st_application_aot_result_destroy(st_application_aot_result_t *result);

/*
 * Loads st-image/manifest.txt followed by application.manifest, builds one
 * canonical selector snapshot and compiles the complete image/application
 * program for the five tagged-64 profiles.  The other five Anvil targets are
 * represented explicitly as unsupported tagged32 profiles and never receive
 * empty or synthetic artifacts.  Every ready profile owns copied assembly,
 * metadata, launch data and its authenticated bundle manifest.
 *
 * Publication is transactional: on failure no ready profile or matrix is
 * returned.  The fixed diagnostic fields remain available until destroy.
 */
st_application_aot_status_t st_application_aot_compile(
    st_application_aot_result_t *result,
    const st_application_aot_options_t *options);

const char *st_application_aot_status_string(
    st_application_aot_status_t status);
const char *st_application_aot_stage_string(st_application_aot_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif
