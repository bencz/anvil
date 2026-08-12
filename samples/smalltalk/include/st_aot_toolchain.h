#ifndef ANVIL_SMALLTALK_AOT_TOOLCHAIN_H
#define ANVIL_SMALLTALK_AOT_TOOLCHAIN_H

#include "st_artifact_bundle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_AOT_TOOLCHAIN_ABI_VERSION UINT32_C(1)

typedef enum {
    ST_AOT_TOOLCHAIN_OK = 0,
    ST_AOT_TOOLCHAIN_ERR_INVALID_ARGUMENT,
    ST_AOT_TOOLCHAIN_ERR_TOOL_NOT_ALLOWED,
    ST_AOT_TOOLCHAIN_ERR_UNSUPPORTED_PROFILE,
    ST_AOT_TOOLCHAIN_ERR_INVALID_MANIFEST,
    ST_AOT_TOOLCHAIN_ERR_ARTIFACT_MISMATCH,
    ST_AOT_TOOLCHAIN_ERR_COLLISION,
    ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY,
    ST_AOT_TOOLCHAIN_ERR_OVERFLOW,
    ST_AOT_TOOLCHAIN_ERR_IO,
    ST_AOT_TOOLCHAIN_ERR_SPAWN,
    ST_AOT_TOOLCHAIN_ERR_TOOL_FAILED,
    ST_AOT_TOOLCHAIN_ERR_DURABILITY
} st_aot_toolchain_status_t;

typedef enum {
    ST_AOT_TOOLCHAIN_STAGE_NONE = 0,
    ST_AOT_TOOLCHAIN_STAGE_ASSEMBLE,
    ST_AOT_TOOLCHAIN_STAGE_LINK
} st_aot_toolchain_stage_t;

typedef struct {
    const char *profile_directory;
    const char *output_directory;
    const char *publication_name;
    const char *executable_name;

    /* The configured compiler driver must exactly match one allowlist entry.
     * It is passed to posix_spawnp as argv[0]; no command text is interpreted
     * by a shell. An absolute path is recommended for product builds. */
    const char *compiler_driver;
    const char *const *allowed_compiler_drivers;
    size_t allowed_compiler_driver_count;

    /* Prebuilt product-runtime objects or archives. They are appended to the
     * link argv in this exact order. Every input is borrowed for this call. */
    const char *const *runtime_link_inputs;
    size_t runtime_link_input_count;

    /* Optional exact linker arguments, for example "-pthread". Empty strings
     * and arguments containing CR or LF are rejected. They are individual
     * argv elements and are never reparsed. */
    const char *const *link_arguments;
    size_t link_argument_count;
} st_aot_toolchain_options_t;

typedef struct {
    st_aot_toolchain_status_t status;
    st_aot_toolchain_stage_t failed_stage;
    int system_error;
    int child_exit_code;
    int child_signal;
    bool committed;

    /* Exact argv and stderr from the failed external process. The vector is
     * NULL-terminated; its strings and stderr bytes are owned by result. */
    char **failed_argv;
    size_t failed_argc;
    char *tool_stderr;
    size_t tool_stderr_length;

    char *published_directory;
    char *published_executable;
    void *implementation;
} st_aot_toolchain_result_t;

void st_aot_toolchain_result_init(st_aot_toolchain_result_t *result);
void st_aot_toolchain_result_destroy(st_aot_toolchain_result_t *result);

/*
 * Assembles and links one materialized x86-64 SysV/GAS/O2 profile. The input
 * bundle manifest and every listed assembly are validated before any tool is
 * invoked. Validated bytes are copied into a private staging directory, so a
 * later change to the source profile cannot affect the child process.
 *
 * The publication directory is atomically installed without replacement.
 * On every pre-publication failure, no final path becomes visible and staging
 * is removed. A post-publication directory-fsync failure reports DURABILITY
 * with committed == true, preserving the already-visible complete product.
 */
st_aot_toolchain_status_t st_aot_toolchain_link(
    st_aot_toolchain_result_t *result,
    const st_aot_toolchain_options_t *options);

const char *st_aot_toolchain_status_string(st_aot_toolchain_status_t status);
const char *st_aot_toolchain_stage_string(st_aot_toolchain_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif
