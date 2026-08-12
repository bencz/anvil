#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "st_aot_toolchain.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef O_NOFOLLOW
#define ST_AOT_TOOLCHAIN_LACKS_NOFOLLOW 1
#define O_NOFOLLOW 0
#endif

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif

#define ST_MANIFEST_LIMIT (16u * 1024u * 1024u)
#define ST_ARTIFACT_LIMIT (256u * 1024u * 1024u)

typedef enum {
    ARTIFACT_METHOD = 0,
    ARTIFACT_METADATA,
    ARTIFACT_LAUNCH
} artifact_kind_t;

typedef struct {
    artifact_kind_t kind;
    uint32_t method_id;
    char *name;
    char *symbol;
    size_t size;
    uint8_t hash[ST_ARTIFACT_SHA256_SIZE];
} artifact_record_t;

typedef struct {
    char *text;
    size_t text_length;
    artifact_record_t *artifacts;
    size_t artifact_count;
    size_t block_count;
} profile_manifest_t;

static atomic_uint_fast64_t staging_sequence = ATOMIC_VAR_INIT(0);

static bool add_size(size_t left, size_t right, size_t *result)
{
    if (left > SIZE_MAX - right) return false;
    *result = left + right;
    return true;
}

static char *copy_string(const char *source)
{
    size_t length;
    char *copy;
    if (source == NULL) return NULL;
    length = strlen(source);
    if (length == SIZE_MAX) return NULL;
    copy = malloc(length + 1u);
    if (copy != NULL) memcpy(copy, source, length + 1u);
    return copy;
}

static bool strict_component(const char *name)
{
    size_t index, length;
    if (name == NULL || name[0] == '\0' || name[0] == '.') return false;
    length = strlen(name);
    if (length > 200u) return false;
    for (index = 0u; index < length; index++) {
        unsigned char byte = (unsigned char)name[index];
        if (!((byte >= 'A' && byte <= 'Z')
                || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9')
                || byte == '_' || byte == '-' || byte == '.'))
            return false;
    }
    return true;
}

static bool strict_argument(const char *argument)
{
    const unsigned char *cursor = (const unsigned char *)argument;
    if (cursor == NULL || *cursor == '\0') return false;
    while (*cursor != '\0') {
        if (*cursor == '\r' || *cursor == '\n') return false;
        cursor++;
    }
    return true;
}

static bool portable_symbol(const char *symbol)
{
    const unsigned char *cursor = (const unsigned char *)symbol;
    if (cursor == NULL
            || !(('A' <= *cursor && *cursor <= 'Z')
                 || ('a' <= *cursor && *cursor <= 'z')
                 || *cursor == '_'))
        return false;
    for (cursor++; *cursor != '\0'; cursor++)
        if (!(('A' <= *cursor && *cursor <= 'Z')
                || ('a' <= *cursor && *cursor <= 'z')
                || ('0' <= *cursor && *cursor <= '9')
                || *cursor == '_'))
            return false;
    return true;
}

static bool has_suffix(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length
        && memcmp(text + text_length - suffix_length,
                  suffix, suffix_length) == 0;
}

static int open_directory_no_symlinks(const char *path)
{
#ifdef ST_AOT_TOOLCHAIN_LACKS_NOFOLLOW
    (void)path;
    errno = ENOTSUP;
    return -1;
#else
    const char *cursor;
    int current;
    if (path == NULL || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    cursor = path;
    current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
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
        if (length == 0u || length > NAME_MAX
                || (length == 1u && start[0] == '.')
                || (length == 2u && start[0] == '.' && start[1] == '.')) {
            close(current);
            errno = EINVAL;
            return -1;
        }
        memcpy(component, start, length);
        component[length] = '\0';
        next = openat(current, component,
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        saved_error = errno;
        close(current);
        if (next < 0) {
            errno = saved_error;
            return -1;
        }
        current = next;
    }
    return current;
#endif
}

static bool read_all_fd(int file, size_t limit, char **bytes_out,
                        size_t *length_out)
{
    struct stat info;
    char *bytes;
    size_t length, offset = 0u;
    if (fstat(file, &info) != 0) return false;
    if (!S_ISREG(info.st_mode) || info.st_size < 0) {
        errno = EINVAL;
        return false;
    }
    if ((uintmax_t)info.st_size > limit) {
        errno = EFBIG;
        return false;
    }
    length = (size_t)info.st_size;
    if (length == SIZE_MAX) {
        errno = EOVERFLOW;
        return false;
    }
    bytes = malloc(length + 1u);
    if (bytes == NULL) return false;
    while (offset < length) {
        ssize_t amount = read(file, bytes + offset, length - offset);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) {
            int saved_error = amount == 0 ? EIO : errno;
            free(bytes);
            errno = saved_error;
            return false;
        }
        offset += (size_t)amount;
    }
    bytes[length] = '\0';
    *bytes_out = bytes;
    *length_out = length;
    return true;
}

static bool write_all(int file, const void *bytes, size_t length)
{
    const unsigned char *cursor = bytes;
    while (length != 0u) {
        ssize_t amount = write(file, cursor, length);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0 || (size_t)amount > length) {
            if (amount == 0) errno = EIO;
            return false;
        }
        cursor += (size_t)amount;
        length -= (size_t)amount;
    }
    return true;
}

static bool parse_size(const char *text, size_t *value_out)
{
    uintmax_t value = 0u;
    const unsigned char *cursor = (const unsigned char *)text;
    if (*cursor == '\0') return false;
    while (*cursor != '\0') {
        unsigned digit;
        if (*cursor < '0' || *cursor > '9') return false;
        digit = (unsigned)(*cursor - '0');
        if (value > (UINTMAX_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
        cursor++;
    }
    if (value > SIZE_MAX) return false;
    *value_out = (size_t)value;
    return true;
}

static bool parse_u32(const char *text, uint32_t *value_out)
{
    size_t value;
    if (!parse_size(text, &value) || value > UINT32_MAX) return false;
    *value_out = (uint32_t)value;
    return true;
}

static int hex_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return (int)(byte - '0');
    if (byte >= 'a' && byte <= 'f') return 10 + (int)(byte - 'a');
    return -1;
}

static bool parse_hash(const char *text,
                       uint8_t hash[ST_ARTIFACT_SHA256_SIZE])
{
    size_t index;
    if (strlen(text) != ST_ARTIFACT_SHA256_SIZE * 2u) return false;
    for (index = 0u; index < ST_ARTIFACT_SHA256_SIZE; index++) {
        int high = hex_value((unsigned char)text[index * 2u]);
        int low = hex_value((unsigned char)text[index * 2u + 1u]);
        if (high < 0 || low < 0) return false;
        hash[index] = (uint8_t)((unsigned)high * 16u + (unsigned)low);
    }
    return true;
}

static char *next_line(char **cursor)
{
    char *line, *end;
    if (cursor == NULL || *cursor == NULL || **cursor == '\0') return NULL;
    line = *cursor;
    end = strchr(line, '\n');
    if (end == NULL) return NULL;
    *end = '\0';
    *cursor = end + 1u;
    return line;
}

static char *required_value(char **cursor, const char *key)
{
    char *line = next_line(cursor);
    size_t length = strlen(key);
    if (line == NULL || memcmp(line, key, length) != 0
            || line[length] != '=')
        return NULL;
    return line + length + 1u;
}

static size_t split_fields(char *text, char **fields, size_t capacity)
{
    size_t count = 0u;
    char *cursor = text;
    while (count < capacity) {
        char *separator;
        fields[count++] = cursor;
        separator = strchr(cursor, '|');
        if (separator == NULL) break;
        *separator = '\0';
        cursor = separator + 1u;
    }
    if (strchr(cursor, '|') != NULL) return capacity + 1u;
    return count;
}

static bool parse_artifact(char *line, artifact_record_t *artifact)
{
    char *fields[6];
    size_t count = split_fields(line, fields, 6u);
    if (count != 6u || !parse_u32(fields[1], &artifact->method_id)
            || !strict_component(fields[2])
            || !has_suffix(fields[2], ".s")
            || !portable_symbol(fields[3])
            || !parse_size(fields[4], &artifact->size)
            || artifact->size == 0u || artifact->size > ST_ARTIFACT_LIMIT
            || !parse_hash(fields[5], artifact->hash))
        return false;
    if (strcmp(fields[0], "method") == 0) {
        artifact->kind = ARTIFACT_METHOD;
        if (artifact->method_id == 0u) return false;
    } else if (strcmp(fields[0], "metadata") == 0) {
        artifact->kind = ARTIFACT_METADATA;
        if (artifact->method_id != 0u
                || strcmp(fields[2], "metadata.s") != 0
                || !has_suffix(fields[3], "_descriptor"))
            return false;
    } else if (strcmp(fields[0], "launch") == 0) {
        artifact->kind = ARTIFACT_LAUNCH;
        if (artifact->method_id != 0u
                || strcmp(fields[2], "launch.s") != 0
                || !has_suffix(fields[3], "_launch_descriptor"))
            return false;
    } else {
        return false;
    }
    artifact->name = fields[2];
    artifact->symbol = fields[3];
    return true;
}

static bool parse_block_line(char *line)
{
    char *fields[11];
    uint32_t u32;
    size_t value;
    if (split_fields(line, fields, 11u) != 11u
            || !parse_u32(fields[0], &u32) || u32 == 0u
            || !parse_u32(fields[1], &u32)
            || !portable_symbol(fields[2])
            || !portable_symbol(fields[3])
            || !portable_symbol(fields[4]))
        return false;
    for (value = 5u; value < 9u; value++)
        if (!parse_u32(fields[value], &u32)) return false;
    return parse_size(fields[9], &value) && parse_size(fields[10], &value);
}

static void manifest_destroy(profile_manifest_t *manifest)
{
    if (manifest == NULL) return;
    free(manifest->artifacts);
    free(manifest->text);
    memset(manifest, 0, sizeof(*manifest));
}

static st_aot_toolchain_status_t manifest_load(
    int profile_directory, profile_manifest_t *manifest, int *error_out)
{
    int file;
    char *cursor, *value;
    size_t index, other, metadata_count = 0u, launch_count = 0u;
    uint8_t bundle_hash[ST_ARTIFACT_SHA256_SIZE];
    memset(manifest, 0, sizeof(*manifest));
    file = openat(profile_directory, "bundle.manifest",
                  O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) {
        *error_out = errno;
        return ST_AOT_TOOLCHAIN_ERR_IO;
    }
    if (!read_all_fd(file, ST_MANIFEST_LIMIT, &manifest->text,
                     &manifest->text_length)) {
        *error_out = errno;
        close(file);
        return errno == ENOMEM ? ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY
                               : ST_AOT_TOOLCHAIN_ERR_IO;
    }
    if (close(file) != 0) {
        *error_out = errno;
        manifest_destroy(manifest);
        return ST_AOT_TOOLCHAIN_ERR_IO;
    }
    if (manifest->text_length == 0u
            || manifest->text[manifest->text_length - 1u] != '\n'
            || memchr(manifest->text, '\0', manifest->text_length) != NULL
            || memchr(manifest->text, '\r', manifest->text_length) != NULL) {
        manifest_destroy(manifest);
        return ST_AOT_TOOLCHAIN_ERR_INVALID_MANIFEST;
    }
    cursor = manifest->text;
    value = next_line(&cursor);
    if (value == NULL
            || strcmp(value, "anvil-smalltalk-artifact-bundle-v2") != 0
            || (value = required_value(&cursor, "target")) == NULL
            || strcmp(value, "x86_64") != 0
            || (value = required_value(&cursor, "abi")) == NULL
            || strcmp(value, "sysv") != 0
            || (value = required_value(&cursor, "syntax")) == NULL
            || strcmp(value, "gas") != 0
            || (value = required_value(&cursor, "optimization")) == NULL
            || strcmp(value, "O2") != 0
            || (value = required_value(&cursor, "metadata-abi")) == NULL
            || strcmp(value, "5") != 0
            || (value = required_value(&cursor, "launch")) == NULL
            || strcmp(value, "present") != 0
            || (value = required_value(&cursor, "artifact-count")) == NULL
            || !parse_size(value, &manifest->artifact_count)
            || manifest->artifact_count < 2u
            || manifest->artifact_count > manifest->text_length / 20u
            || (value = required_value(&cursor, "block-count")) == NULL
            || !parse_size(value, &manifest->block_count)
            || (value = required_value(&cursor, "bundle-sha256")) == NULL
            || !parse_hash(value, bundle_hash)) {
        manifest_destroy(manifest);
        return ST_AOT_TOOLCHAIN_ERR_UNSUPPORTED_PROFILE;
    }
    manifest->artifacts = calloc(manifest->artifact_count,
                                 sizeof(*manifest->artifacts));
    if (manifest->artifacts == NULL) {
        manifest_destroy(manifest);
        return ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
    }
    for (index = 0u; index < manifest->artifact_count; index++) {
        char *line = required_value(&cursor, "artifact");
        artifact_record_t *artifact = &manifest->artifacts[index];
        if (line == NULL || !parse_artifact(line, artifact)) {
            manifest_destroy(manifest);
            return ST_AOT_TOOLCHAIN_ERR_INVALID_MANIFEST;
        }
        if (artifact->kind == ARTIFACT_METADATA) metadata_count++;
        if (artifact->kind == ARTIFACT_LAUNCH) launch_count++;
        for (other = 0u; other < index; other++)
            if (strcmp(manifest->artifacts[other].name, artifact->name) == 0
                    || strcmp(manifest->artifacts[other].symbol,
                              artifact->symbol) == 0) {
                manifest_destroy(manifest);
                return ST_AOT_TOOLCHAIN_ERR_INVALID_MANIFEST;
            }
    }
    for (index = 0u; index < manifest->block_count; index++) {
        char *line = required_value(&cursor, "block");
        if (line == NULL || !parse_block_line(line)) {
            manifest_destroy(manifest);
            return ST_AOT_TOOLCHAIN_ERR_INVALID_MANIFEST;
        }
    }
    if (*cursor != '\0' || metadata_count != 1u || launch_count != 1u
            || manifest->artifacts[manifest->artifact_count - 2u].kind
                   != ARTIFACT_METADATA
            || manifest->artifacts[manifest->artifact_count - 1u].kind
                   != ARTIFACT_LAUNCH) {
        manifest_destroy(manifest);
        return ST_AOT_TOOLCHAIN_ERR_INVALID_MANIFEST;
    }
    return ST_AOT_TOOLCHAIN_OK;
}

static bool copy_authenticated_artifact(
    int source_directory, int staging_directory,
    const artifact_record_t *artifact)
{
    int source = -1, destination = -1, saved_error = 0;
    char *bytes = NULL;
    size_t length = 0u;
    uint8_t hash[ST_ARTIFACT_SHA256_SIZE];
    bool success = false;
    source = openat(source_directory, artifact->name,
                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0) return false;
    if (!read_all_fd(source, ST_ARTIFACT_LIMIT, &bytes, &length))
        goto cleanup;
    if (length != artifact->size
            || !st_artifact_sha256(bytes, length, hash)
            || memcmp(hash, artifact->hash, sizeof(hash)) != 0) {
        errno = EBADMSG;
        goto cleanup;
    }
    destination = openat(staging_directory, artifact->name,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (destination < 0 || !write_all(destination, bytes, length)
            || fchmod(destination, 0644) != 0 || fsync(destination) != 0)
        goto cleanup;
    success = true;
cleanup:
    saved_error = errno;
    if (source >= 0 && close(source) != 0 && success) {
        success = false;
        saved_error = errno;
    }
    if (destination >= 0 && close(destination) != 0 && success) {
        success = false;
        saved_error = errno;
    }
    free(bytes);
    errno = saved_error;
    return success;
}

static bool copy_runtime_input(int staging_directory, const char *source_path,
                               size_t index, char output_name[40])
{
    int source = -1, destination = -1, saved_error = 0;
    struct stat info;
    unsigned char buffer[65536];
    bool success = false;
    const char *suffix = has_suffix(source_path, ".a") ? ".a" : ".o";
    if (source_path[0] != '/') {
        errno = EINVAL;
        return false;
    }
    source = open(source_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0 || fstat(source, &info) != 0 || !S_ISREG(info.st_mode))
        goto cleanup;
    if (snprintf(output_name, 40u, "runtime-%08zu%s", index, suffix) < 0) {
        errno = EOVERFLOW;
        goto cleanup;
    }
    destination = openat(staging_directory, output_name,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (destination < 0) goto cleanup;
    for (;;) {
        ssize_t amount = read(source, buffer, sizeof(buffer));
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) goto cleanup;
        if (amount == 0) break;
        if (!write_all(destination, buffer, (size_t)amount)) goto cleanup;
    }
    if (fchmod(destination, 0644) != 0 || fsync(destination) != 0)
        goto cleanup;
    success = true;
cleanup:
    saved_error = errno;
    if (source >= 0 && close(source) != 0 && success) {
        success = false;
        saved_error = errno;
    }
    if (destination >= 0 && close(destination) != 0 && success) {
        success = false;
        saved_error = errno;
    }
    errno = saved_error;
    return success;
}

static char *join_path(const char *left, const char *right)
{
    size_t left_length = strlen(left), right_length = strlen(right), length;
    char *result;
    bool separator = left_length != 0u && left[left_length - 1u] != '/';
    if (!add_size(left_length, separator ? 1u : 0u, &length)
            || !add_size(length, right_length, &length)
            || length == SIZE_MAX)
        return NULL;
    result = malloc(length + 1u);
    if (result == NULL) return NULL;
    memcpy(result, left, left_length);
    if (separator) result[left_length++] = '/';
    memcpy(result + left_length, right, right_length + 1u);
    return result;
}

static bool record_argv(st_aot_toolchain_result_t *result,
                        char *const argv[])
{
    size_t count = 0u, index;
    while (argv[count] != NULL) count++;
    result->failed_argv = calloc(count + 1u, sizeof(*result->failed_argv));
    if (result->failed_argv == NULL) return false;
    for (index = 0u; index < count; index++) {
        result->failed_argv[index] = copy_string(argv[index]);
        if (result->failed_argv[index] == NULL) return false;
        result->failed_argc++;
    }
    return true;
}

static bool capture_stderr(int staging_directory,
                           st_aot_toolchain_result_t *result)
{
    int file = openat(staging_directory, ".tool-stderr",
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    char *bytes = NULL;
    size_t length = 0u;
    if (file < 0) return false;
    if (!read_all_fd(file, ST_ARTIFACT_LIMIT, &bytes, &length)) {
        int saved_error = errno;
        close(file);
        errno = saved_error;
        return false;
    }
    if (close(file) != 0) {
        int saved_error = errno;
        free(bytes);
        errno = saved_error;
        return false;
    }
    result->tool_stderr = bytes;
    result->tool_stderr_length = length;
    return true;
}

static st_aot_toolchain_status_t run_tool(
    st_aot_toolchain_result_t *result, int staging_directory,
    st_aot_toolchain_stage_t stage, char *const argv[])
{
    posix_spawn_file_actions_t actions;
    pid_t child;
    int error_file = -1, spawn_status, wait_status;
    bool actions_initialized = false;
    error_file = openat(staging_directory, ".tool-stderr",
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (error_file < 0) {
        result->system_error = errno;
        return ST_AOT_TOOLCHAIN_ERR_IO;
    }
    spawn_status = posix_spawn_file_actions_init(&actions);
    if (spawn_status == 0) {
        actions_initialized = true;
        spawn_status = posix_spawn_file_actions_adddup2(
            &actions, error_file, STDERR_FILENO);
    }
    if (spawn_status == 0)
        spawn_status = posix_spawn_file_actions_addclose(&actions, error_file);
    if (spawn_status == 0)
        spawn_status = posix_spawnp(&child, argv[0], &actions, NULL,
                                    argv, environ);
    if (actions_initialized) posix_spawn_file_actions_destroy(&actions);
    close(error_file);
    if (spawn_status != 0) {
        result->failed_stage = stage;
        result->system_error = spawn_status;
        if (!record_argv(result, argv))
            return ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
        (void)capture_stderr(staging_directory, result);
        return ST_AOT_TOOLCHAIN_ERR_SPAWN;
    }
    do {
        spawn_status = waitpid(child, &wait_status, 0) < 0 ? errno : 0;
    } while (spawn_status == EINTR);
    if (spawn_status != 0) {
        result->failed_stage = stage;
        result->system_error = spawn_status;
        if (!record_argv(result, argv))
            return ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
        (void)capture_stderr(staging_directory, result);
        return ST_AOT_TOOLCHAIN_ERR_IO;
    }
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        result->failed_stage = stage;
        result->child_exit_code = WIFEXITED(wait_status)
            ? WEXITSTATUS(wait_status) : -1;
        result->child_signal = WIFSIGNALED(wait_status)
            ? WTERMSIG(wait_status) : 0;
        if (!record_argv(result, argv)
                || !capture_stderr(staging_directory, result))
            return errno == ENOMEM ? ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY
                                   : ST_AOT_TOOLCHAIN_ERR_IO;
        return ST_AOT_TOOLCHAIN_ERR_TOOL_FAILED;
    }
    if (unlinkat(staging_directory, ".tool-stderr", 0) != 0)
        return ST_AOT_TOOLCHAIN_ERR_IO;
    return ST_AOT_TOOLCHAIN_OK;
}

static void cleanup_staging(int output_directory, int staging_directory,
                            const char *staging_name)
{
    int duplicate = dup(staging_directory);
    DIR *stream;
    struct dirent *entry;
    if (duplicate < 0) return;
    stream = fdopendir(duplicate);
    if (stream == NULL) {
        close(duplicate);
        return;
    }
    while ((entry = readdir(stream)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0
                && strcmp(entry->d_name, "..") != 0)
            (void)unlinkat(staging_directory, entry->d_name, 0);
    }
    closedir(stream);
    (void)unlinkat(output_directory, staging_name, AT_REMOVEDIR);
}

static int publish_noreplace(int directory, const char *staging,
                             const char *publication)
{
#ifdef SYS_renameat2
    return (int)syscall(SYS_renameat2, directory, staging,
                        directory, publication, RENAME_NOREPLACE);
#else
    (void)directory;
    (void)staging;
    (void)publication;
    errno = ENOSYS;
    return -1;
#endif
}

static bool options_valid(const st_aot_toolchain_options_t *options,
                          bool *tool_allowed_out)
{
    size_t index;
    *tool_allowed_out = false;
    if (options == NULL || options->profile_directory == NULL
            || options->profile_directory[0] != '/'
            || options->output_directory == NULL
            || options->output_directory[0] != '/'
            || !strict_component(options->publication_name)
            || !strict_component(options->executable_name)
            || options->compiler_driver == NULL
            || options->compiler_driver[0] != '/'
            || !strict_argument(options->compiler_driver)
            || options->allowed_compiler_driver_count == 0u
            || options->allowed_compiler_drivers == NULL
            || (options->runtime_link_input_count != 0u
                && options->runtime_link_inputs == NULL)
            || (options->link_argument_count != 0u
                && options->link_arguments == NULL))
        return false;
    for (index = 0u; index < options->allowed_compiler_driver_count; index++) {
        const char *allowed = options->allowed_compiler_drivers[index];
        if (!strict_argument(allowed)) return false;
        if (strcmp(allowed, options->compiler_driver) == 0)
            *tool_allowed_out = true;
    }
    for (index = 0u; index < options->runtime_link_input_count; index++)
        if (!strict_argument(options->runtime_link_inputs[index])
                || options->runtime_link_inputs[index][0] != '/')
            return false;
    for (index = 0u; index < options->link_argument_count; index++)
        if (!strict_argument(options->link_arguments[index])) return false;
    return true;
}

void st_aot_toolchain_result_init(st_aot_toolchain_result_t *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));
}

void st_aot_toolchain_result_destroy(st_aot_toolchain_result_t *result)
{
    size_t index;
    if (result == NULL) return;
    for (index = 0u; index < result->failed_argc; index++)
        free(result->failed_argv[index]);
    free(result->failed_argv);
    free(result->tool_stderr);
    free(result->published_directory);
    free(result->published_executable);
    memset(result, 0, sizeof(*result));
}

static bool result_empty(const st_aot_toolchain_result_t *result)
{
    return result != NULL && result->status == ST_AOT_TOOLCHAIN_OK
        && result->failed_stage == ST_AOT_TOOLCHAIN_STAGE_NONE
        && result->system_error == 0 && result->child_exit_code == 0
        && result->child_signal == 0 && !result->committed
        && result->failed_argv == NULL && result->failed_argc == 0u
        && result->tool_stderr == NULL && result->tool_stderr_length == 0u
        && result->published_directory == NULL
        && result->published_executable == NULL
        && result->implementation == NULL;
}

st_aot_toolchain_status_t st_aot_toolchain_link(
    st_aot_toolchain_result_t *result,
    const st_aot_toolchain_options_t *options)
{
    profile_manifest_t manifest;
    st_aot_toolchain_status_t status;
    bool tool_allowed;
    int profile_directory = -1, output_directory = -1, staging_directory = -1;
    int saved_error = 0;
    char staging_name[96];
    char **runtime_names = NULL;
    char **object_names = NULL;
    char **argv = NULL;
    char *published_directory = NULL;
    char *published_executable = NULL;
    size_t index, argc;
    bool staging_created = false;
    memset(&manifest, 0, sizeof(manifest));
    if (!result_empty(result) || !options_valid(options, &tool_allowed)) {
        if (result != NULL)
            result->status = ST_AOT_TOOLCHAIN_ERR_INVALID_ARGUMENT;
        return ST_AOT_TOOLCHAIN_ERR_INVALID_ARGUMENT;
    }
    if (!tool_allowed) {
        result->status = ST_AOT_TOOLCHAIN_ERR_TOOL_NOT_ALLOWED;
        return result->status;
    }
    profile_directory = open_directory_no_symlinks(
        options->profile_directory);
    output_directory = open_directory_no_symlinks(
        options->output_directory);
    if (profile_directory < 0 || output_directory < 0) {
        result->system_error = errno;
        status = ST_AOT_TOOLCHAIN_ERR_IO;
        goto cleanup;
    }
    status = manifest_load(profile_directory, &manifest, &saved_error);
    if (status != ST_AOT_TOOLCHAIN_OK) {
        result->system_error = saved_error;
        goto cleanup;
    }
    if (snprintf(staging_name, sizeof(staging_name),
                 ".anvil-st-aot-link-%ld-%" PRIuFAST64,
                 (long)getpid(), atomic_fetch_add_explicit(
                     &staging_sequence, 1u, memory_order_relaxed)) < 0) {
        status = ST_AOT_TOOLCHAIN_ERR_OVERFLOW;
        goto cleanup;
    }
    if (mkdirat(output_directory, staging_name, 0700) != 0) {
        result->system_error = errno;
        status = errno == EEXIST ? ST_AOT_TOOLCHAIN_ERR_COLLISION
                                 : ST_AOT_TOOLCHAIN_ERR_IO;
        goto cleanup;
    }
    staging_created = true;
    staging_directory = openat(output_directory, staging_name,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (staging_directory < 0) {
        result->system_error = errno;
        status = ST_AOT_TOOLCHAIN_ERR_IO;
        goto cleanup;
    }
    object_names = calloc(manifest.artifact_count, sizeof(*object_names));
    runtime_names = calloc(options->runtime_link_input_count,
                           sizeof(*runtime_names));
    if (object_names == NULL
            || (runtime_names == NULL
                && options->runtime_link_input_count != 0u)) {
        status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    for (index = 0u; index < manifest.artifact_count; index++) {
        char object_name[40];
        char *source_path = NULL, *object_path = NULL;
        if (!copy_authenticated_artifact(profile_directory,
                                          staging_directory,
                                          &manifest.artifacts[index])) {
            result->system_error = errno;
            status = errno == EBADMSG
                ? ST_AOT_TOOLCHAIN_ERR_ARTIFACT_MISMATCH
                : errno == ENOMEM ? ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY
                                  : ST_AOT_TOOLCHAIN_ERR_IO;
            goto cleanup;
        }
        if (snprintf(object_name, sizeof(object_name),
                     "artifact-%08zu.o", index) < 0) {
            status = ST_AOT_TOOLCHAIN_ERR_OVERFLOW;
            goto cleanup;
        }
        object_names[index] = copy_string(object_name);
        source_path = join_path(options->output_directory, staging_name);
        object_path = source_path != NULL
            ? join_path(source_path, object_name) : NULL;
        if (source_path != NULL) {
            char *temporary = join_path(source_path,
                                        manifest.artifacts[index].name);
            free(source_path);
            source_path = temporary;
        }
        if (object_names[index] == NULL || source_path == NULL
                || object_path == NULL) {
            free(source_path);
            free(object_path);
            status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        argv = calloc(7u, sizeof(*argv));
        if (argv == NULL) {
            free(source_path);
            free(object_path);
            status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
        argv[0] = (char *)options->compiler_driver;
        argv[1] = "-c";
        argv[2] = "-x";
        argv[3] = "assembler";
        argv[4] = source_path;
        argv[5] = "-o";
        argv[6] = object_path;
        {
            char **expanded = realloc(argv, 8u * sizeof(*argv));
            if (expanded == NULL) {
                free(source_path);
                free(object_path);
                free(argv);
                argv = NULL;
                status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
                goto cleanup;
            }
            argv = expanded;
            argv[7] = NULL;
        }
        status = run_tool(result, staging_directory,
                          ST_AOT_TOOLCHAIN_STAGE_ASSEMBLE, argv);
        free(source_path);
        free(object_path);
        free(argv);
        argv = NULL;
        if (status != ST_AOT_TOOLCHAIN_OK) goto cleanup;
    }
    for (index = 0u; index < options->runtime_link_input_count; index++) {
        char runtime_name[40];
        if (!copy_runtime_input(staging_directory,
                                options->runtime_link_inputs[index], index,
                                runtime_name)) {
            result->system_error = errno;
            status = errno == ENOMEM ? ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY
                                     : ST_AOT_TOOLCHAIN_ERR_IO;
            goto cleanup;
        }
        runtime_names[index] = copy_string(runtime_name);
        if (runtime_names[index] == NULL) {
            status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }
    if (!add_size(1u, manifest.artifact_count, &argc)
            || !add_size(argc, options->runtime_link_input_count, &argc)
            || !add_size(argc, options->link_argument_count, &argc)
            || !add_size(argc, 3u, &argc)) {
        status = ST_AOT_TOOLCHAIN_ERR_OVERFLOW;
        goto cleanup;
    }
    argv = calloc(argc, sizeof(*argv));
    if (argv == NULL) {
        status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    argc = 0u;
    argv[argc++] = (char *)options->compiler_driver;
    for (index = 0u; index < manifest.artifact_count; index++) {
        char *directory = join_path(options->output_directory, staging_name);
        argv[argc] = directory != NULL
            ? join_path(directory, object_names[index]) : NULL;
        free(directory);
        if (argv[argc++] == NULL) {
            status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }
    for (index = 0u; index < options->runtime_link_input_count; index++) {
        char *directory = join_path(options->output_directory, staging_name);
        argv[argc] = directory != NULL
            ? join_path(directory, runtime_names[index]) : NULL;
        free(directory);
        if (argv[argc++] == NULL) {
            status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }
    for (index = 0u; index < options->link_argument_count; index++)
        argv[argc++] = (char *)options->link_arguments[index];
    argv[argc++] = "-o";
    {
        char *directory = join_path(options->output_directory, staging_name);
        argv[argc] = directory != NULL
            ? join_path(directory, options->executable_name) : NULL;
        free(directory);
        if (argv[argc++] == NULL) {
            status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
            goto cleanup;
        }
    }
    argv[argc] = NULL;
    status = run_tool(result, staging_directory,
                      ST_AOT_TOOLCHAIN_STAGE_LINK, argv);
    if (status != ST_AOT_TOOLCHAIN_OK) goto cleanup;
    published_directory = join_path(options->output_directory,
                                    options->publication_name);
    published_executable = published_directory != NULL
        ? join_path(published_directory, options->executable_name) : NULL;
    if (published_directory == NULL || published_executable == NULL) {
        status = ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (fsync(staging_directory) != 0) {
        result->system_error = errno;
        status = ST_AOT_TOOLCHAIN_ERR_DURABILITY;
        goto cleanup;
    }
    if (publish_noreplace(output_directory, staging_name,
                          options->publication_name) != 0) {
        result->system_error = errno;
        status = errno == EEXIST ? ST_AOT_TOOLCHAIN_ERR_COLLISION
                                 : ST_AOT_TOOLCHAIN_ERR_IO;
        goto cleanup;
    }
    staging_created = false;
    result->committed = true;
    result->published_directory = published_directory;
    result->published_executable = published_executable;
    published_directory = NULL;
    published_executable = NULL;
    if (fsync(output_directory) != 0) {
        result->system_error = errno;
        status = ST_AOT_TOOLCHAIN_ERR_DURABILITY;
        goto cleanup;
    }
    status = ST_AOT_TOOLCHAIN_OK;
cleanup:
    if (argv != NULL) {
        size_t first_owned = 1u;
        size_t last_owned = 1u + manifest.artifact_count
            + options->runtime_link_input_count;
        for (index = first_owned; index < last_owned; index++)
            free(argv[index]);
        if (last_owned + options->link_argument_count + 1u < argc)
            free(argv[last_owned + options->link_argument_count + 1u]);
        free(argv);
    }
    if (object_names != NULL)
        for (index = 0u; index < manifest.artifact_count; index++)
            free(object_names[index]);
    if (runtime_names != NULL)
        for (index = 0u; index < options->runtime_link_input_count; index++)
            free(runtime_names[index]);
    free(object_names);
    free(runtime_names);
    free(published_directory);
    free(published_executable);
    if (staging_created && staging_directory >= 0)
        cleanup_staging(output_directory, staging_directory, staging_name);
    if (staging_directory >= 0) close(staging_directory);
    if (profile_directory >= 0) close(profile_directory);
    if (output_directory >= 0) close(output_directory);
    manifest_destroy(&manifest);
    result->status = status;
    return status;
}

const char *st_aot_toolchain_status_string(st_aot_toolchain_status_t status)
{
    switch (status) {
    case ST_AOT_TOOLCHAIN_OK: return "ok";
    case ST_AOT_TOOLCHAIN_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_AOT_TOOLCHAIN_ERR_TOOL_NOT_ALLOWED: return "tool not allowed";
    case ST_AOT_TOOLCHAIN_ERR_UNSUPPORTED_PROFILE: return "unsupported profile";
    case ST_AOT_TOOLCHAIN_ERR_INVALID_MANIFEST: return "invalid manifest";
    case ST_AOT_TOOLCHAIN_ERR_ARTIFACT_MISMATCH: return "artifact mismatch";
    case ST_AOT_TOOLCHAIN_ERR_COLLISION: return "publication collision";
    case ST_AOT_TOOLCHAIN_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_AOT_TOOLCHAIN_ERR_OVERFLOW: return "size overflow";
    case ST_AOT_TOOLCHAIN_ERR_IO: return "I/O failure";
    case ST_AOT_TOOLCHAIN_ERR_SPAWN: return "process spawn failure";
    case ST_AOT_TOOLCHAIN_ERR_TOOL_FAILED: return "external tool failed";
    case ST_AOT_TOOLCHAIN_ERR_DURABILITY: return "durability failure";
    default: return "unknown AOT toolchain status";
    }
}

const char *st_aot_toolchain_stage_string(st_aot_toolchain_stage_t stage)
{
    switch (stage) {
    case ST_AOT_TOOLCHAIN_STAGE_NONE: return "none";
    case ST_AOT_TOOLCHAIN_STAGE_ASSEMBLE: return "assemble";
    case ST_AOT_TOOLCHAIN_STAGE_LINK: return "link";
    default: return "unknown";
    }
}
