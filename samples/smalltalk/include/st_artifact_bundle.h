#ifndef ANVIL_SMALLTALK_ARTIFACT_BUNDLE_H
#define ANVIL_SMALLTALK_ARTIFACT_BUNDLE_H

#include "st_aot_compile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_ARTIFACT_BUNDLE_FORMAT_VERSION UINT32_C(2)
#define ST_ARTIFACT_SHA256_SIZE 32u

typedef enum {
    ST_ARTIFACT_BUNDLE_OK = 0,
    ST_ARTIFACT_BUNDLE_ERR_INVALID_ARGUMENT,
    ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT,
    ST_ARTIFACT_BUNDLE_ERR_UNSUPPORTED_TARGET,
    ST_ARTIFACT_BUNDLE_ERR_COLLISION,
    ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY,
    ST_ARTIFACT_BUNDLE_ERR_OVERFLOW,
    ST_ARTIFACT_BUNDLE_ERR_CODEGEN
} st_artifact_bundle_status_t;

typedef enum {
    ST_ARTIFACT_METHOD_ASSEMBLY = 0,
    ST_ARTIFACT_METADATA_ASSEMBLY,
    ST_ARTIFACT_LAUNCH_ASSEMBLY
} st_artifact_kind_t;

typedef void *(*st_artifact_allocate_fn)(void *user, size_t size);
typedef void (*st_artifact_deallocate_fn)(void *user, void *pointer);

typedef struct {
    st_artifact_allocate_fn allocate;
    st_artifact_deallocate_fn deallocate;
    void *user;
} st_artifact_allocator_t;

typedef struct {
    /* Target, ABI, syntax, optimization and symbol prefix are always derived
     * from the owned provenance in st_aot_compile_result_t. */
    st_artifact_allocator_t allocator;

    /* Optional application launch-data module.  Generic compiler regressions
     * may omit it; product application bundles require it.  Both fields are
     * either present together or absent together, and the named exported
     * descriptor must exist in the borrowed module. */
    anvil_module_t *launch_module;
    const char *launch_symbol;
    size_t launch_symbol_length;
} st_artifact_bundle_options_t;

typedef struct {
    st_artifact_kind_t kind;
    st_class_graph_method_id_t method_id;
    char *name;
    size_t name_length;
    char *symbol;
    size_t symbol_length;
    unsigned char *bytes;
    size_t size;
    uint8_t sha256[ST_ARTIFACT_SHA256_SIZE];
} st_artifact_blob_t;

typedef struct {
    st_artifact_bundle_status_t status;
    anvil_arch_t target;
    anvil_abi_t abi;
    anvil_syntax_t syntax;
    anvil_opt_level_t optimization;
    st_artifact_blob_t *artifacts;
    size_t artifact_count;
    size_t block_count;
    char *manifest;
    size_t manifest_length;
    uint8_t bundle_sha256[ST_ARTIFACT_SHA256_SIZE];

    /* Populated only for ST_ARTIFACT_BUNDLE_ERR_CODEGEN.  Zero means the
     * metadata module; method ids are one-based. */
    st_class_graph_method_id_t failed_method_id;
    anvil_error_t codegen_error;
    void *implementation;
} st_artifact_bundle_t;

void st_artifact_bundle_init(st_artifact_bundle_t *bundle);
void st_artifact_bundle_destroy(st_artifact_bundle_t *bundle);

/* Computes the digest used by blob and bundle manifests.  Empty input is
 * represented by bytes == NULL and length == 0. */
bool st_artifact_sha256(const void *bytes, size_t length,
                        uint8_t output[ST_ARTIFACT_SHA256_SIZE]);

/*
 * Renders one owned assembly blob per lowered method plus one metadata blob.
 * No filesystem, process, assembler, or linker operation is performed.  The
 * input compilation result remains owned by the caller and must only stay
 * alive for this call; every published byte, name, symbol and manifest field
 * is copied into the returned bundle.  Failure is transactional.
 */
st_artifact_bundle_status_t st_artifact_bundle_render(
    st_artifact_bundle_t *bundle,
    const st_aot_compile_result_t *compiled,
    const st_artifact_bundle_options_t *options);

const char *st_artifact_bundle_status_string(
    st_artifact_bundle_status_t status);

#ifdef __cplusplus
}
#endif

#endif
