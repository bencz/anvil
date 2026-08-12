#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "st_artifact_materialize.h"
#include "artifact_materialize_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define ST_ARTIFACT_MATERIALIZE_LACKS_NOFOLLOW 1
#define O_NOFOLLOW 0
#endif

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

static atomic_uint_fast64_t staging_sequence = ATOMIC_VAR_INIT(0);

static void *default_allocate(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void default_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static ssize_t default_write(void *user, int file_descriptor,
                             const void *bytes, size_t length)
{
    (void)user;
    return write(file_descriptor, bytes, length);
}

static const char *target_name(anvil_arch_t target)
{
    switch (target) {
    case ANVIL_ARCH_X86_64: return "x86_64";
    case ANVIL_ARCH_ARM64: return "arm64";
    case ANVIL_ARCH_PPC64: return "ppc64";
    case ANVIL_ARCH_PPC64LE: return "ppc64le";
    case ANVIL_ARCH_ZARCH: return "zarch";
    default: return NULL;
    }
}

static const char *abi_name(anvil_abi_t abi)
{
    switch (abi) {
    case ANVIL_ABI_DEFAULT: return "default";
    case ANVIL_ABI_SYSV: return "sysv";
    case ANVIL_ABI_DARWIN: return "darwin";
    case ANVIL_ABI_WIN64: return "win64";
    case ANVIL_ABI_MVS: return "mvs";
    default: return NULL;
    }
}

static const char *syntax_name(anvil_syntax_t syntax)
{
    switch (syntax) {
    case ANVIL_SYNTAX_GAS: return "gas";
    case ANVIL_SYNTAX_HLASM: return "hlasm";
    default: return NULL;
    }
}

static const char *optimization_name(anvil_opt_level_t optimization)
{
    switch (optimization) {
    case ANVIL_OPT_NONE: return "O0";
    case ANVIL_OPT_DEBUG: return "Og";
    case ANVIL_OPT_BASIC: return "O1";
    case ANVIL_OPT_STANDARD: return "O2";
    case ANVIL_OPT_AGGRESSIVE: return "O3";
    default: return NULL;
    }
}

static bool target_abi_valid(anvil_arch_t target, anvil_abi_t abi)
{
    switch (target) {
    case ANVIL_ARCH_X86_64:
        return abi == ANVIL_ABI_DEFAULT || abi == ANVIL_ABI_SYSV
            || abi == ANVIL_ABI_DARWIN || abi == ANVIL_ABI_WIN64;
    case ANVIL_ARCH_ARM64:
        return abi == ANVIL_ABI_SYSV || abi == ANVIL_ABI_DARWIN;
    case ANVIL_ARCH_PPC64:
    case ANVIL_ARCH_PPC64LE:
        return abi == ANVIL_ABI_DEFAULT || abi == ANVIL_ABI_SYSV;
    case ANVIL_ARCH_ZARCH:
        return abi == ANVIL_ABI_DEFAULT || abi == ANVIL_ABI_MVS;
    default:
        return false;
    }
}

static bool filename_valid(const char *name, size_t length,
                           const char *required_extension)
{
    size_t index, extension_length;
    if (name == NULL || length == 0u || length > 240u
            || name[length] != '\0' || name[0] == '.')
        return false;
    for (index = 0u; index < length; index++) {
        unsigned char byte = (unsigned char)name[index];
        if (!((byte >= 'A' && byte <= 'Z')
                || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9') || byte == '_'
                || byte == '-' || byte == '.'))
            return false;
    }
    extension_length = strlen(required_extension);
    return length > extension_length
        && memcmp(name + length - extension_length, required_extension,
                  extension_length) == 0;
}

static bool symbol_valid(const char *symbol, size_t length)
{
    size_t index;
    unsigned char byte;
    if (symbol == NULL || length == 0u || symbol[length] != '\0') return false;
    byte = (unsigned char)symbol[0];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
            || byte == '_'))
        return false;
    for (index = 1u; index < length; index++) {
        byte = (unsigned char)symbol[index];
        if (!((byte >= 'A' && byte <= 'Z')
                || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9') || byte == '_'))
            return false;
    }
    return true;
}

static bool manifest_line(const st_artifact_bundle_t *bundle,
                          const char *key, const char *value)
{
    char expected[128];
    const char *found;
    int length = snprintf(expected, sizeof(expected), "%s=%s\n", key, value);
    if (length < 0 || (size_t)length >= sizeof(expected)) return false;
    found = strstr(bundle->manifest, expected);
    return found != NULL && (found == bundle->manifest || found[-1] == '\n')
        && (size_t)(found - bundle->manifest) <= bundle->manifest_length
        && (size_t)length <= bundle->manifest_length
                              - (size_t)(found - bundle->manifest);
}

static bool hash_equal(const uint8_t *bytes, size_t length,
                       const uint8_t expected[ST_ARTIFACT_SHA256_SIZE])
{
    uint8_t actual[ST_ARTIFACT_SHA256_SIZE];
    return st_artifact_sha256(bytes, length, actual)
        && memcmp(actual, expected, sizeof(actual)) == 0;
}

static bool manifest_has_bundle_hash(const st_artifact_bundle_t *bundle)
{
    static const char marker[] = "bundle-sha256=";
    static const char digits[] = "0123456789abcdef";
    char expected[ST_ARTIFACT_SHA256_SIZE * 2u + 1u];
    const char *found;
    size_t index;
    for (index = 0u; index < ST_ARTIFACT_SHA256_SIZE; index++) {
        expected[index * 2u] = digits[bundle->bundle_sha256[index] >> 4u];
        expected[index * 2u + 1u] =
            digits[bundle->bundle_sha256[index] & UINT8_C(0x0f)];
    }
    expected[sizeof(expected) - 1u] = '\0';
    if (bundle->manifest_length
            < (sizeof(marker) - 1u) + sizeof(expected))
        return false;
    found = strstr(bundle->manifest, marker);
    return found != NULL
        && (size_t)(found - bundle->manifest) < bundle->manifest_length
        && (size_t)(found - bundle->manifest)
               <= bundle->manifest_length - (sizeof(marker) - 1u)
        && bundle->manifest_length - (size_t)(found - bundle->manifest)
               >= (sizeof(marker) - 1u) + sizeof(expected)
        && memcmp(found + sizeof(marker) - 1u, expected,
                  sizeof(expected) - 1u) == 0
        && found[sizeof(marker) - 1u + sizeof(expected) - 1u] == '\n';
}

static bool bundle_valid(const st_artifact_bundle_t *bundle,
                         char profile[ST_ARTIFACT_PROFILE_NAME_MAX])
{
    const char *target, *abi, *syntax, *optimization, *extension;
    size_t index, other, metadata_count = 0u, launch_count = 0u;
    int length;
    if (bundle == NULL || bundle->status != ST_ARTIFACT_BUNDLE_OK
            || bundle->implementation == NULL || bundle->artifact_count == 0u
            || bundle->artifacts == NULL || bundle->manifest == NULL
            || bundle->manifest_length == 0u
            || bundle->manifest[bundle->manifest_length] != '\0')
        return false;
    target = target_name(bundle->target);
    abi = abi_name(bundle->abi);
    syntax = syntax_name(bundle->syntax);
    optimization = optimization_name(bundle->optimization);
    if (target == NULL || abi == NULL || syntax == NULL
            || optimization == NULL || !target_abi_valid(bundle->target,
                                                          bundle->abi)
            || (bundle->target == ANVIL_ARCH_ZARCH)
               != (bundle->syntax == ANVIL_SYNTAX_HLASM))
        return false;
    length = snprintf(profile, ST_ARTIFACT_PROFILE_NAME_MAX, "%s-%s-%s-%s",
                      target, abi, syntax, optimization);
    if (length < 0 || (size_t)length >= ST_ARTIFACT_PROFILE_NAME_MAX)
        return false;
    if (strncmp(bundle->manifest, "anvil-smalltalk-artifact-bundle-v",
                sizeof("anvil-smalltalk-artifact-bundle-v") - 1u) != 0
            || !manifest_line(bundle, "target", target)
            || !manifest_line(bundle, "abi", abi)
            || !manifest_line(bundle, "syntax", syntax)
            || !manifest_line(bundle, "optimization", optimization))
        return false;
    extension = bundle->syntax == ANVIL_SYNTAX_HLASM ? ".asm" : ".s";
    for (index = 0u; index < bundle->artifact_count; index++) {
        const st_artifact_blob_t *artifact = &bundle->artifacts[index];
        if (!filename_valid(artifact->name, artifact->name_length, extension)
                || !symbol_valid(artifact->symbol, artifact->symbol_length)
                || artifact->bytes == NULL || artifact->size == 0u
                || !hash_equal(artifact->bytes, artifact->size,
                               artifact->sha256)
                || strcmp(artifact->name, "bundle.manifest") == 0)
            return false;
        if (artifact->kind == ST_ARTIFACT_METADATA_ASSEMBLY) {
            const char *expected = bundle->syntax == ANVIL_SYNTAX_HLASM
                ? "metadata.asm" : "metadata.s";
            metadata_count++;
            if (artifact->method_id != ST_CLASS_GRAPH_INVALID_ID
                    || strcmp(artifact->name, expected) != 0)
                return false;
        } else if (artifact->kind == ST_ARTIFACT_LAUNCH_ASSEMBLY) {
            const char *expected = bundle->syntax == ANVIL_SYNTAX_HLASM
                ? "launch.asm" : "launch.s";
            launch_count++;
            if (artifact->method_id != ST_CLASS_GRAPH_INVALID_ID
                    || strcmp(artifact->name, expected) != 0) {
                return false;
            }
        } else if (artifact->kind != ST_ARTIFACT_METHOD_ASSEMBLY
                || artifact->method_id == ST_CLASS_GRAPH_INVALID_ID) {
            return false;
        }
        for (other = 0u; other < index; other++)
            if (bundle->artifacts[other].name_length == artifact->name_length
                    && memcmp(bundle->artifacts[other].name, artifact->name,
                              artifact->name_length) == 0)
                return false;
    }
    return metadata_count == 1u && launch_count <= 1u
        && manifest_line(bundle, "launch",
                         launch_count == 1u ? "present" : "absent")
        && manifest_has_bundle_hash(bundle);
}

static bool bit_get(const uint8_t *bits, size_t index)
{
    return (bits[index >> 3u] & (uint8_t)(UINT8_C(1) << (index & 7u))) != 0u;
}

static void bit_set(uint8_t *bits, size_t index)
{
    bits[index >> 3u] |= (uint8_t)(UINT8_C(1) << (index & 7u));
}

int st_artifact_publish_noreplace(
    int directory, const char *staging, const char *profile)
{
#ifdef SYS_renameat2
    return (int)syscall(SYS_renameat2, directory, staging, directory, profile,
                        RENAME_NOREPLACE);
#else
    (void)directory;
    (void)staging;
    (void)profile;
    errno = ENOSYS;
    return -1;
#endif
}

static bool write_all(st_artifact_materialize_write_fn writer, void *user,
                      int file_descriptor, const void *bytes, size_t length)
{
    const uint8_t *cursor = bytes;
    while (length != 0u) {
        ssize_t written = writer(user, file_descriptor, cursor, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0 || (size_t)written > length) {
            errno = EIO;
            return false;
        }
        cursor += (size_t)written;
        length -= (size_t)written;
    }
    return true;
}

bool st_artifact_write_one(
    int directory, const char *name, const void *bytes, size_t length,
    st_artifact_materialize_write_fn writer, void *write_user)
{
    int file = openat(directory, name,
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                      0600);
    bool ok;
    int saved_error = 0;
    if (file < 0) return false;
    ok = write_all(writer, write_user, file, bytes, length);
    if (ok && fchmod(file, 0644) != 0) ok = false;
    if (ok && fsync(file) != 0) ok = false;
    if (!ok) saved_error = errno;
    if (close(file) != 0 && ok) {
        ok = false;
        saved_error = errno;
    }
    if (!ok) errno = saved_error;
    return ok;
}

int st_artifact_open_directory_no_symlinks(const char *path)
{
    const char *cursor = path;
    int current = open(path[0] == '/' ? "/" : ".",
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current < 0) return -1;
    while (*cursor != '\0') {
        char component[NAME_MAX + 1u];
        const char *start;
        size_t length;
        int next, saved_error;
        while (*cursor == '/') cursor++;
        if (*cursor == '\0') break;
        start = cursor;
        while (*cursor != '\0' && *cursor != '/') cursor++;
        length = (size_t)(cursor - start);
        if (length == 1u && start[0] == '.') continue;
        if (length == 2u && start[0] == '.' && start[1] == '.') {
            (void)close(current);
            errno = EINVAL;
            return -1;
        }
        if (length == 0u || length > NAME_MAX) {
            (void)close(current);
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(component, start, length);
        component[length] = '\0';
        next = openat(current, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC
                                         | O_NOFOLLOW);
        if (next < 0) {
            saved_error = errno;
            (void)close(current);
            errno = saved_error;
            return -1;
        }
        (void)close(current);
        current = next;
    }
    return current;
}

static void clean_staging(int root, int staging_directory,
                          const char *staging_name,
                          const st_artifact_bundle_t *bundle,
                          const uint8_t *created, bool manifest_created)
{
    size_t index;
    if (staging_directory >= 0) {
        for (index = 0u; index < bundle->artifact_count; index++)
            if (bit_get(created, index))
                (void)unlinkat(staging_directory,
                               bundle->artifacts[index].name, 0);
        if (manifest_created)
            (void)unlinkat(staging_directory, "bundle.manifest", 0);
        (void)close(staging_directory);
    }
    if (root >= 0 && staging_name[0] != '\0')
        (void)unlinkat(root, staging_name, AT_REMOVEDIR);
}

void st_artifact_materialize_result_init(
    st_artifact_materialize_result_t *result)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->status = ST_ARTIFACT_MATERIALIZE_ERR_INVALID_ARGUMENT;
}

st_artifact_materialize_status_t st_artifact_bundle_materialize_at(
    st_artifact_materialize_result_t *result,
    const st_artifact_bundle_t *bundle, int output_directory_fd,
    const st_artifact_materialize_options_t *options)
{
    st_artifact_allocator_t allocator = {
        default_allocate, default_deallocate, NULL
    };
    st_artifact_materialize_write_fn writer = default_write;
    void *write_user = NULL;
    uint8_t *created = NULL;
    size_t bitmap_size, index;
    int root = output_directory_fd;
    int staging_directory = -1;
    int saved_error = 0;
    char profile[ST_ARTIFACT_PROFILE_NAME_MAX] = {0};
    char staging[96] = {0};
    bool manifest_created = false;
    st_artifact_materialize_status_t status = ST_ARTIFACT_MATERIALIZE_ERR_IO;
    struct stat root_stat;
    if (result == NULL) return ST_ARTIFACT_MATERIALIZE_ERR_INVALID_ARGUMENT;
    st_artifact_materialize_result_init(result);
    if (output_directory_fd < 0 || options == NULL
            || ((options->allocator.allocate == NULL)
                != (options->allocator.deallocate == NULL)))
        return ST_ARTIFACT_MATERIALIZE_ERR_INVALID_ARGUMENT;
#ifdef ST_ARTIFACT_MATERIALIZE_LACKS_NOFOLLOW
    result->status = ST_ARTIFACT_MATERIALIZE_ERR_UNSUPPORTED_PLATFORM;
    return result->status;
#endif
    if (!bundle_valid(bundle, profile)) {
        result->status = ST_ARTIFACT_MATERIALIZE_ERR_INVALID_BUNDLE;
        return result->status;
    }
    memcpy(result->profile, profile, strlen(profile) + 1u);
    if (options->allocator.allocate != NULL) allocator = options->allocator;
    if (options->write != NULL) {
        writer = options->write;
        write_user = options->write_user;
    }
    if (bundle->artifact_count > SIZE_MAX - 7u) {
        result->status = ST_ARTIFACT_MATERIALIZE_ERR_INVALID_BUNDLE;
        return result->status;
    }
    bitmap_size = (bundle->artifact_count + 7u) >> 3u;
    created = allocator.allocate(allocator.user, bitmap_size);
    if (created == NULL) {
        result->status = ST_ARTIFACT_MATERIALIZE_ERR_OUT_OF_MEMORY;
        return result->status;
    }
    memset(created, 0, bitmap_size);
    if (fstat(root, &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
        saved_error = errno != 0 ? errno : ENOTDIR;
        goto failure;
    }
    for (index = 0u; index < 128u; index++) {
        uint64_t sequence = atomic_fetch_add_explicit(
            &staging_sequence, UINT64_C(1), memory_order_relaxed);
        int length = snprintf(staging, sizeof(staging),
                              ".anvil-staging-%ld-%016" PRIx64,
                              (long)getpid(), sequence);
        if (length < 0 || (size_t)length >= sizeof(staging)) {
            saved_error = EOVERFLOW;
            goto failure;
        }
        if (mkdirat(root, staging, 0700) == 0) break;
        if (errno != EEXIST) {
            saved_error = errno;
            goto failure;
        }
        staging[0] = '\0';
    }
    if (staging[0] == '\0') {
        saved_error = EEXIST;
        status = ST_ARTIFACT_MATERIALIZE_ERR_COLLISION;
        goto failure;
    }
    staging_directory = openat(root, staging, O_RDONLY | O_DIRECTORY
                                             | O_CLOEXEC | O_NOFOLLOW);
    if (staging_directory < 0) {
        saved_error = errno;
        goto failure;
    }
    for (index = 0u; index < bundle->artifact_count; index++) {
        const st_artifact_blob_t *artifact = &bundle->artifacts[index];
        /* Record the attempted name before open/write: an exact-write failure
         * can leave a partially-created file that must be rolled back. */
        bit_set(created, index);
        if (!st_artifact_write_one(
                staging_directory, artifact->name, artifact->bytes,
                artifact->size, writer, write_user)) {
            saved_error = errno;
            goto failure;
        }
    }
    manifest_created = true;
    if (!st_artifact_write_one(
            staging_directory, "bundle.manifest", bundle->manifest,
            bundle->manifest_length, writer, write_user)) {
        saved_error = errno;
        goto failure;
    }
    if (fchmod(staging_directory, 0755) != 0
            || fsync(staging_directory) != 0) {
        saved_error = errno;
        goto failure;
    }
    if (st_artifact_publish_noreplace(root, staging, profile) != 0) {
        saved_error = errno;
        if (errno == EEXIST || errno == ENOTEMPTY)
            status = ST_ARTIFACT_MATERIALIZE_ERR_COLLISION;
        else if (errno == ENOSYS || errno == EINVAL)
            status = ST_ARTIFACT_MATERIALIZE_ERR_UNSUPPORTED_PLATFORM;
        goto failure;
    }
    staging[0] = '\0';
    result->committed = 1;
    if (close(staging_directory) != 0) {
        staging_directory = -1;
        result->system_error = errno;
        result->status = ST_ARTIFACT_MATERIALIZE_ERR_DURABILITY;
        allocator.deallocate(allocator.user, created);
        return result->status;
    }
    staging_directory = -1;
    if (fsync(root) != 0) {
        result->system_error = errno;
        result->status = ST_ARTIFACT_MATERIALIZE_ERR_DURABILITY;
        allocator.deallocate(allocator.user, created);
        return result->status;
    }
    allocator.deallocate(allocator.user, created);
    result->status = ST_ARTIFACT_MATERIALIZE_OK;
    return result->status;

failure:
    clean_staging(root, staging_directory, staging, bundle, created,
                  manifest_created);
    allocator.deallocate(allocator.user, created);
    result->system_error = saved_error;
    result->status = status;
    return result->status;
}

st_artifact_materialize_status_t st_artifact_bundle_materialize(
    st_artifact_materialize_result_t *result,
    const st_artifact_bundle_t *bundle, const char *output_directory,
    const st_artifact_materialize_options_t *options)
{
    st_artifact_materialize_status_t status;
    int root;

    if (result == NULL) {
        return ST_ARTIFACT_MATERIALIZE_ERR_INVALID_ARGUMENT;
    }
    st_artifact_materialize_result_init(result);
    if (output_directory == NULL || output_directory[0] == '\0'
            || options == NULL) {
        return ST_ARTIFACT_MATERIALIZE_ERR_INVALID_ARGUMENT;
    }
    root = st_artifact_open_directory_no_symlinks(output_directory);
    if (root < 0) {
        result->system_error = errno;
        result->status = ST_ARTIFACT_MATERIALIZE_ERR_IO;
        return result->status;
    }
    status = st_artifact_bundle_materialize_at(
        result, bundle, root, options);
    if (close(root) != 0 && status == ST_ARTIFACT_MATERIALIZE_OK) {
        result->system_error = errno;
        result->status = ST_ARTIFACT_MATERIALIZE_ERR_DURABILITY;
        return result->status;
    }
    return status;
}

const char *st_artifact_materialize_status_string(
    st_artifact_materialize_status_t status)
{
    switch (status) {
    case ST_ARTIFACT_MATERIALIZE_OK: return "ok";
    case ST_ARTIFACT_MATERIALIZE_ERR_INVALID_ARGUMENT:
        return "invalid argument";
    case ST_ARTIFACT_MATERIALIZE_ERR_INVALID_BUNDLE:
        return "invalid artifact bundle";
    case ST_ARTIFACT_MATERIALIZE_ERR_UNSUPPORTED_PLATFORM:
        return "atomic no-replace publication is unsupported";
    case ST_ARTIFACT_MATERIALIZE_ERR_COLLISION:
        return "profile already exists";
    case ST_ARTIFACT_MATERIALIZE_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_ARTIFACT_MATERIALIZE_ERR_IO: return "I/O error";
    case ST_ARTIFACT_MATERIALIZE_ERR_DURABILITY:
        return "profile committed but directory synchronization failed";
    }
    return "unknown artifact materialization status";
}
