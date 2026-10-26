#include "st_aot_toolchain.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#ifndef ST_AOTC_PATH
#error "ST_AOTC_PATH must name the application compiler"
#endif

#ifndef ST_AOT_LINK_PATH
#error "ST_AOT_LINK_PATH must name the native AOT linker"
#endif

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                 \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static bool format_path(
    char *output, size_t output_capacity,
    const char *directory, const char *name)
{
    int length = snprintf(
        output, output_capacity, "%s/%s", directory, name);

    return length > 0 && (size_t)length < output_capacity;
}

static bool run_process(char *const arguments[], int *exit_code_out)
{
    pid_t child = -1;
    int status = 0;
    int error;

    if (arguments == NULL || arguments[0] == NULL
            || exit_code_out == NULL) {
        return false;
    }
    *exit_code_out = -1;
    error = posix_spawn(
        &child, arguments[0], NULL, NULL, arguments, environ);
    if (error != 0) {
        fprintf(stderr, "posix_spawn(%s): %s\n",
                arguments[0], strerror(error));
        return false;
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return false;
        }
    }
    if (!WIFEXITED(status)) {
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "%s terminated by signal %d\n",
                    arguments[0], WTERMSIG(status));
        }
        return false;
    }
    *exit_code_out = WEXITSTATUS(status);
    return true;
}

static bool has_suffix(const char *text, const char *suffix)
{
    size_t text_length;
    size_t suffix_length;

    if (text == NULL || suffix == NULL) {
        return false;
    }
    text_length = strlen(text);
    suffix_length = strlen(suffix);
    return suffix_length <= text_length
        && memcmp(
            text + text_length - suffix_length,
            suffix, suffix_length) == 0;
}

static bool cross_assemble_arm64_profile(
    const char *profile_directory, const char *object_directory)
{
    DIR *directory;
    struct dirent *entry;
    size_t assembled = 0u;
    bool succeeded = true;

    if (profile_directory == NULL || object_directory == NULL
            || mkdir(object_directory, 0700) != 0) {
        return false;
    }
    directory = opendir(profile_directory);
    if (directory == NULL) {
        return false;
    }
    while (succeeded && (entry = readdir(directory)) != NULL) {
        char source_path[1536];
        char object_path[1536];
        char *arguments[7];
        int exit_code = -1;
        int source_length;
        int object_length;

        if (!has_suffix(entry->d_name, ".s")) {
            continue;
        }
        source_length = snprintf(
            source_path, sizeof(source_path), "%s/%s",
            profile_directory, entry->d_name);
        object_length = snprintf(
            object_path, sizeof(object_path), "%s/%08zu.o",
            object_directory, assembled);
        if (source_length <= 0
                || (size_t)source_length >= sizeof(source_path)
                || object_length <= 0
                || (size_t)object_length >= sizeof(object_path)) {
            succeeded = false;
            break;
        }
        arguments[0] = "/usr/bin/clang";
        arguments[1] = "--target=aarch64-linux-gnu";
        arguments[2] = "-c";
        arguments[3] = source_path;
        arguments[4] = "-o";
        arguments[5] = object_path;
        arguments[6] = NULL;
        succeeded = run_process(arguments, &exit_code) && exit_code == 0;
        assembled++;
    }
    if (closedir(directory) != 0) {
        succeeded = false;
    }
    return succeeded && assembled != 0u;
}

static bool run_process_captured(
    const char *executable,
    const char *stdout_path,
    const char *stderr_path,
    int *exit_code_out)
{
    posix_spawn_file_actions_t actions;
    char *arguments[2];
    pid_t child = -1;
    int status = 0;
    int error;

    if (executable == NULL || stdout_path == NULL || stderr_path == NULL
            || exit_code_out == NULL) {
        return false;
    }
    *exit_code_out = -1;
    error = posix_spawn_file_actions_init(&actions);
    if (error != 0) {
        return false;
    }
    error = posix_spawn_file_actions_addopen(
        &actions, STDOUT_FILENO, stdout_path,
        O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (error == 0) {
        error = posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, stderr_path,
            O_WRONLY | O_CREAT | O_EXCL, 0600);
    }
    arguments[0] = (char *)executable;
    arguments[1] = NULL;
    if (error == 0) {
        error = posix_spawn(
            &child, executable, &actions, NULL, arguments, environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
        fprintf(stderr, "posix_spawn(%s): %s\n",
                executable, strerror(error));
        return false;
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return false;
        }
    }
    if (!WIFEXITED(status)) {
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "%s terminated by signal %d\n",
                    executable, WTERMSIG(status));
        }
        return false;
    }
    *exit_code_out = WEXITSTATUS(status);
    return true;
}

static bool read_file(
    const char *path, uint8_t **bytes_out, size_t *length_out)
{
    struct stat information;
    uint8_t *bytes = NULL;
    size_t offset = 0u;
    int descriptor;

    if (path == NULL || bytes_out == NULL || length_out == NULL) {
        return false;
    }
    *bytes_out = NULL;
    *length_out = 0u;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &information) != 0
            || information.st_size < 0
            || (uintmax_t)information.st_size > SIZE_MAX) {
        if (descriptor >= 0) {
            close(descriptor);
        }
        return false;
    }
    if (information.st_size != 0) {
        bytes = malloc((size_t)information.st_size);
        if (bytes == NULL) {
            close(descriptor);
            return false;
        }
    }
    while (offset < (size_t)information.st_size) {
        ssize_t count = read(
            descriptor, bytes + offset,
            (size_t)information.st_size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            free(bytes);
            close(descriptor);
            return false;
        }
        offset += (size_t)count;
    }
    if (close(descriptor) != 0) {
        free(bytes);
        return false;
    }
    *bytes_out = bytes;
    *length_out = offset;
    return true;
}

static int remove_entry(
    const char *path, const struct stat *information,
    int type, struct FTW *walk)
{
    (void)information;
    (void)walk;

    return type == FTW_DP ? rmdir(path) : unlink(path);
}

static void remove_tree(const char *path)
{
    if (path != NULL) {
        (void)nftw(path, remove_entry, 32, FTW_DEPTH | FTW_PHYS);
    }
}

/* Mirrors SMALLTALK_PRODUCT_RUNTIME_SRCS in the root Makefile: the runtime
 * object is every source of the shipped runtime, discovered by pattern, so
 * new primitive families cannot silently go missing from this gate. */
static const char *const runtime_source_patterns[] = {
    "samples/smalltalk/src/platform/posix/*.c",
    "samples/smalltalk/src/compiler/primitive.c",
    "samples/smalltalk/src/runtime/*.c",
    "samples/smalltalk/src/runtime/control/*.c",
    "samples/smalltalk/src/runtime/primitives/*.c",
    "samples/smalltalk/src/product/application_startup.c",
    "samples/smalltalk/examples/support/native_main.c"
};

static bool build_runtime_object(
    const char *object_path, const char *launch_symbol)
{
    static const char *const prefix[] = {
        "/usr/bin/gcc", "-std=c11", "-D_GNU_SOURCE", "-O2", "-Iinclude",
        "-Isamples/smalltalk/include", NULL /* launch definition */, "-r"
    };
    enum { PREFIX_COUNT = sizeof(prefix) / sizeof(prefix[0]), SUFFIX_COUNT = 3 };
    char launch_definition[256];
    glob_t sources;
    char **arguments = NULL;
    size_t argument = 0u;
    int exit_code = -1;
    bool ok = false;

    if (object_path == NULL || launch_symbol == NULL
            || snprintf(
                launch_definition, sizeof(launch_definition),
                "-DST_APPLICATION_LAUNCH_SYMBOL=%s", launch_symbol) <= 0
            || strlen(launch_definition) >= sizeof(launch_definition) - 1u) {
        return false;
    }

    memset(&sources, 0, sizeof(sources));
    for (size_t index = 0u; index < sizeof(runtime_source_patterns)
             / sizeof(runtime_source_patterns[0]); index++) {
        /* GLOB_ERR is deliberately absent: a pattern with no match is a
         * missing source and fails the exact-count check below. */
        size_t before = sources.gl_pathc;
        if (glob(runtime_source_patterns[index],
                 index == 0u ? 0 : GLOB_APPEND, NULL, &sources) != 0
                || sources.gl_pathc == before) {
            fprintf(stderr, "runtime source pattern matched nothing: %s\n",
                    runtime_source_patterns[index]);
            goto done;
        }
    }

    arguments = calloc(PREFIX_COUNT + sources.gl_pathc + SUFFIX_COUNT,
                       sizeof(*arguments));
    if (arguments == NULL) goto done;
    for (size_t index = 0u; index < PREFIX_COUNT; index++) {
        arguments[argument++] = prefix[index] != NULL
            ? (char *)prefix[index] : launch_definition;
    }
    for (size_t index = 0u; index < sources.gl_pathc; index++) {
        arguments[argument++] = sources.gl_pathv[index];
    }
    arguments[argument++] = "-o";
    arguments[argument++] = (char *)object_path;
    arguments[argument] = NULL;

    ok = run_process(arguments, &exit_code) && exit_code == 0;
done:
    free(arguments);
    globfree(&sources);
    return ok;
}

int main(void)
{
    static const uint8_t expected_stdout[] =
        "Hello from Anvil Smalltalk\n";
    static const char *const allowed_compilers[] = {
        "/usr/bin/gcc"
    };
    static const char *const link_arguments[] = {
        "-pthread", "-lm"
    };
    char temporary_root[] = "/tmp/anvil-st-hello-e2e-XXXXXX";
    char profile_directory[1024];
    char arm64_profile_directory[1024];
    char arm64_object_directory[1024];
    char link_output_directory[1024];
    char runtime_object[1024];
    char stdout_path[1024];
    char stderr_path[1024];
    char closure_profile_directory[1024];
    char closure_link_output_directory[1024];
    char closure_runtime_object[1024];
    char closure_stdout_path[1024];
    char closure_stderr_path[1024];
    char closure_executable[1024];
    char *compiler_arguments[8];
    const char *runtime_inputs[1];
    st_aot_toolchain_options_t toolchain_options;
    st_aot_toolchain_result_t toolchain_result;
    uint8_t *stdout_bytes = NULL;
    uint8_t *stderr_bytes = NULL;
    size_t stdout_length = 0u;
    size_t stderr_length = 0u;
    int exit_code = -1;
    bool paths_ready;

    CHECK(mkdtemp(temporary_root) != NULL);
    paths_ready = format_path(
        profile_directory, sizeof(profile_directory), temporary_root,
        "hello/x86_64-sysv-gas-O2");
    paths_ready = paths_ready && format_path(
        arm64_profile_directory, sizeof(arm64_profile_directory),
        temporary_root, "hello/arm64-sysv-gas-O2");
    paths_ready = paths_ready && format_path(
        arm64_object_directory, sizeof(arm64_object_directory),
        temporary_root, "hello-arm64-objects");
    paths_ready = paths_ready && format_path(
        link_output_directory, sizeof(link_output_directory),
        temporary_root, "linked");
    paths_ready = paths_ready && format_path(
        runtime_object, sizeof(runtime_object), temporary_root,
        "smalltalk-runtime.o");
    paths_ready = paths_ready && format_path(
        stdout_path, sizeof(stdout_path), temporary_root, "stdout.bin");
    paths_ready = paths_ready && format_path(
        stderr_path, sizeof(stderr_path), temporary_root, "stderr.bin");
    paths_ready = paths_ready && format_path(
        closure_profile_directory, sizeof(closure_profile_directory),
        temporary_root, "closures/x86_64-sysv-gas-O2");
    paths_ready = paths_ready && format_path(
        closure_link_output_directory,
        sizeof(closure_link_output_directory), temporary_root,
        "linked-closures");
    paths_ready = paths_ready && format_path(
        closure_runtime_object, sizeof(closure_runtime_object),
        temporary_root, "smalltalk-closures-runtime.o");
    paths_ready = paths_ready && format_path(
        closure_stdout_path, sizeof(closure_stdout_path),
        temporary_root, "closures-stdout.bin");
    paths_ready = paths_ready && format_path(
        closure_stderr_path, sizeof(closure_stderr_path),
        temporary_root, "closures-stderr.bin");
    paths_ready = paths_ready && format_path(
        closure_executable, sizeof(closure_executable),
        closure_link_output_directory, "closures-native/closures");
    CHECK(paths_ready);

    compiler_arguments[0] = ST_AOTC_PATH;
    compiler_arguments[1] = "samples/smalltalk/st-image";
    compiler_arguments[2] = "samples/smalltalk/examples/hello";
    compiler_arguments[3] = "hello";
    compiler_arguments[4] = "HelloApplication";
    compiler_arguments[5] = "run";
    compiler_arguments[6] = temporary_root;
    compiler_arguments[7] = NULL;
    CHECK(run_process(compiler_arguments, &exit_code));
    CHECK(exit_code == 0);
    CHECK(cross_assemble_arm64_profile(
        arm64_profile_directory, arm64_object_directory));
    CHECK(build_runtime_object(
        runtime_object, "st_app_hello_launch_descriptor"));
    CHECK(mkdir(link_output_directory, 0700) == 0);

    memset(&toolchain_options, 0, sizeof(toolchain_options));
    runtime_inputs[0] = runtime_object;
    toolchain_options.profile_directory = profile_directory;
    toolchain_options.output_directory = link_output_directory;
    toolchain_options.publication_name = "hello-native";
    toolchain_options.executable_name = "hello";
    toolchain_options.compiler_driver = "/usr/bin/gcc";
    toolchain_options.allowed_compiler_drivers = allowed_compilers;
    toolchain_options.allowed_compiler_driver_count = 1u;
    toolchain_options.runtime_link_inputs = runtime_inputs;
    toolchain_options.runtime_link_input_count = 1u;
    toolchain_options.link_arguments = link_arguments;
    toolchain_options.link_argument_count = 2u;
    st_aot_toolchain_result_init(&toolchain_result);
    CHECK(st_aot_toolchain_link(&toolchain_result, &toolchain_options)
          == ST_AOT_TOOLCHAIN_OK);
    CHECK(toolchain_result.committed);
    CHECK(toolchain_result.published_executable != NULL);

    if (toolchain_result.published_executable != NULL) {
        CHECK(run_process_captured(
            toolchain_result.published_executable,
            stdout_path, stderr_path, &exit_code));
        CHECK(exit_code == 0);
        CHECK(read_file(stdout_path, &stdout_bytes, &stdout_length));
        CHECK(read_file(stderr_path, &stderr_bytes, &stderr_length));
        CHECK(stdout_length == sizeof(expected_stdout) - 1u);
        CHECK(stdout_length != sizeof(expected_stdout) - 1u
              || memcmp(stdout_bytes, expected_stdout, stdout_length) == 0);
        CHECK(stderr_length == 0u);
    }

    free(stderr_bytes);
    free(stdout_bytes);
    stderr_bytes = NULL;
    stdout_bytes = NULL;
    stderr_length = 0u;
    stdout_length = 0u;
    st_aot_toolchain_result_destroy(&toolchain_result);

    compiler_arguments[2] = "samples/smalltalk/examples/closures";
    compiler_arguments[3] = "closures";
    compiler_arguments[4] = "ClosuresApplication";
    CHECK(run_process(compiler_arguments, &exit_code));
    CHECK(exit_code == 0);
    CHECK(build_runtime_object(
        closure_runtime_object, "st_app_closures_launch_descriptor"));
    CHECK(mkdir(closure_link_output_directory, 0700) == 0);

    {
        char *linker_arguments[] = {
            ST_AOT_LINK_PATH,
            closure_profile_directory,
            closure_link_output_directory,
            "closures-native",
            "closures",
            closure_runtime_object,
            NULL
        };

        CHECK(run_process(linker_arguments, &exit_code));
        CHECK(exit_code == 0);
    }
    if (exit_code == 0) {
        CHECK(run_process_captured(
            closure_executable,
            closure_stdout_path, closure_stderr_path, &exit_code));
        CHECK(exit_code == 42);
        CHECK(read_file(
            closure_stdout_path, &stdout_bytes, &stdout_length));
        CHECK(read_file(
            closure_stderr_path, &stderr_bytes, &stderr_length));
        CHECK(stdout_length == 0u);
        CHECK(stderr_length == 0u);
    }

    free(stderr_bytes);
    free(stdout_bytes);
    remove_tree(temporary_root);
    if (failures == 0u) {
        puts("smalltalk applications: PASS "
             "(Hello exact stdout; closures return 42)");
    }
    return failures == 0u ? 0 : 1;
}
