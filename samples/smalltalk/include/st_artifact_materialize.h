#ifndef ANVIL_SMALLTALK_ARTIFACT_MATERIALIZE_H
#define ANVIL_SMALLTALK_ARTIFACT_MATERIALIZE_H

#include "st_artifact_bundle.h"

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_ARTIFACT_PROFILE_NAME_MAX 96u

typedef enum {
    ST_ARTIFACT_MATERIALIZE_OK = 0,
    ST_ARTIFACT_MATERIALIZE_ERR_INVALID_ARGUMENT,
    ST_ARTIFACT_MATERIALIZE_ERR_INVALID_BUNDLE,
    ST_ARTIFACT_MATERIALIZE_ERR_UNSUPPORTED_PLATFORM,
    ST_ARTIFACT_MATERIALIZE_ERR_COLLISION,
    ST_ARTIFACT_MATERIALIZE_ERR_OUT_OF_MEMORY,
    ST_ARTIFACT_MATERIALIZE_ERR_IO,
    ST_ARTIFACT_MATERIALIZE_ERR_DURABILITY
} st_artifact_materialize_status_t;

/* A write seam is exposed so callers with unusual I/O layers can retain the
 * materializer's exact-write semantics.  NULL selects write(2). */
typedef ssize_t (*st_artifact_materialize_write_fn)(
    void *user, int file_descriptor, const void *bytes, size_t length);

typedef struct {
    st_artifact_allocator_t allocator;
    st_artifact_materialize_write_fn write;
    void *write_user;
} st_artifact_materialize_options_t;

typedef struct {
    st_artifact_materialize_status_t status;
    int system_error;
    /* True means the complete profile was atomically made visible.  A
     * durability error may be reported after this point if the containing
     * directory could not be synchronized. */
    int committed;
    char profile[ST_ARTIFACT_PROFILE_NAME_MAX];
} st_artifact_materialize_result_t;

void st_artifact_materialize_result_init(
    st_artifact_materialize_result_t *result);

/* Materializes exactly one canonical target/ABI/syntax/optimization profile.
 * output_directory must already exist, must contain no `..` component and no
 * path component may be a symbolic link.  Publication is all-or-nothing and
 * never replaces an existing profile. */
st_artifact_materialize_status_t st_artifact_bundle_materialize(
    st_artifact_materialize_result_t *result,
    const st_artifact_bundle_t *bundle,
    const char *output_directory,
    const st_artifact_materialize_options_t *options);

/* Descriptor-relative form used by higher-level transactional publishers.
 * output_directory_fd is borrowed, must name an open directory and remains
 * open on return. The same validation, exact writes, fsyncs and atomic
 * no-replace profile publication used by the path API are preserved. */
st_artifact_materialize_status_t st_artifact_bundle_materialize_at(
    st_artifact_materialize_result_t *result,
    const st_artifact_bundle_t *bundle,
    int output_directory_fd,
    const st_artifact_materialize_options_t *options);

const char *st_artifact_materialize_status_string(
    st_artifact_materialize_status_t status);

#ifdef __cplusplus
}
#endif

#endif
