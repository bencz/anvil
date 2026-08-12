#define _POSIX_C_SOURCE 200809L

#include "st_source_bundle.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#if !defined(O_CLOEXEC) || !defined(O_DIRECTORY) || !defined(O_NOFOLLOW)
#error "source_bundle requires POSIX.1-2008 secure openat flags"
#endif

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

static void *bundle_allocate(st_source_bundle_t *bundle, size_t size)
{
    return bundle->allocator.allocate(bundle->allocator.user, size);
}

static void bundle_deallocate(st_source_bundle_t *bundle, void *pointer)
{
    if (pointer != NULL) {
        bundle->allocator.deallocate(bundle->allocator.user, pointer);
    }
}

static void diagnostic_path(st_source_diagnostic_t *diagnostic,
                            const char *path, size_t length)
{
    size_t copy_length = length;
    if (path == NULL) {
        diagnostic->path[0] = '\0';
        diagnostic->path_truncated = false;
        return;
    }
    if (copy_length >= sizeof(diagnostic->path)) {
        copy_length = sizeof(diagnostic->path) - 1u;
        diagnostic->path_truncated = true;
    } else {
        diagnostic->path_truncated = false;
    }
    if (copy_length != 0u) memcpy(diagnostic->path, path, copy_length);
    diagnostic->path[copy_length] = '\0';
}

static st_source_load_status_t set_error(st_source_bundle_t *bundle,
                                         st_source_load_status_t status,
                                         st_source_load_phase_t phase,
                                         size_t source_index,
                                         size_t manifest_line,
                                         const char *path,
                                         size_t path_length,
                                         int system_error)
{
    bundle->diagnostic.status = status;
    bundle->diagnostic.phase = phase;
    bundle->diagnostic.source_index = source_index;
    bundle->diagnostic.manifest_line = manifest_line;
    bundle->diagnostic.system_error = system_error;
    diagnostic_path(&bundle->diagnostic, path, path_length);
    return status;
}

static void source_file_destroy(st_source_bundle_t *bundle,
                                st_source_file_t *file)
{
    st_ast_unit_destroy(&file->ast);
    bundle_deallocate(bundle, file->source);
    bundle_deallocate(bundle, file->source_name);
    bundle_deallocate(bundle, file->path);
    memset(file, 0, sizeof(*file));
}

static void release_files(st_source_bundle_t *bundle)
{
    size_t index;
    for (index = 0u; index < bundle->count; index++) {
        source_file_destroy(bundle, &bundle->files[index]);
    }
    bundle_deallocate(bundle, bundle->files);
    bundle->files = NULL;
    bundle->count = 0u;
    bundle->capacity = 0u;
    bundle->image_count = 0u;
    bundle->total_source_bytes = 0u;
}

void st_source_bundle_destroy(st_source_bundle_t *bundle)
{
    if (bundle == NULL) return;
    if (bundle->allocator.allocate != NULL
            && bundle->allocator.deallocate != NULL) {
        release_files(bundle);
    }
    memset(bundle, 0, sizeof(*bundle));
}

static char *copy_string(st_source_bundle_t *bundle,
                         const char *bytes, size_t length)
{
    char *copy;
    if (length == SIZE_MAX) return NULL;
    copy = bundle_allocate(bundle, length + 1u);
    if (copy == NULL) return NULL;
    if (length != 0u) memcpy(copy, bytes, length);
    copy[length] = '\0';
    return copy;
}

static st_source_load_status_t read_entire_file(
    st_source_bundle_t *bundle, int fd, const struct stat *metadata,
    unsigned char **source_out, size_t *length_out,
    st_source_load_phase_t phase, size_t source_index, size_t manifest_line,
    const char *path, size_t max_bytes)
{
    unsigned char *bytes = NULL;
    size_t capacity;
    size_t length = 0u;
    size_t path_length = path == NULL ? 0u : strlen(path);

    *source_out = NULL;
    *length_out = 0u;
    if (max_bytes == 0u || max_bytes == SIZE_MAX) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT, phase,
                         source_index, manifest_line, path, path_length, 0);
    }
    if (metadata->st_size < 0) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_OVERFLOW, phase,
                         source_index, manifest_line, path, path_length, 0);
    }
    if ((uintmax_t)metadata->st_size >= (uintmax_t)SIZE_MAX) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_OVERFLOW, phase,
                         source_index, manifest_line, path, path_length, 0);
    }
    if ((uintmax_t)metadata->st_size > (uintmax_t)max_bytes) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED, phase,
                         source_index, manifest_line, path, path_length, 0);
    }
    capacity = (size_t)metadata->st_size + 1u;
    bytes = bundle_allocate(bundle, capacity);
    if (bytes == NULL) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY, phase,
                         source_index, manifest_line, path, path_length, 0);
    }

    for (;;) {
        ssize_t amount;
        size_t available = capacity - length - 1u;
        if (available == 0u) {
            size_t new_capacity;
            unsigned char *grown;
            if (length == max_bytes) {
                unsigned char extra;
                do {
                    amount = read(fd, &extra, 1u);
                } while (amount < 0 && errno == EINTR);
                if (amount == 0) break;
                if (amount < 0) {
                    int saved_errno = errno;
                    bundle_deallocate(bundle, bytes);
                    return set_error(bundle, ST_SOURCE_LOAD_ERR_IO, phase,
                                     source_index, manifest_line, path,
                                     path_length, saved_errno);
                }
                bundle_deallocate(bundle, bytes);
                return set_error(bundle, ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED,
                                 phase, source_index, manifest_line, path,
                                 path_length, 0);
            }
            if (capacity > SIZE_MAX / 2u) {
                bundle_deallocate(bundle, bytes);
                return set_error(bundle, ST_SOURCE_LOAD_ERR_OVERFLOW, phase,
                                 source_index, manifest_line, path,
                                 path_length, 0);
            }
            new_capacity = capacity * 2u;
            if (new_capacity > max_bytes + 1u)
                new_capacity = max_bytes + 1u;
            grown = bundle_allocate(bundle, new_capacity);
            if (grown == NULL) {
                bundle_deallocate(bundle, bytes);
                return set_error(bundle, ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY,
                                 phase, source_index, manifest_line, path,
                                 path_length, 0);
            }
            if (length != 0u) memcpy(grown, bytes, length);
            bundle_deallocate(bundle, bytes);
            bytes = grown;
            capacity = new_capacity;
            available = capacity - length - 1u;
        }
#ifdef SSIZE_MAX
        if (available > (size_t)SSIZE_MAX) available = (size_t)SSIZE_MAX;
#endif
        amount = read(fd, bytes + length, available);
        if (amount < 0) {
            int saved_errno = errno;
            if (saved_errno == EINTR) continue;
            bundle_deallocate(bundle, bytes);
            return set_error(bundle, ST_SOURCE_LOAD_ERR_IO, phase,
                             source_index, manifest_line, path, path_length,
                             saved_errno);
        }
        if (amount == 0) break;
        length += (size_t)amount;
    }
    bytes[length] = '\0';
    *source_out = bytes;
    *length_out = length;
    return ST_SOURCE_LOAD_OK;
}

static st_source_load_status_t classify_open_error(
    st_source_bundle_t *bundle, int system_error, bool protected_path,
    st_source_load_phase_t phase, size_t source_index, size_t manifest_line,
    const char *path)
{
    st_source_load_status_t status;
    if (system_error == ENOENT) {
        status = ST_SOURCE_LOAD_ERR_MISSING_FILE;
    } else if (protected_path
            && (system_error == ELOOP || system_error == ENOTDIR)) {
        status = ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL;
    } else {
        status = ST_SOURCE_LOAD_ERR_IO;
    }
    return set_error(bundle, status, phase, source_index, manifest_line,
                     path, path == NULL ? 0u : strlen(path), system_error);
}

static bool manifest_path_is_safe(const char *path, size_t length)
{
    size_t component_start = 0u;
    size_t index;
    if (length == 0u || path[0] == '/' || path[0] == '\\') return false;
    if (length >= 2u
            && ((path[0] >= 'A' && path[0] <= 'Z')
                || (path[0] >= 'a' && path[0] <= 'z'))
            && path[1] == ':') {
        return false;
    }
    for (index = 0u; index <= length; index++) {
        bool at_end = index == length;
        unsigned char byte = at_end ? (unsigned char)'/'
                                    : (unsigned char)path[index];
        if (!at_end && (byte == '\0' || byte == '\\'
                || byte < 0x20u || byte == 0x7fu)) {
            return false;
        }
        if (byte == '/') {
            size_t component_length = index - component_start;
            if (component_length == 0u
                    || (component_length == 1u
                        && path[component_start] == '.')
                    || (component_length == 2u
                        && path[component_start] == '.'
                        && path[component_start + 1u] == '.')) {
                return false;
            }
            component_start = index + 1u;
        }
    }
    return true;
}

/* Opens a caller-supplied root without following any path component.  A
 * single leading slash selects the filesystem root; a relative path starts at
 * the current directory.  The exact path "." is accepted, while embedded
 * '.', '..', empty components and repeated slashes are rejected. */
static int open_protected_directory(const char *path)
{
    const char *cursor;
    int directory_fd;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(path, ".") == 0) {
        return open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (path[0] == '/') {
        directory_fd = open(
            "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        cursor = path + 1u;
        if (*cursor == '\0') return directory_fd;
    } else {
        directory_fd = open(
            ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        cursor = path;
    }
    if (directory_fd < 0) return -1;
    while (*cursor != '\0') {
        const char *slash = strchr(cursor, '/');
        size_t length = slash == NULL ? strlen(cursor)
                                      : (size_t)(slash - cursor);
        char component[NAME_MAX + 1u];
        int next_fd;

        if (length == 0u || length > NAME_MAX
                || (length == 1u && cursor[0] == '.')
                || (length == 2u && cursor[0] == '.' && cursor[1] == '.')) {
            (void)close(directory_fd);
            errno = EINVAL;
            return -1;
        }
        memcpy(component, cursor, length);
        component[length] = '\0';
        next_fd = openat(directory_fd, component,
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next_fd < 0) {
            int saved_errno = errno;
            (void)close(directory_fd);
            errno = saved_errno;
            return -1;
        }
        (void)close(directory_fd);
        directory_fd = next_fd;
        if (slash == NULL) break;
        cursor = slash + 1u;
        if (*cursor == '\0') break;
    }
    return directory_fd;
}

/* `path` is temporarily split in place; every slash is restored. */
static int open_image_source(int image_fd, char *path)
{
    int directory_fd = dup(image_fd);
    char *component = path;
    char *slash;
    if (directory_fd < 0) return -1;
    for (;;) {
        int next_fd;
        slash = strchr(component, '/');
        if (slash == NULL) break;
        *slash = '\0';
        next_fd = openat(directory_fd, component,
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        *slash = '/';
        if (next_fd < 0) {
            int saved_errno = errno;
            close(directory_fd);
            errno = saved_errno;
            return -1;
        }
        close(directory_fd);
        directory_fd = next_fd;
        component = slash + 1;
    }
    {
        int file_fd = openat(directory_fd, component,
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        int saved_errno = errno;
        close(directory_fd);
        errno = saved_errno;
        return file_fd;
    }
}

static bool same_identity(const st_source_file_t *file,
                          const struct stat *metadata)
{
    return file->device == (uintmax_t)metadata->st_dev
        && file->inode == (uintmax_t)metadata->st_ino;
}

static bool identity_already_loaded(const st_source_bundle_t *bundle,
                                    const struct stat *metadata)
{
    size_t index;
    for (index = 0u; index < bundle->count; index++) {
        if (same_identity(&bundle->files[index], metadata)) return true;
    }
    return false;
}

static bool spelling_already_loaded(const st_source_bundle_t *bundle,
                                    st_source_origin_t origin,
                                    const char *path)
{
    size_t index;
    for (index = 0u; index < bundle->count; index++) {
        const st_source_file_t *file = &bundle->files[index];
        if (file->origin == origin && strcmp(file->path, path) == 0) {
            return true;
        }
        if (strcmp(file->source_name, path) == 0) return true;
    }
    return false;
}

static st_source_load_status_t append_file(st_source_bundle_t *bundle,
                                           st_source_file_t *file,
                                           st_source_load_phase_t phase)
{
    if (bundle->count == bundle->capacity) {
        size_t new_capacity = bundle->capacity == 0u ? 8u
                                                     : bundle->capacity * 2u;
        st_source_file_t *grown;
        if (new_capacity < bundle->capacity
                || new_capacity > SIZE_MAX / sizeof(*grown)) {
            return set_error(bundle, ST_SOURCE_LOAD_ERR_OVERFLOW, phase,
                             bundle->count, file->manifest_line,
                             file->source_name, strlen(file->source_name), 0);
        }
        grown = bundle_allocate(bundle, new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return set_error(bundle, ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY, phase,
                             bundle->count, file->manifest_line,
                             file->source_name, strlen(file->source_name), 0);
        }
        if (bundle->count != 0u) {
            memcpy(grown, bundle->files,
                   bundle->count * sizeof(*bundle->files));
        }
        bundle_deallocate(bundle, bundle->files);
        bundle->files = grown;
        bundle->capacity = new_capacity;
    }
    file->ordinal = bundle->count;
    bundle->files[bundle->count++] = *file;
    memset(file, 0, sizeof(*file));
    return ST_SOURCE_LOAD_OK;
}

static st_source_load_status_t joined_source_name(
    st_source_bundle_t *bundle, const char *directory, const char *relative,
    char **name_out, st_source_load_phase_t phase, size_t source_index,
    size_t manifest_line)
{
    size_t directory_length = strlen(directory);
    size_t relative_length = strlen(relative);
    bool needs_separator = directory_length != 0u
        && directory[directory_length - 1u] != '/';
    size_t length;
    char *name;
    if (directory_length > SIZE_MAX - relative_length
            || directory_length + relative_length
                > SIZE_MAX - (needs_separator ? 1u : 0u) - 1u) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_OVERFLOW, phase,
                         source_index, manifest_line, relative,
                         relative_length, 0);
    }
    length = directory_length + relative_length + (needs_separator ? 1u : 0u);
    name = bundle_allocate(bundle, length + 1u);
    if (name == NULL) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY, phase,
                         source_index, manifest_line, relative,
                         relative_length, 0);
    }
    memcpy(name, directory, directory_length);
    if (needs_separator) name[directory_length++] = '/';
    memcpy(name + directory_length, relative, relative_length);
    name[length] = '\0';
    *name_out = name;
    return ST_SOURCE_LOAD_OK;
}

static st_source_load_status_t load_one_source(
    st_source_bundle_t *bundle, int directory_fd, st_source_origin_t origin,
    const char *source_directory, const char *path, size_t manifest_line)
{
    st_source_file_t file;
    st_source_load_phase_t phase = origin == ST_SOURCE_ORIGIN_IMAGE
        ? ST_SOURCE_PHASE_IMAGE_SOURCE : ST_SOURCE_PHASE_APPLICATION_SOURCE;
    struct stat metadata;
    st_source_load_status_t status;
    st_parser_t parser;
    int fd;
    int saved_errno;
    bool ast_initialized = false;
    size_t remaining_total;

    memset(&file, 0, sizeof(file));
    memset(&parser, 0, sizeof(parser));
    if (bundle->count >= bundle->limits.max_files) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED, phase,
                         bundle->count, manifest_line, path, strlen(path), 0);
    }
    file.origin = origin;
    file.manifest_line = manifest_line;
    file.path = copy_string(bundle, path, strlen(path));
    if (file.path == NULL) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY, phase,
                         bundle->count, manifest_line, path, strlen(path), 0);
    }
    if (source_directory != NULL) {
        status = joined_source_name(bundle, source_directory, path,
                                    &file.source_name, phase, bundle->count,
                                    manifest_line);
    } else {
        file.source_name = copy_string(bundle, path, strlen(path));
        status = file.source_name == NULL
            ? set_error(bundle, ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY, phase,
                        bundle->count, 0u, path, strlen(path), 0)
            : ST_SOURCE_LOAD_OK;
    }
    if (status != ST_SOURCE_LOAD_OK) goto failed;

    if (spelling_already_loaded(bundle, origin,
                                source_directory != NULL
                                    ? file.path : file.source_name)) {
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_DUPLICATE_SOURCE, phase,
                           bundle->count, manifest_line, file.source_name,
                           strlen(file.source_name), 0);
        goto failed;
    }

    if (directory_fd >= 0) {
        fd = open_image_source(directory_fd, file.path);
    } else {
        fd = open(file.path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    }
    if (fd < 0) {
        saved_errno = errno;
        status = classify_open_error(bundle, saved_errno,
                                     directory_fd >= 0, phase,
                                     bundle->count, manifest_line,
                                     file.source_name);
        goto failed;
    }
    if (fstat(fd, &metadata) != 0) {
        saved_errno = errno;
        close(fd);
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_IO, phase, bundle->count,
                           manifest_line, file.source_name,
                           strlen(file.source_name), saved_errno);
        goto failed;
    }
    if (!S_ISREG(metadata.st_mode)) {
        close(fd);
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_NOT_REGULAR_FILE, phase,
                           bundle->count, manifest_line, file.source_name,
                           strlen(file.source_name), 0);
        goto failed;
    }
    if (identity_already_loaded(bundle, &metadata)) {
        close(fd);
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_DUPLICATE_SOURCE, phase,
                           bundle->count, manifest_line, file.source_name,
                           strlen(file.source_name), 0);
        goto failed;
    }
    file.device = (uintmax_t)metadata.st_dev;
    file.inode = (uintmax_t)metadata.st_ino;
    if (bundle->total_source_bytes > bundle->limits.max_total_bytes) {
        close(fd);
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_OVERFLOW, phase,
                           bundle->count, manifest_line, file.source_name,
                           strlen(file.source_name), 0);
        goto failed;
    }
    remaining_total = bundle->limits.max_total_bytes
        - bundle->total_source_bytes;
    if (remaining_total > bundle->limits.max_file_bytes)
        remaining_total = bundle->limits.max_file_bytes;
    if (remaining_total == 0u) {
        close(fd);
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED, phase,
                           bundle->count, manifest_line, file.source_name,
                           strlen(file.source_name), 0);
        goto failed;
    }
    status = read_entire_file(bundle, fd, &metadata, &file.source,
                              &file.source_length, phase, bundle->count,
                              manifest_line, file.source_name,
                              remaining_total);
    saved_errno = errno;
    if (close(fd) != 0 && status == ST_SOURCE_LOAD_OK) {
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_IO, phase, bundle->count,
                           manifest_line, file.source_name,
                           strlen(file.source_name), errno);
    }
    errno = saved_errno;
    if (status != ST_SOURCE_LOAD_OK) goto failed;
    if (file.source_length > bundle->limits.max_total_bytes
            - bundle->total_source_bytes) {
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED, phase,
                           bundle->count, manifest_line, file.source_name,
                           strlen(file.source_name), 0);
        goto failed;
    }

    if (!st_ast_unit_init(&file.ast, file.source_name)) {
        st_ast_status_t ast_status = st_ast_unit_status(&file.ast);
        ast_initialized = true;
        status = set_error(bundle,
            ast_status == ST_AST_ERR_OVERFLOW ? ST_SOURCE_LOAD_ERR_OVERFLOW
                                              : ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY,
            phase, bundle->count, manifest_line, file.source_name,
            strlen(file.source_name), 0);
        goto failed;
    }
    ast_initialized = true;
    if (!st_parser_init_memory(&parser, &file.ast,
                               file.source, file.source_length)
            || !st_parse_source_unit(&parser)) {
        const st_parse_error_t *parse_error = st_parser_error(&parser);
        st_parse_status_t parse_status = st_parser_status(&parser);
        st_ast_status_t ast_status = st_ast_unit_status(&file.ast);
        if (parse_error != NULL) bundle->diagnostic.parse_error = *parse_error;
        if (ast_status == ST_AST_ERR_OVERFLOW) {
            status = ST_SOURCE_LOAD_ERR_OVERFLOW;
        } else if (ast_status == ST_AST_ERR_OUT_OF_MEMORY
                || parse_status == ST_PARSE_ERR_OUT_OF_MEMORY) {
            status = ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY;
        } else {
            status = ST_SOURCE_LOAD_ERR_PARSE;
        }
        (void)set_error(bundle, status, phase, bundle->count, manifest_line,
                        file.source_name, strlen(file.source_name), 0);
        st_parser_destroy(&parser);
        goto failed;
    }
    st_parser_destroy(&parser);
    status = append_file(bundle, &file, phase);
    if (status != ST_SOURCE_LOAD_OK) goto failed;
    bundle->total_source_bytes += bundle->files[bundle->count - 1u].source_length;
    return ST_SOURCE_LOAD_OK;

failed:
    if (ast_initialized) st_ast_unit_destroy(&file.ast);
    bundle_deallocate(bundle, file.source);
    bundle_deallocate(bundle, file.source_name);
    bundle_deallocate(bundle, file.path);
    return status;
}

static st_source_load_status_t load_manifest_sources(
    st_source_bundle_t *bundle,
    int directory_fd,
    const char *source_directory,
    st_source_origin_t origin,
    st_source_load_phase_t manifest_phase,
    const char *manifest_filename,
    const unsigned char *manifest,
    size_t manifest_length)
{
    size_t offset = 0u;
    size_t line_number = 1u;
    size_t initial_count = bundle->count;
    if (memchr(manifest, '\0', manifest_length) != NULL) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_INVALID_MANIFEST,
                         manifest_phase, bundle->count, 0u,
                         manifest_filename, strlen(manifest_filename), 0);
    }
    while (offset < manifest_length) {
        size_t begin = offset;
        size_t length;
        char *path;
        st_source_load_status_t status;
        while (offset < manifest_length && manifest[offset] != '\n') offset++;
        length = offset - begin;
        if (length != 0u && manifest[begin + length - 1u] == '\r') length--;
        if (length != 0u && manifest[begin] != '#') {
            if (!manifest_path_is_safe((const char *)manifest + begin,
                                       length)) {
                return set_error(bundle, ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL,
                                 manifest_phase, bundle->count,
                                 line_number, (const char *)manifest + begin,
                                 length, 0);
            }
            path = copy_string(bundle, (const char *)manifest + begin, length);
            if (path == NULL) {
                return set_error(bundle, ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY,
                                 manifest_phase, bundle->count,
                                 line_number, (const char *)manifest + begin,
                                 length, 0);
            }
            status = load_one_source(bundle, directory_fd,
                                     origin, source_directory,
                                     path, line_number);
            bundle_deallocate(bundle, path);
            if (status != ST_SOURCE_LOAD_OK) return status;
        }
        if (offset < manifest_length) offset++;
        if (offset < manifest_length) {
            if (line_number == SIZE_MAX) {
                return set_error(bundle, ST_SOURCE_LOAD_ERR_OVERFLOW,
                                 manifest_phase, bundle->count,
                                 line_number, NULL, 0u, 0);
            }
            line_number++;
        }
    }
    if (bundle->count == initial_count) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_INVALID_MANIFEST,
                         manifest_phase, bundle->count, 0u,
                         manifest_filename, strlen(manifest_filename), 0);
    }
    if (origin == ST_SOURCE_ORIGIN_IMAGE) {
        bundle->image_count = bundle->count;
    }
    return ST_SOURCE_LOAD_OK;
}

st_source_load_status_t st_source_bundle_load_with_limits(
    st_source_bundle_t *bundle, const char *image_directory,
    const char *const *app_paths, size_t app_count,
    const st_source_allocator_t *allocator, const st_source_limits_t *limits)
{
    st_source_load_status_t status;
    struct stat metadata;
    unsigned char *manifest = NULL;
    size_t manifest_length = 0u;
    size_t index;
    int image_fd = -1;
    int manifest_fd = -1;
    int saved_errno;

    if (bundle == NULL) return ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT;
    memset(bundle, 0, sizeof(*bundle));
    bundle->diagnostic.status = ST_SOURCE_LOAD_OK;
    if (limits == NULL || limits->max_manifest_bytes == 0u
            || limits->max_manifest_bytes == SIZE_MAX
            || limits->max_file_bytes == 0u
            || limits->max_file_bytes == SIZE_MAX
            || limits->max_total_bytes == 0u
            || limits->max_total_bytes == SIZE_MAX
            || limits->max_files == 0u) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT,
                         ST_SOURCE_PHASE_NONE, 0u, 0u, NULL, 0u, 0);
    }
    bundle->limits = *limits;
    if (allocator == NULL
            || (allocator->allocate == NULL
                && allocator->deallocate == NULL)) {
        bundle->allocator.allocate = default_allocate;
        bundle->allocator.deallocate = default_deallocate;
    } else if (allocator->allocate == NULL || allocator->deallocate == NULL) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT,
                         ST_SOURCE_PHASE_NONE, 0u, 0u, NULL, 0u, 0);
    } else {
        bundle->allocator = *allocator;
    }
    if (image_directory == NULL || image_directory[0] == '\0'
            || (app_count != 0u && app_paths == NULL)) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT,
                         ST_SOURCE_PHASE_NONE, 0u, 0u, NULL, 0u, 0);
    }
    if (app_count > SIZE_MAX / sizeof(st_source_file_t)) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_OVERFLOW,
                         ST_SOURCE_PHASE_APPLICATION_SOURCE, 0u, 0u,
                         NULL, 0u, 0);
    }
    if (app_count > bundle->limits.max_files) {
        return set_error(bundle, ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED,
                         ST_SOURCE_PHASE_APPLICATION_SOURCE, 0u, 0u,
                         NULL, 0u, 0);
    }

    image_fd = open_protected_directory(image_directory);
    if (image_fd < 0) {
        status = classify_open_error(bundle, errno, true,
                                     ST_SOURCE_PHASE_IMAGE_MANIFEST, 0u, 0u,
                                     image_directory);
        goto failed;
    }
    manifest_fd = openat(image_fd, ST_IMAGE_MANIFEST_FILENAME,
                         O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (manifest_fd < 0) {
        status = classify_open_error(bundle, errno, true,
                                     ST_SOURCE_PHASE_IMAGE_MANIFEST, 0u, 0u,
                                     ST_IMAGE_MANIFEST_FILENAME);
        goto failed;
    }
    if (fstat(manifest_fd, &metadata) != 0) {
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_IO,
                           ST_SOURCE_PHASE_IMAGE_MANIFEST, 0u, 0u,
                           ST_IMAGE_MANIFEST_FILENAME,
                           strlen(ST_IMAGE_MANIFEST_FILENAME), errno);
        goto failed;
    }
    if (!S_ISREG(metadata.st_mode)) {
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_NOT_REGULAR_FILE,
                           ST_SOURCE_PHASE_IMAGE_MANIFEST, 0u, 0u,
                           ST_IMAGE_MANIFEST_FILENAME,
                           strlen(ST_IMAGE_MANIFEST_FILENAME), 0);
        goto failed;
    }
    status = read_entire_file(bundle, manifest_fd, &metadata, &manifest,
                              &manifest_length,
                              ST_SOURCE_PHASE_IMAGE_MANIFEST, 0u, 0u,
                              ST_IMAGE_MANIFEST_FILENAME,
                              bundle->limits.max_manifest_bytes);
    saved_errno = errno;
    if (close(manifest_fd) != 0 && status == ST_SOURCE_LOAD_OK) {
        status = set_error(bundle, ST_SOURCE_LOAD_ERR_IO,
                           ST_SOURCE_PHASE_IMAGE_MANIFEST, 0u, 0u,
                           ST_IMAGE_MANIFEST_FILENAME,
                           strlen(ST_IMAGE_MANIFEST_FILENAME), errno);
    }
    manifest_fd = -1;
    errno = saved_errno;
    if (status != ST_SOURCE_LOAD_OK) goto failed;

    status = load_manifest_sources(
        bundle, image_fd, image_directory, ST_SOURCE_ORIGIN_IMAGE,
        ST_SOURCE_PHASE_IMAGE_MANIFEST, ST_IMAGE_MANIFEST_FILENAME,
        manifest, manifest_length);
    bundle_deallocate(bundle, manifest);
    manifest = NULL;
    if (status != ST_SOURCE_LOAD_OK) goto failed;

    for (index = 0u; index < app_count; index++) {
        if (app_paths[index] == NULL || app_paths[index][0] == '\0') {
            status = set_error(bundle, ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT,
                               ST_SOURCE_PHASE_APPLICATION_SOURCE,
                               bundle->count, 0u, NULL, 0u, 0);
            goto failed;
        }
        status = load_one_source(bundle, -1,
                                 ST_SOURCE_ORIGIN_APPLICATION, NULL,
                                 app_paths[index], 0u);
        if (status != ST_SOURCE_LOAD_OK) goto failed;
    }
    close(image_fd);
    return ST_SOURCE_LOAD_OK;

failed:
    saved_errno = errno;
    if (manifest_fd >= 0) close(manifest_fd);
    if (image_fd >= 0) close(image_fd);
    bundle_deallocate(bundle, manifest);
    release_files(bundle);
    errno = saved_errno;
    return status;
}

st_source_load_status_t st_source_bundle_load(
    st_source_bundle_t *bundle, const char *image_directory,
    const char *const *app_paths, size_t app_count,
    const st_source_allocator_t *allocator)
{
    static const st_source_limits_t defaults = {
        ST_SOURCE_DEFAULT_MAX_MANIFEST_BYTES,
        ST_SOURCE_DEFAULT_MAX_FILE_BYTES,
        ST_SOURCE_DEFAULT_MAX_TOTAL_BYTES,
        ST_SOURCE_DEFAULT_MAX_FILES
    };
    return st_source_bundle_load_with_limits(bundle, image_directory,
        app_paths, app_count, allocator, &defaults);
}

st_source_load_status_t st_source_bundle_load_manifests_with_limits(
    st_source_bundle_t *bundle,
    const char *image_directory,
    const char *application_directory,
    const st_source_allocator_t *allocator,
    const st_source_limits_t *limits)
{
    st_source_load_status_t status;
    struct stat metadata;
    unsigned char *manifest = NULL;
    size_t manifest_length = 0u;
    int application_fd = -1;
    int manifest_fd = -1;
    int saved_errno;

    if (application_directory == NULL || application_directory[0] == '\0') {
        if (bundle == NULL) {
            return ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT;
        }
        memset(bundle, 0, sizeof(*bundle));
        bundle->diagnostic.status = ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT;
        return bundle->diagnostic.status;
    }
    status = st_source_bundle_load_with_limits(
        bundle, image_directory, NULL, 0u, allocator, limits);
    if (status != ST_SOURCE_LOAD_OK) {
        return status;
    }

    application_fd = open_protected_directory(application_directory);
    if (application_fd < 0) {
        status = classify_open_error(
            bundle, errno, true, ST_SOURCE_PHASE_APPLICATION_MANIFEST,
            bundle->count, 0u, application_directory);
        goto failed;
    }
    manifest_fd = openat(
        application_fd, ST_APPLICATION_MANIFEST_FILENAME,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (manifest_fd < 0) {
        status = classify_open_error(
            bundle, errno, true, ST_SOURCE_PHASE_APPLICATION_MANIFEST,
            bundle->count, 0u, ST_APPLICATION_MANIFEST_FILENAME);
        goto failed;
    }
    if (fstat(manifest_fd, &metadata) != 0) {
        status = set_error(
            bundle, ST_SOURCE_LOAD_ERR_IO,
            ST_SOURCE_PHASE_APPLICATION_MANIFEST, bundle->count, 0u,
            ST_APPLICATION_MANIFEST_FILENAME,
            strlen(ST_APPLICATION_MANIFEST_FILENAME), errno);
        goto failed;
    }
    if (!S_ISREG(metadata.st_mode)) {
        status = set_error(
            bundle, ST_SOURCE_LOAD_ERR_NOT_REGULAR_FILE,
            ST_SOURCE_PHASE_APPLICATION_MANIFEST, bundle->count, 0u,
            ST_APPLICATION_MANIFEST_FILENAME,
            strlen(ST_APPLICATION_MANIFEST_FILENAME), 0);
        goto failed;
    }
    status = read_entire_file(
        bundle, manifest_fd, &metadata, &manifest, &manifest_length,
        ST_SOURCE_PHASE_APPLICATION_MANIFEST, bundle->count, 0u,
        ST_APPLICATION_MANIFEST_FILENAME,
        bundle->limits.max_manifest_bytes);
    saved_errno = errno;
    if (close(manifest_fd) != 0 && status == ST_SOURCE_LOAD_OK) {
        status = set_error(
            bundle, ST_SOURCE_LOAD_ERR_IO,
            ST_SOURCE_PHASE_APPLICATION_MANIFEST, bundle->count, 0u,
            ST_APPLICATION_MANIFEST_FILENAME,
            strlen(ST_APPLICATION_MANIFEST_FILENAME), errno);
    }
    manifest_fd = -1;
    errno = saved_errno;
    if (status != ST_SOURCE_LOAD_OK) {
        goto failed;
    }

    status = load_manifest_sources(
        bundle, application_fd, application_directory,
        ST_SOURCE_ORIGIN_APPLICATION,
        ST_SOURCE_PHASE_APPLICATION_MANIFEST,
        ST_APPLICATION_MANIFEST_FILENAME, manifest, manifest_length);
    bundle_deallocate(bundle, manifest);
    manifest = NULL;
    if (status != ST_SOURCE_LOAD_OK) {
        goto failed;
    }
    if (close(application_fd) != 0) {
        status = set_error(
            bundle, ST_SOURCE_LOAD_ERR_IO,
            ST_SOURCE_PHASE_APPLICATION_MANIFEST, bundle->count, 0u,
            application_directory, strlen(application_directory), errno);
        application_fd = -1;
        goto failed;
    }
    return ST_SOURCE_LOAD_OK;

failed:
    saved_errno = errno;
    if (manifest_fd >= 0) {
        (void)close(manifest_fd);
    }
    if (application_fd >= 0) {
        (void)close(application_fd);
    }
    bundle_deallocate(bundle, manifest);
    release_files(bundle);
    errno = saved_errno;
    return status;
}

st_source_load_status_t st_source_bundle_load_manifests(
    st_source_bundle_t *bundle,
    const char *image_directory,
    const char *application_directory,
    const st_source_allocator_t *allocator)
{
    static const st_source_limits_t defaults = {
        ST_SOURCE_DEFAULT_MAX_MANIFEST_BYTES,
        ST_SOURCE_DEFAULT_MAX_FILE_BYTES,
        ST_SOURCE_DEFAULT_MAX_TOTAL_BYTES,
        ST_SOURCE_DEFAULT_MAX_FILES
    };
    return st_source_bundle_load_manifests_with_limits(
        bundle, image_directory, application_directory, allocator, &defaults);
}

const char *st_source_load_status_string(st_source_load_status_t status)
{
    switch (status) {
    case ST_SOURCE_LOAD_OK: return "ok";
    case ST_SOURCE_LOAD_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_SOURCE_LOAD_ERR_OVERFLOW: return "size overflow";
    case ST_SOURCE_LOAD_ERR_LIMIT_EXCEEDED: return "resource limit exceeded";
    case ST_SOURCE_LOAD_ERR_IO: return "I/O error";
    case ST_SOURCE_LOAD_ERR_MISSING_FILE: return "missing file";
    case ST_SOURCE_LOAD_ERR_NOT_REGULAR_FILE: return "not a regular file";
    case ST_SOURCE_LOAD_ERR_INVALID_MANIFEST: return "invalid source manifest";
    case ST_SOURCE_LOAD_ERR_PATH_TRAVERSAL: return "unsafe source path";
    case ST_SOURCE_LOAD_ERR_DUPLICATE_SOURCE: return "duplicate source";
    case ST_SOURCE_LOAD_ERR_PARSE: return "Smalltalk parse error";
    default: return "invalid source-load status";
    }
}

const char *st_source_load_phase_string(st_source_load_phase_t phase)
{
    switch (phase) {
    case ST_SOURCE_PHASE_NONE: return "none";
    case ST_SOURCE_PHASE_IMAGE_MANIFEST: return "image manifest";
    case ST_SOURCE_PHASE_IMAGE_SOURCE: return "image source";
    case ST_SOURCE_PHASE_APPLICATION_MANIFEST: return "application manifest";
    case ST_SOURCE_PHASE_APPLICATION_SOURCE: return "application source";
    default: return "invalid source-load phase";
    }
}
