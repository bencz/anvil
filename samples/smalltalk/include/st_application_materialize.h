#ifndef ANVIL_SMALLTALK_APPLICATION_MATERIALIZE_H
#define ANVIL_SMALLTALK_APPLICATION_MATERIALIZE_H

#include "st_application_aot.h"
#include "st_artifact_materialize.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_APPLICATION_MATERIALIZE_ABI_VERSION UINT32_C(1)
#define ST_APPLICATION_PUBLICATION_NAME_MAX 64u

typedef enum {
    ST_APPLICATION_MATERIALIZE_OK = 0,
    ST_APPLICATION_MATERIALIZE_ERR_INVALID_ARGUMENT,
    ST_APPLICATION_MATERIALIZE_ERR_INVALID_RESULT,
    ST_APPLICATION_MATERIALIZE_ERR_UNSUPPORTED_PLATFORM,
    ST_APPLICATION_MATERIALIZE_ERR_COLLISION,
    ST_APPLICATION_MATERIALIZE_ERR_PROFILE,
    ST_APPLICATION_MATERIALIZE_ERR_IO,
    ST_APPLICATION_MATERIALIZE_ERR_DURABILITY
} st_application_materialize_status_t;

typedef struct {
    st_artifact_materialize_options_t artifact_options;
} st_application_materialize_options_t;

typedef struct {
    st_application_materialize_status_t status;
    st_artifact_materialize_status_t profile_status;
    size_t failed_profile_index;
    int system_error;
    bool committed;
    char application[ST_APPLICATION_PUBLICATION_NAME_MAX + 1u];
    char profile[ST_ARTIFACT_PROFILE_NAME_MAX];
} st_application_materialize_result_t;

void st_application_materialize_result_init(
    st_application_materialize_result_t *result);

/* Publishes the complete five-profile assembly matrix and matrix.manifest as
 * one directory. output_directory must already exist and every component is
 * opened without following symbolic links. A private sibling staging tree is
 * used; no profile is externally visible unless the whole application can be
 * atomically installed without replacement. Unsupported narrow targets exist
 * only as authenticated matrix records and never receive empty directories. */
st_application_materialize_status_t st_application_aot_materialize(
    st_application_materialize_result_t *result,
    const st_application_aot_result_t *application,
    const char *application_name,
    const char *output_directory,
    const st_application_materialize_options_t *options);

const char *st_application_materialize_status_string(
    st_application_materialize_status_t status);

#ifdef __cplusplus
}
#endif

#endif
