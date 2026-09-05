#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "st_application_materialize.h"

#include "artifact_materialize_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define ST_APPLICATION_MATERIALIZE_LACKS_NOFOLLOW 1
#define O_NOFOLLOW 0
#endif

#define MATRIX_BUFFER_CAPACITY 4096u
#define STAGING_NAME_CAPACITY 112u

typedef struct {
    anvil_arch_t target;
    anvil_abi_t abi;
    bool supported;
} expected_profile_t;

static const expected_profile_t expected_profiles[] = {
    {ANVIL_ARCH_X86_64, ANVIL_ABI_SYSV, true},
    {ANVIL_ARCH_ARM64, ANVIL_ABI_SYSV, true},
    {ANVIL_ARCH_PPC64, ANVIL_ABI_SYSV, true},
    {ANVIL_ARCH_PPC64LE, ANVIL_ABI_SYSV, true},
    {ANVIL_ARCH_ZARCH, ANVIL_ABI_MVS, true},
    {ANVIL_ARCH_X86, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_S370, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_S370_XA, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_S390, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_PPC32, ANVIL_ABI_DEFAULT, false},
    {ANVIL_ARCH_X86_64, ANVIL_ABI_WIN64, true}
};

_Static_assert(sizeof(expected_profiles) / sizeof(expected_profiles[0]) == ST_APPLICATION_AOT_PROFILE_COUNT,
               "application publication matrix changed");

static atomic_uint_fast64_t application_staging_sequence =
    ATOMIC_VAR_INIT(0);

static ssize_t default_write(
    void *user, int file_descriptor, const void *bytes, size_t length)
{
    (void)user;
    return write(file_descriptor, bytes, length);
}

static bool portable_name(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;
    size_t length;

    if (name == NULL) {
        return false;
    }
    length = strlen(name);
    if (length == 0u || length > ST_APPLICATION_PUBLICATION_NAME_MAX
            || !((*cursor >= 'A' && *cursor <= 'Z')
                || (*cursor >= 'a' && *cursor <= 'z'))) {
        return false;
    }
    for (cursor++; *cursor != '\0'; cursor++) {
        if (!((*cursor >= 'A' && *cursor <= 'Z')
                || (*cursor >= 'a' && *cursor <= 'z')
                || (*cursor >= '0' && *cursor <= '9')
                || *cursor == '_')) {
            return false;
        }
    }
    return true;
}

static const char *target_name(anvil_arch_t target)
{
    switch (target) {
    case ANVIL_ARCH_X86_64: return "x86_64";
    case ANVIL_ARCH_ARM64: return "arm64";
    case ANVIL_ARCH_PPC64: return "ppc64";
    case ANVIL_ARCH_PPC64LE: return "ppc64le";
    case ANVIL_ARCH_ZARCH: return "zarch";
    case ANVIL_ARCH_X86: return "x86";
    case ANVIL_ARCH_S370: return "s370";
    case ANVIL_ARCH_S370_XA: return "s370_xa";
    case ANVIL_ARCH_S390: return "s390";
    case ANVIL_ARCH_PPC32: return "ppc32";
    default: return NULL;
    }
}

static const char *abi_name(anvil_abi_t abi)
{
    switch (abi) {
    case ANVIL_ABI_DEFAULT: return "default";
    case ANVIL_ABI_SYSV: return "sysv";
    case ANVIL_ABI_WIN64: return "win64";
    case ANVIL_ABI_MVS: return "mvs";
    default: return NULL;
    }
}

static const char *syntax_name(anvil_syntax_t syntax)
{
    switch (syntax) {
    case ANVIL_SYNTAX_DEFAULT: return "default";
    case ANVIL_SYNTAX_GAS: return "gas";
    case ANVIL_SYNTAX_HLASM: return "hlasm";
    default: return NULL;
    }
}

static const char *optimization_name(anvil_opt_level_t optimization)
{
    switch (optimization) {
    case ANVIL_OPT_STANDARD: return "O2";
    default: return NULL;
    }
}

static void hash_hex(
    const uint8_t hash[ST_ARTIFACT_SHA256_SIZE],
    char output[ST_ARTIFACT_SHA256_SIZE * 2u + 1u])
{
    static const char digits[] = "0123456789abcdef";

    for (size_t index = 0u; index < ST_ARTIFACT_SHA256_SIZE; index++) {
        output[index * 2u] = digits[hash[index] >> 4u];
        output[index * 2u + 1u] = digits[hash[index] & UINT8_C(0x0f)];
    }
    output[ST_ARTIFACT_SHA256_SIZE * 2u] = '\0';
}

static bool append_line(
    char buffer[MATRIX_BUFFER_CAPACITY], size_t *used,
    const char *format, ...)
{
    va_list arguments;
    int amount;

    if (*used >= MATRIX_BUFFER_CAPACITY) {
        return false;
    }
    va_start(arguments, format);
    amount = vsnprintf(
        buffer + *used, MATRIX_BUFFER_CAPACITY - *used, format, arguments);
    va_end(arguments);
    if (amount < 0 || (size_t)amount >= MATRIX_BUFFER_CAPACITY - *used) {
        return false;
    }
    *used += (size_t)amount;
    return true;
}

static bool application_result_valid(
    const st_application_aot_result_t *application,
    const char *application_name)
{
    char expected[MATRIX_BUFFER_CAPACITY];
    size_t used = 0u;

    if (application == NULL || application->status != ST_APPLICATION_AOT_OK
            || application->profile_count != ST_APPLICATION_AOT_PROFILE_COUNT
            || application->matrix_manifest == NULL
            || application->matrix_manifest_length == 0u
            || application->matrix_manifest[
                   application->matrix_manifest_length] != '\0'
            || !append_line(
                expected, &used,
                "anvil-smalltalk-application-matrix-v%" PRIu32 "\n"
                "application=%s\nprofile-count=%u\n",
                ST_APPLICATION_AOT_MATRIX_VERSION, application_name,
                ST_APPLICATION_AOT_PROFILE_COUNT)) {
        return false;
    }

    for (size_t index = 0u; index < ST_APPLICATION_AOT_PROFILE_COUNT;
         index++) {
        const st_application_aot_profile_t *profile =
            &application->profiles[index];
        const expected_profile_t *definition = &expected_profiles[index];
        const char *target = target_name(profile->target);
        const char *abi = abi_name(profile->abi);
        const char *syntax = syntax_name(profile->syntax);
        const char *optimization = optimization_name(profile->optimization);

        if (profile->target != definition->target || profile->abi != definition->abi || target == NULL
                || abi == NULL || syntax == NULL || optimization == NULL) {
            return false;
        }
        if (definition->supported) {
            char hash[ST_ARTIFACT_SHA256_SIZE * 2u + 1u];
            size_t launch_count = 0u;

            if (profile->state != ST_APPLICATION_PROFILE_READY
                    || profile->bundle.status != ST_ARTIFACT_BUNDLE_OK
                    || profile->bundle.target != profile->target
                    || profile->bundle.abi != profile->abi
                    || profile->bundle.syntax != profile->syntax
                    || profile->bundle.optimization != profile->optimization) {
                return false;
            }
            for (size_t artifact = 0u;
                 artifact < profile->bundle.artifact_count; artifact++) {
                if (profile->bundle.artifacts[artifact].kind
                        == ST_ARTIFACT_LAUNCH_ASSEMBLY) {
                    launch_count++;
                }
            }
            if (launch_count != 1u) {
                return false;
            }
            hash_hex(profile->bundle.bundle_sha256, hash);
            if (!append_line(
                    expected, &used,
                    "profile=%s|%s|%s|%s|ready|%s\n",
                    target, abi, syntax, optimization, hash)) {
                return false;
            }
        } else {
            if (profile->state != ST_APPLICATION_PROFILE_UNSUPPORTED
                    || profile->reason == NULL
                    || strcmp(
                        profile->reason,
                        "tagged32-abi-unimplemented") != 0
                    || profile->bundle.artifacts != NULL
                    || profile->bundle.artifact_count != 0u) {
                return false;
            }
            if (!append_line(
                    expected, &used,
                    "profile=%s|%s|%s|%s|unsupported|%s\n",
                    target, abi, syntax, optimization, profile->reason)) {
                return false;
            }
        }
    }
    return used == application->matrix_manifest_length
        && memcmp(expected, application->matrix_manifest, used) == 0;
}

static void remove_profile(
    int application_directory, const char *profile_name,
    const st_artifact_bundle_t *bundle)
{
    int profile = openat(
        application_directory, profile_name,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

    if (profile >= 0) {
        for (size_t index = 0u; index < bundle->artifact_count; index++) {
            (void)unlinkat(profile, bundle->artifacts[index].name, 0);
        }
        (void)unlinkat(profile, "bundle.manifest", 0);
        (void)close(profile);
    }
    (void)unlinkat(application_directory, profile_name, AT_REMOVEDIR);
}

static void clean_staging(
    int root, int application_directory, const char *staging_name,
    const st_application_aot_result_t *application,
    char profile_names[ST_APPLICATION_AOT_PROFILE_COUNT]
                      [ST_ARTIFACT_PROFILE_NAME_MAX],
    size_t profile_count, bool matrix_created)
{
    if (application_directory < 0 && root >= 0 && staging_name[0] != '\0') {
        application_directory = openat(
            root, staging_name,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (application_directory >= 0) {
        for (size_t index = 0u; index < profile_count; index++) {
            if (profile_names[index][0] == '\0') {
                continue;
            }

            remove_profile(
                application_directory, profile_names[index],
                &application->profiles[index].bundle);
        }
        if (matrix_created) {
            (void)unlinkat(application_directory, "matrix.manifest", 0);
        }
        (void)close(application_directory);
    }
    if (root >= 0 && staging_name[0] != '\0') {
        (void)unlinkat(root, staging_name, AT_REMOVEDIR);
    }
}

void st_application_materialize_result_init(
    st_application_materialize_result_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->status = ST_APPLICATION_MATERIALIZE_ERR_INVALID_ARGUMENT;
        result->failed_profile_index = SIZE_MAX;
    }
}

st_application_materialize_status_t st_application_aot_materialize(
    st_application_materialize_result_t *result,
    const st_application_aot_result_t *application,
    const char *application_name, const char *output_directory,
    const st_application_materialize_options_t *options)
{
    st_artifact_materialize_write_fn writer = default_write;
    void *write_user = NULL;
    char staging[STAGING_NAME_CAPACITY] = {0};
    char profile_names[ST_APPLICATION_AOT_PROFILE_COUNT]
                      [ST_ARTIFACT_PROFILE_NAME_MAX] = {{0}};
    int root = -1;
    int application_directory = -1;
    int saved_error = 0;
    size_t materialized_count = 0u;
    bool matrix_created = false;
    st_application_materialize_status_t status =
        ST_APPLICATION_MATERIALIZE_ERR_IO;

    if (result == NULL) {
        return ST_APPLICATION_MATERIALIZE_ERR_INVALID_ARGUMENT;
    }
    st_application_materialize_result_init(result);
    if (!portable_name(application_name) || output_directory == NULL
            || output_directory[0] == '\0' || options == NULL
            || ((options->artifact_options.allocator.allocate == NULL)
                != (options->artifact_options.allocator.deallocate == NULL))) {
        return result->status;
    }
#ifdef ST_APPLICATION_MATERIALIZE_LACKS_NOFOLLOW
    result->status = ST_APPLICATION_MATERIALIZE_ERR_UNSUPPORTED_PLATFORM;
    return result->status;
#endif
    if (!application_result_valid(application, application_name)) {
        result->status = ST_APPLICATION_MATERIALIZE_ERR_INVALID_RESULT;
        return result->status;
    }
    memcpy(result->application, application_name, strlen(application_name) + 1u);
    if (options->artifact_options.write != NULL) {
        writer = options->artifact_options.write;
        write_user = options->artifact_options.write_user;
    }
    root = st_artifact_open_directory_no_symlinks(output_directory);
    if (root < 0) {
        result->system_error = errno;
        result->status = ST_APPLICATION_MATERIALIZE_ERR_IO;
        return result->status;
    }
    for (size_t attempt = 0u; attempt < 128u; attempt++) {
        uint64_t sequence = atomic_fetch_add_explicit(
            &application_staging_sequence, UINT64_C(1), memory_order_relaxed);
        int amount = snprintf(
            staging, sizeof(staging),
            ".anvil-application-staging-%ld-%016" PRIx64,
            (long)getpid(), sequence);

        if (amount < 0 || (size_t)amount >= sizeof(staging)) {
            saved_error = EOVERFLOW;
            goto failure;
        }
        if (mkdirat(root, staging, 0700) == 0) {
            break;
        }
        if (errno != EEXIST) {
            saved_error = errno;
            goto failure;
        }
        staging[0] = '\0';
    }
    if (staging[0] == '\0') {
        saved_error = EEXIST;
        status = ST_APPLICATION_MATERIALIZE_ERR_COLLISION;
        goto failure;
    }
    application_directory = openat(
        root, staging, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (application_directory < 0) {
        saved_error = errno;
        goto failure;
    }

    for (size_t index = 0u;
         index < ST_APPLICATION_AOT_PROFILE_COUNT; index++) {
        st_artifact_materialize_result_t profile_result;

        if (application->profiles[index].state != ST_APPLICATION_PROFILE_READY) {
            continue;
        }

        st_artifact_materialize_result_init(&profile_result);
        result->profile_status = st_artifact_bundle_materialize_at(
            &profile_result, &application->profiles[index].bundle,
            application_directory, &options->artifact_options);
        if (profile_result.committed) {
            memcpy(
                profile_names[index], profile_result.profile,
                strlen(profile_result.profile) + 1u);
            materialized_count = index + 1u;
        }
        if (result->profile_status != ST_ARTIFACT_MATERIALIZE_OK) {
            result->failed_profile_index = index;
            result->system_error = profile_result.system_error;
            memcpy(
                result->profile, profile_result.profile,
                strlen(profile_result.profile) + 1u);
            status = ST_APPLICATION_MATERIALIZE_ERR_PROFILE;
            goto failure;
        }
    }

    matrix_created = true;
    if (!st_artifact_write_one(
            application_directory, "matrix.manifest",
            application->matrix_manifest, application->matrix_manifest_length,
            writer, write_user)) {
        saved_error = errno;
        goto failure;
    }
    if (fchmod(application_directory, 0755) != 0
            || fsync(application_directory) != 0) {
        saved_error = errno;
        goto failure;
    }
    if (close(application_directory) != 0) {
        application_directory = -1;
        saved_error = errno;
        goto failure;
    }
    application_directory = -1;
    if (st_artifact_publish_noreplace(
            root, staging, application_name) != 0) {
        saved_error = errno;
        if (errno == EEXIST || errno == ENOTEMPTY) {
            status = ST_APPLICATION_MATERIALIZE_ERR_COLLISION;
        } else if (errno == ENOSYS || errno == EINVAL) {
            status = ST_APPLICATION_MATERIALIZE_ERR_UNSUPPORTED_PLATFORM;
        }
        goto failure;
    }
    staging[0] = '\0';
    result->committed = true;
    if (fsync(root) != 0) {
        result->system_error = errno;
        (void)close(root);
        result->status = ST_APPLICATION_MATERIALIZE_ERR_DURABILITY;
        return result->status;
    }
    if (close(root) != 0) {
        result->system_error = errno;
        result->status = ST_APPLICATION_MATERIALIZE_ERR_DURABILITY;
        return result->status;
    }
    result->status = ST_APPLICATION_MATERIALIZE_OK;
    return result->status;

failure:
    clean_staging(
        root, application_directory, staging, application, profile_names,
        materialized_count, matrix_created);
    if (root >= 0) {
        (void)close(root);
    }
    result->system_error = saved_error;
    result->status = status;
    return result->status;
}

const char *st_application_materialize_status_string(
    st_application_materialize_status_t status)
{
    switch (status) {
    case ST_APPLICATION_MATERIALIZE_OK: return "ok";
    case ST_APPLICATION_MATERIALIZE_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_APPLICATION_MATERIALIZE_ERR_INVALID_RESULT:
        return "invalid application AOT result";
    case ST_APPLICATION_MATERIALIZE_ERR_UNSUPPORTED_PLATFORM:
        return "atomic no-replace publication is unsupported";
    case ST_APPLICATION_MATERIALIZE_ERR_COLLISION:
        return "application already exists";
    case ST_APPLICATION_MATERIALIZE_ERR_PROFILE:
        return "profile materialization failed";
    case ST_APPLICATION_MATERIALIZE_ERR_IO: return "I/O error";
    case ST_APPLICATION_MATERIALIZE_ERR_DURABILITY:
        return "application committed but directory synchronization failed";
    }
    return "unknown application materialization status";
}
