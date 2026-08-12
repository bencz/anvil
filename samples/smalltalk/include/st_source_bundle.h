#ifndef ANVIL_SMALLTALK_SOURCE_BUNDLE_H
#define ANVIL_SMALLTALK_SOURCE_BUNDLE_H

#include "st_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ST_IMAGE_MANIFEST_FILENAME "manifest.txt"
#define ST_APPLICATION_MANIFEST_FILENAME "application.manifest"
#define ST_SOURCE_DIAGNOSTIC_PATH_CAPACITY 512u
#define ST_SOURCE_DEFAULT_MAX_MANIFEST_BYTES (4u * 1024u * 1024u)
#define ST_SOURCE_DEFAULT_MAX_FILE_BYTES (64u * 1024u * 1024u)
#define ST_SOURCE_DEFAULT_MAX_TOTAL_BYTES (512u * 1024u * 1024u)
#define ST_SOURCE_DEFAULT_MAX_FILES 65536u

typedef enum {
    ST_SOURCE_ORIGIN_IMAGE = 0,
    ST_SOURCE_ORIGIN_APPLICATION
} st_source_origin_t;

typedef enum {
    ST_SOURCE_LOAD_OK = 0,
    ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT,
    ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY,
    ST_SOURCE_LOAD_ERR_OVERFLOW,
    ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED,
    ST_SOURCE_LOAD_ERR_IO,
    ST_SOURCE_LOAD_ERR_MISSING_FILE,
    ST_SOURCE_LOAD_ERR_NOT_REGULAR_FILE,
    ST_SOURCE_LOAD_ERR_INVALID_MANIFEST,
    ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL,
    ST_SOURCE_LOAD_ERR_DUPLICATE_SOURCE,
    ST_SOURCE_LOAD_ERR_PARSE
} st_source_load_status_t;

typedef enum {
    ST_SOURCE_PHASE_NONE = 0,
    ST_SOURCE_PHASE_IMAGE_MANIFEST,
    ST_SOURCE_PHASE_IMAGE_SOURCE,
    ST_SOURCE_PHASE_APPLICATION_MANIFEST,
    ST_SOURCE_PHASE_APPLICATION_SOURCE
} st_source_load_phase_t;

typedef void *(*st_source_allocate_fn)(void *user, size_t size);
typedef void (*st_source_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_source_allocate_fn allocate;
    st_source_deallocate_fn deallocate;
    void *user;
} st_source_allocator_t;

typedef struct {
    size_t max_manifest_bytes;
    size_t max_file_bytes;
    size_t max_total_bytes;
    size_t max_files;
} st_source_limits_t;

typedef struct {
    st_source_load_status_t status;
    st_source_load_phase_t phase;
    size_t source_index;
    size_t manifest_line;
    int system_error;
    bool path_truncated;
    char path[ST_SOURCE_DIAGNOSTIC_PATH_CAPACITY];
    st_parse_error_t parse_error;
} st_source_diagnostic_t;

typedef struct {
    st_source_origin_t origin;
    size_t ordinal;
    /* One-based for manifest-loaded files; zero for legacy direct app paths. */
    size_t manifest_line;

    /* `path` preserves the exact manifest/caller spelling. `source_name` is
     * the name stored in the AST and used in diagnostics. */
    char *path;
    char *source_name;
    unsigned char *source;
    size_t source_length;
    st_ast_unit_t ast;

    /* Stable file identity is retained so aliases/hard links cannot enter the
     * same compilation twice under different spellings. */
    uintmax_t device;
    uintmax_t inode;
} st_source_file_t;

typedef struct {
    st_source_file_t *files;
    size_t count;
    size_t capacity;
    size_t image_count;
    size_t total_source_bytes;
    st_source_allocator_t allocator;
    st_source_limits_t limits;
    st_source_diagnostic_t diagnostic;
} st_source_bundle_t;

/* Loads and parses image files in manifest order, followed by app_paths in
 * caller order. No directory scan or glob is performed. `bundle` must be
 * fresh or previously destroyed. Loading is all-or-nothing: on failure no
 * source/AST remains owned, while `diagnostic` remains available.
 *
 * Manifest grammar: UTF-8/byte paths separated by LF (CRLF is accepted).
 * Empty lines and lines beginning with '#' are ignored. Every other line is
 * an untrimmed relative POSIX path beneath image_directory. Empty components,
 * '.', '..', backslashes, absolute/drive paths, trailing slashes and symlinked
 * path components are rejected. */
st_source_load_status_t st_source_bundle_load(
    st_source_bundle_t *bundle,
    const char *image_directory,
    const char *const *app_paths,
    size_t app_count,
    const st_source_allocator_t *allocator);

/* As above, with explicit non-zero resource budgets. Limits are checked both
 * against fstat and while reading because a regular file may grow after it is
 * opened. `max_total_bytes` excludes the manifest. */
st_source_load_status_t st_source_bundle_load_with_limits(
    st_source_bundle_t *bundle,
    const char *image_directory,
    const char *const *app_paths,
    size_t app_count,
    const st_source_allocator_t *allocator,
    const st_source_limits_t *limits);

/* Loads both product manifests with the same path grammar and protected
 * openat/O_NOFOLLOW traversal.  The complete image manifest is consumed
 * first, followed by the complete application manifest.  Both directories
 * and both manifests are borrowed only for the call; the returned bundle owns
 * every source buffer, source name, path and AST. */
st_source_load_status_t st_source_bundle_load_manifests(
    st_source_bundle_t *bundle,
    const char *image_directory,
    const char *application_directory,
    const st_source_allocator_t *allocator);

st_source_load_status_t st_source_bundle_load_manifests_with_limits(
    st_source_bundle_t *bundle,
    const char *image_directory,
    const char *application_directory,
    const st_source_allocator_t *allocator,
    const st_source_limits_t *limits);

void st_source_bundle_destroy(st_source_bundle_t *bundle);
const char *st_source_load_status_string(st_source_load_status_t status);
const char *st_source_load_phase_string(st_source_load_phase_t phase);

#endif
