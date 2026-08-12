#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "st_aot_toolchain.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        fprintf(stderr, "%s:%d: check failed: %s\n",                        \
                __FILE__, __LINE__, #condition);                               \
        failures++;                                                            \
    }                                                                          \
} while (0)

typedef struct {
    const char *kind;
    uint32_t method_id;
    const char *name;
    const char *symbol;
    const char *bytes;
} fixture_artifact_t;

static bool write_all(int file, const void *bytes, size_t length)
{
    const unsigned char *cursor = bytes;
    while (length != 0u) {
        ssize_t amount = write(file, cursor, length);
        if (amount < 0 && errno == EINTR) continue;
        if (amount <= 0) return false;
        cursor += (size_t)amount;
        length -= (size_t)amount;
    }
    return true;
}

static bool write_file(const char *directory, const char *name,
                       const void *bytes, size_t length)
{
    char path[512];
    int file;
    int count = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (count < 0 || (size_t)count >= sizeof(path)) return false;
    file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (file < 0) return false;
    if (!write_all(file, bytes, length) || close(file) != 0) return false;
    return true;
}

static void hash_hex(const uint8_t hash[ST_ARTIFACT_SHA256_SIZE],
                     char output[ST_ARTIFACT_SHA256_SIZE * 2u + 1u])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0u; index < ST_ARTIFACT_SHA256_SIZE; index++) {
        output[index * 2u] = digits[hash[index] >> 4u];
        output[index * 2u + 1u] = digits[hash[index] & UINT8_C(0x0f)];
    }
    output[ST_ARTIFACT_SHA256_SIZE * 2u] = '\0';
}

static bool build_profile(const char *directory, const char *method_bytes)
{
    const char metadata[] =
        ".data\n.globl st_test_descriptor\n"
        "st_test_descriptor:\n.quad 0\n";
    const char launch[] =
        ".data\n.globl st_test_launch_descriptor\n"
        "st_test_launch_descriptor:\n.quad st_test_descriptor\n";
    fixture_artifact_t artifacts[] = {
        {"method", 1u, "00000001-st_test_main.s", "st_test_main",
         method_bytes},
        {"metadata", 0u, "metadata.s", "st_test_descriptor", metadata},
        {"launch", 0u, "launch.s", "st_test_launch_descriptor", launch}
    };
    char manifest[4096];
    size_t used = 0u, index;
    int count = snprintf(manifest, sizeof(manifest),
        "anvil-smalltalk-artifact-bundle-v2\n"
        "target=x86_64\nabi=sysv\nsyntax=gas\noptimization=O2\n"
        "metadata-abi=5\nlaunch=present\nartifact-count=3\n"
        "block-count=0\n"
        "bundle-sha256="
        "00000000000000000000000000000000"
        "00000000000000000000000000000000\n");
    if (count < 0 || (size_t)count >= sizeof(manifest)) return false;
    used = (size_t)count;
    for (index = 0u; index < 3u; index++) {
        uint8_t hash[ST_ARTIFACT_SHA256_SIZE];
        char hexadecimal[ST_ARTIFACT_SHA256_SIZE * 2u + 1u];
        size_t length = strlen(artifacts[index].bytes);
        if (!st_artifact_sha256(artifacts[index].bytes, length, hash)
                || !write_file(directory, artifacts[index].name,
                               artifacts[index].bytes, length))
            return false;
        hash_hex(hash, hexadecimal);
        count = snprintf(manifest + used, sizeof(manifest) - used,
            "artifact=%s|%08u|%s|%s|%zu|%s\n",
            artifacts[index].kind, artifacts[index].method_id,
            artifacts[index].name, artifacts[index].symbol,
            length, hexadecimal);
        if (count < 0 || (size_t)count >= sizeof(manifest) - used)
            return false;
        used += (size_t)count;
    }
    return write_file(directory, "bundle.manifest", manifest, used);
}

static bool run_executable(const char *path)
{
    pid_t child = fork();
    int status;
    if (child < 0) return false;
    if (child == 0) {
        char *const argv[] = {(char *)path, NULL};
        execv(path, argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void remove_flat_directory(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;
    char child[512];
    if (directory == NULL) return;
    while ((entry = readdir(directory)) != NULL) {
        int count;
        if (strcmp(entry->d_name, ".") == 0
                || strcmp(entry->d_name, "..") == 0)
            continue;
        count = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (count > 0 && (size_t)count < sizeof(child)) CHECK(unlink(child) == 0);
    }
    CHECK(closedir(directory) == 0);
    CHECK(rmdir(path) == 0);
}

int main(void)
{
    static const char valid_method[] =
        ".text\n.globl main\n.type main,@function\n"
        "main:\n xorl %eax,%eax\n ret\n"
        ".section .note.GNU-stack,\"\",@progbits\n";
    static const char invalid_method[] = ".text\nthis is not assembly\n";
    char root[] = "/tmp/anvil-st-toolchain-XXXXXX";
    char profile[512], output[512], published[512];
    const char *allowed[] = {"/usr/bin/gcc"};
    st_aot_toolchain_options_t options;
    st_aot_toolchain_result_t result;
    int count;
    CHECK(mkdtemp(root) != NULL);
    count = snprintf(profile, sizeof(profile), "%s/profile", root);
    CHECK(count > 0 && (size_t)count < sizeof(profile));
    count = snprintf(output, sizeof(output), "%s/output", root);
    CHECK(count > 0 && (size_t)count < sizeof(output));
    CHECK(mkdir(profile, 0755) == 0);
    CHECK(mkdir(output, 0755) == 0);
    CHECK(build_profile(profile, valid_method));
    memset(&options, 0, sizeof(options));
    options.profile_directory = profile;
    options.output_directory = output;
    options.publication_name = "native";
    options.executable_name = "application";
    options.compiler_driver = "/usr/bin/gcc";
    options.allowed_compiler_drivers = allowed;
    options.allowed_compiler_driver_count = 1u;

    st_aot_toolchain_result_init(&result);
    CHECK(st_aot_toolchain_link(&result, &options) == ST_AOT_TOOLCHAIN_OK);
    CHECK(result.committed && result.failed_argv == NULL);
    CHECK(result.published_executable != NULL
          && run_executable(result.published_executable));
    st_aot_toolchain_result_destroy(&result);

    options.publication_name = "disallowed";
    options.compiler_driver = "/usr/bin/clang";
    st_aot_toolchain_result_init(&result);
    CHECK(st_aot_toolchain_link(&result, &options)
          == ST_AOT_TOOLCHAIN_ERR_TOOL_NOT_ALLOWED);
    CHECK(!result.committed);
    st_aot_toolchain_result_destroy(&result);

    options.publication_name = "spawn-failure";
    options.compiler_driver = "/does/not/exist/gcc";
    allowed[0] = options.compiler_driver;
    st_aot_toolchain_result_init(&result);
    {
        st_aot_toolchain_status_t status =
            st_aot_toolchain_link(&result, &options);
        /* Valgrind implements exec through a wrapper child and therefore
         * reports its exit instead of the host posix_spawn ENOENT. Both paths
         * must retain the exact failed argv and stage. */
        CHECK(status == ST_AOT_TOOLCHAIN_ERR_SPAWN
              || status == ST_AOT_TOOLCHAIN_ERR_TOOL_FAILED);
    }
    CHECK(result.failed_stage == ST_AOT_TOOLCHAIN_STAGE_ASSEMBLE
          && result.failed_argc == 7u
          && strcmp(result.failed_argv[0], options.compiler_driver) == 0);
    st_aot_toolchain_result_destroy(&result);

    CHECK(build_profile(profile, invalid_method));
    options.publication_name = "tool-failure";
    options.compiler_driver = "/usr/bin/gcc";
    allowed[0] = options.compiler_driver;
    st_aot_toolchain_result_init(&result);
    CHECK(st_aot_toolchain_link(&result, &options)
          == ST_AOT_TOOLCHAIN_ERR_TOOL_FAILED);
    CHECK(result.failed_stage == ST_AOT_TOOLCHAIN_STAGE_ASSEMBLE
          && result.tool_stderr != NULL
          && result.tool_stderr_length != 0u);
    st_aot_toolchain_result_destroy(&result);

    count = snprintf(published, sizeof(published), "%s/native", output);
    CHECK(count > 0 && (size_t)count < sizeof(published));
    remove_flat_directory(published);
    remove_flat_directory(profile);
    remove_flat_directory(output);
    CHECK(rmdir(root) == 0);
    if (failures != 0u) {
        fprintf(stderr, "aot toolchain: %u failure(s)\n", failures);
        return 1;
    }
    puts("aot toolchain: transactional assembly/link tests passed");
    return 0;
}
