#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "st_artifact_materialize.h"

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
    size_t maximum;
    size_t calls;
    size_t total;
    size_t fail_at;
} write_probe_t;

typedef struct {
    size_t allocations;
    size_t deallocations;
} allocation_probe_t;

static void *always_fail_allocate(void *user, size_t size)
{
    allocation_probe_t *probe = user;
    (void)size;
    probe->allocations++;
    return NULL;
}

static void count_deallocate(void *user, void *pointer)
{
    allocation_probe_t *probe = user;
    CHECK(pointer == NULL);
    probe->deallocations++;
}

static ssize_t probed_write(void *user, int file_descriptor,
                            const void *bytes, size_t length)
{
    write_probe_t *probe = user;
    size_t amount = length;
    probe->calls++;
    if (probe->fail_at != SIZE_MAX && probe->total >= probe->fail_at) {
        errno = ENOSPC;
        return -1;
    }
    if (amount > probe->maximum) amount = probe->maximum;
    if (probe->fail_at != SIZE_MAX && amount > probe->fail_at - probe->total)
        amount = probe->fail_at - probe->total;
    if (amount == 0u) {
        errno = ENOSPC;
        return -1;
    }
    {
        ssize_t written = write(file_descriptor, bytes, amount);
        if (written > 0) probe->total += (size_t)written;
        return written;
    }
}

static bool make_directory(char path[128], const char *stem)
{
    int length = snprintf(path, 128u, "/tmp/%s-XXXXXX", stem);
    return length > 0 && length < 128 && mkdtemp(path) != NULL;
}

static bool join_path(char output[320], const char *left, const char *right)
{
    int length = snprintf(output, 320u, "%s/%s", left, right);
    return length > 0 && length < 320;
}

static bool read_exact(const char *path, const void *expected, size_t length)
{
    uint8_t buffer[512];
    int file;
    ssize_t amount;
    struct stat info;
    if (length > sizeof(buffer)) return false;
    file = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) return false;
    amount = read(file, buffer, sizeof(buffer));
    if (fstat(file, &info) != 0 || close(file) != 0) return false;
    return amount == (ssize_t)length && info.st_size == (off_t)length
        && memcmp(buffer, expected, length) == 0
        && (info.st_mode & 0777) == 0644;
}

static size_t staging_count(const char *directory)
{
    DIR *stream = opendir(directory);
    struct dirent *entry;
    size_t count = 0u;
    if (stream == NULL) return SIZE_MAX;
    while ((entry = readdir(stream)) != NULL)
        if (strncmp(entry->d_name, ".anvil-staging-", 15u) == 0)
            count++;
    (void)closedir(stream);
    return count;
}

static void remove_profile(const char *root, const char *profile,
                           const st_artifact_bundle_t *bundle)
{
    char path[320];
    size_t index;
    for (index = 0u; index < bundle->artifact_count; index++) {
        CHECK(join_path(path, root, profile));
        {
            size_t used = strlen(path);
            CHECK(used + 1u + bundle->artifacts[index].name_length
                  < sizeof(path));
            path[used] = '/';
            memcpy(path + used + 1u, bundle->artifacts[index].name,
                   bundle->artifacts[index].name_length + 1u);
        }
        CHECK(unlink(path) == 0);
    }
    CHECK(join_path(path, root, profile));
    CHECK(strlen(path) + sizeof("/bundle.manifest") < sizeof(path));
    strcat(path, "/bundle.manifest");
    CHECK(unlink(path) == 0);
    CHECK(join_path(path, root, profile));
    CHECK(rmdir(path) == 0);
}

static void bundle_init(st_artifact_bundle_t *bundle,
                        st_artifact_blob_t artifacts[2])
{
    static unsigned char method[] = "\t.text\nmethod:\n\tret\n";
    static unsigned char metadata[] = "\t.data\nmetadata:\n\t.quad method\n";
    static char manifest[256];
    static const uint8_t bundle_hash[ST_ARTIFACT_SHA256_SIZE] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f
    };
    memset(bundle, 0, sizeof(*bundle));
    memset(artifacts, 0, 2u * sizeof(*artifacts));
    artifacts[0].kind = ST_ARTIFACT_METHOD_ASSEMBLY;
    artifacts[0].method_id = 1u;
    artifacts[0].name = "00000001-demo_m1.s";
    artifacts[0].name_length = strlen(artifacts[0].name);
    artifacts[0].symbol = "demo_m1";
    artifacts[0].symbol_length = strlen(artifacts[0].symbol);
    artifacts[0].bytes = method;
    artifacts[0].size = sizeof(method) - 1u;
    CHECK(st_artifact_sha256(method, sizeof(method) - 1u,
                             artifacts[0].sha256));
    artifacts[1].kind = ST_ARTIFACT_METADATA_ASSEMBLY;
    artifacts[1].name = "metadata.s";
    artifacts[1].name_length = strlen(artifacts[1].name);
    artifacts[1].symbol = "demo_descriptor";
    artifacts[1].symbol_length = strlen(artifacts[1].symbol);
    artifacts[1].bytes = metadata;
    artifacts[1].size = sizeof(metadata) - 1u;
    CHECK(st_artifact_sha256(metadata, sizeof(metadata) - 1u,
                             artifacts[1].sha256));
    snprintf(manifest, sizeof(manifest),
             "anvil-smalltalk-artifact-bundle-v2\n"
             "target=x86_64\nabi=sysv\nsyntax=gas\noptimization=O2\n"
             "launch=absent\n"
             "bundle-sha256="
             "000102030405060708090a0b0c0d0e0f"
             "101112131415161718191a1b1c1d1e1f\n");
    bundle->status = ST_ARTIFACT_BUNDLE_OK;
    bundle->target = ANVIL_ARCH_X86_64;
    bundle->abi = ANVIL_ABI_SYSV;
    bundle->syntax = ANVIL_SYNTAX_GAS;
    bundle->optimization = ANVIL_OPT_STANDARD;
    bundle->artifacts = artifacts;
    bundle->artifact_count = 2u;
    bundle->manifest = manifest;
    bundle->manifest_length = strlen(manifest);
    memcpy(bundle->bundle_sha256, bundle_hash, sizeof(bundle_hash));
    bundle->implementation = bundle;
}

static void test_success_partial_writes_and_collision(
    st_artifact_bundle_t *bundle)
{
    char root[128], path[320];
    st_artifact_materialize_result_t result;
    write_probe_t probe = {3u, 0u, 0u, SIZE_MAX};
    st_artifact_materialize_options_t options = {
        {0}, probed_write, &probe
    };
    CHECK(make_directory(root, "anvil-st-materialize"));
    st_artifact_materialize_result_init(&result);
    CHECK(st_artifact_bundle_materialize(&result, bundle, root, &options)
          == ST_ARTIFACT_MATERIALIZE_OK);
    CHECK(result.committed == 1);
    CHECK(strcmp(result.profile, "x86_64-sysv-gas-O2") == 0);
    CHECK(probe.calls > bundle->artifact_count + 1u);
    CHECK(staging_count(root) == 0u);
    CHECK(join_path(path, root, result.profile));
    strcat(path, "/00000001-demo_m1.s");
    CHECK(read_exact(path, bundle->artifacts[0].bytes,
                     bundle->artifacts[0].size));
    CHECK(join_path(path, root, result.profile));
    strcat(path, "/metadata.s");
    CHECK(read_exact(path, bundle->artifacts[1].bytes,
                     bundle->artifacts[1].size));
    CHECK(join_path(path, root, result.profile));
    strcat(path, "/bundle.manifest");
    CHECK(read_exact(path, bundle->manifest, bundle->manifest_length));
    st_artifact_materialize_result_init(&result);
    CHECK(st_artifact_bundle_materialize(&result, bundle, root, &options)
          == ST_ARTIFACT_MATERIALIZE_ERR_COLLISION);
    CHECK(result.committed == 0 && staging_count(root) == 0u);
    remove_profile(root, "x86_64-sysv-gas-O2", bundle);
    CHECK(rmdir(root) == 0);
}

static void test_mid_write_rollback(st_artifact_bundle_t *bundle)
{
    char root[128], profile[320];
    st_artifact_materialize_result_t result;
    write_probe_t probe = {4u, 0u, 0u, bundle->artifacts[0].size + 2u};
    st_artifact_materialize_options_t options = {
        {0}, probed_write, &probe
    };
    CHECK(make_directory(root, "anvil-st-materialize-fail"));
    st_artifact_materialize_result_init(&result);
    CHECK(st_artifact_bundle_materialize(&result, bundle, root, &options)
          == ST_ARTIFACT_MATERIALIZE_ERR_IO);
    CHECK(result.system_error == ENOSPC && result.committed == 0);
    CHECK(staging_count(root) == 0u);
    CHECK(join_path(profile, root, "x86_64-sysv-gas-O2"));
    CHECK(access(profile, F_OK) != 0 && errno == ENOENT);
    CHECK(rmdir(root) == 0);
}

static void test_invalid_names_symlinks_and_oom(st_artifact_bundle_t *bundle)
{
    char root[128], outside[128], link_path[320], profile[320], nested[320];
    st_artifact_materialize_result_t result;
    st_artifact_materialize_options_t options = {{0}, NULL, NULL};
    allocation_probe_t allocation = {0u, 0u};
    char *saved_name = bundle->artifacts[0].name;
    size_t saved_length = bundle->artifacts[0].name_length;
    CHECK(make_directory(root, "anvil-st-materialize-secure"));
    CHECK(make_directory(outside, "anvil-st-materialize-outside"));

    bundle->artifacts[0].name = "../escaped.s";
    bundle->artifacts[0].name_length = strlen(bundle->artifacts[0].name);
    st_artifact_materialize_result_init(&result);
    CHECK(st_artifact_bundle_materialize(&result, bundle, root, &options)
          == ST_ARTIFACT_MATERIALIZE_ERR_INVALID_BUNDLE);
    CHECK(staging_count(root) == 0u);
    bundle->artifacts[0].name = saved_name;
    bundle->artifacts[0].name_length = saved_length;

    CHECK(join_path(link_path, root, "output-link"));
    CHECK(symlink(outside, link_path) == 0);
    st_artifact_materialize_result_init(&result);
    CHECK(st_artifact_bundle_materialize(&result, bundle, link_path, &options)
          == ST_ARTIFACT_MATERIALIZE_ERR_IO);
    CHECK(result.system_error == ELOOP || result.system_error == ENOTDIR);
    CHECK(unlink(link_path) == 0);

    CHECK(join_path(nested, outside, "real"));
    CHECK(mkdir(nested, 0700) == 0);
    CHECK(join_path(link_path, root, "intermediate"));
    CHECK(symlink(outside, link_path) == 0);
    CHECK(strlen(link_path) + sizeof("/real") < sizeof(link_path));
    strcat(link_path, "/real");
    st_artifact_materialize_result_init(&result);
    CHECK(st_artifact_bundle_materialize(&result, bundle, link_path, &options)
          == ST_ARTIFACT_MATERIALIZE_ERR_IO);
    CHECK(result.system_error == ELOOP || result.system_error == ENOTDIR);
    link_path[strlen(link_path) - sizeof("/real") + 1u] = '\0';
    CHECK(unlink(link_path) == 0);

    snprintf(link_path, sizeof(link_path), "%s/../%s", root,
             strrchr(outside, '/') + 1);
    st_artifact_materialize_result_init(&result);
    CHECK(st_artifact_bundle_materialize(&result, bundle, link_path, &options)
          == ST_ARTIFACT_MATERIALIZE_ERR_IO);
    CHECK(result.system_error == EINVAL);

    CHECK(join_path(profile, root, "x86_64-sysv-gas-O2"));
    CHECK(symlink(outside, profile) == 0);
    st_artifact_materialize_result_init(&result);
    CHECK(st_artifact_bundle_materialize(&result, bundle, root, &options)
          == ST_ARTIFACT_MATERIALIZE_ERR_COLLISION);
    CHECK(staging_count(root) == 0u);
    CHECK(unlink(profile) == 0);

    options.allocator = (st_artifact_allocator_t){
        always_fail_allocate, count_deallocate, &allocation
    };
    st_artifact_materialize_result_init(&result);
    CHECK(st_artifact_bundle_materialize(&result, bundle, root, &options)
          == ST_ARTIFACT_MATERIALIZE_ERR_OUT_OF_MEMORY);
    CHECK(allocation.allocations == 1u && allocation.deallocations == 0u);
    CHECK(staging_count(root) == 0u);

    CHECK(rmdir(nested) == 0);
    CHECK(rmdir(outside) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_deterministic_trees(st_artifact_bundle_t *bundle)
{
    char first[128], second[128], left[320], right[320];
    st_artifact_materialize_result_t a, b;
    st_artifact_materialize_options_t options = {{0}, NULL, NULL};
    size_t index;
    CHECK(make_directory(first, "anvil-st-materialize-a"));
    CHECK(make_directory(second, "anvil-st-materialize-b"));
    st_artifact_materialize_result_init(&a);
    st_artifact_materialize_result_init(&b);
    CHECK(st_artifact_bundle_materialize(&a, bundle, first, &options)
          == ST_ARTIFACT_MATERIALIZE_OK);
    CHECK(st_artifact_bundle_materialize(&b, bundle, second, &options)
          == ST_ARTIFACT_MATERIALIZE_OK);
    CHECK(strcmp(a.profile, b.profile) == 0);
    for (index = 0u; index < bundle->artifact_count; index++) {
        snprintf(left, sizeof(left), "%s/%s/%s", first, a.profile,
                 bundle->artifacts[index].name);
        snprintf(right, sizeof(right), "%s/%s/%s", second, b.profile,
                 bundle->artifacts[index].name);
        CHECK(read_exact(left, bundle->artifacts[index].bytes,
                         bundle->artifacts[index].size));
        CHECK(read_exact(right, bundle->artifacts[index].bytes,
                         bundle->artifacts[index].size));
    }
    remove_profile(first, a.profile, bundle);
    remove_profile(second, b.profile, bundle);
    CHECK(rmdir(first) == 0);
    CHECK(rmdir(second) == 0);
}

int main(void)
{
    st_artifact_bundle_t bundle;
    st_artifact_blob_t artifacts[2];
    bundle_init(&bundle, artifacts);
    test_success_partial_writes_and_collision(&bundle);
    test_mid_write_rollback(&bundle);
    test_invalid_names_symlinks_and_oom(&bundle);
    test_deterministic_trees(&bundle);
    if (failures != 0u) {
        fprintf(stderr, "smalltalk artifact materializer: %u failure(s)\n",
                failures);
        return 1;
    }
    puts("smalltalk artifact materializer: PASS (atomic no-replace profiles, exact partial writes, secure rollback)");
    return 0;
}
