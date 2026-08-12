#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "st_application_materialize.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(                                                         \
                stderr, "%s:%d: check failed: %s\n",                       \
                __FILE__, __LINE__, #condition);                             \
            failures++;                                                     \
        }                                                                    \
    } while (0)

typedef struct {
    st_artifact_blob_t artifacts[2];
    unsigned char metadata[48];
    unsigned char launch[48];
    char manifest[384];
} bundle_storage_t;

typedef struct {
    size_t bytes_before_failure;
    size_t written;
} write_failure_t;

static const anvil_arch_t supported_targets[] = {
    ANVIL_ARCH_X86_64,
    ANVIL_ARCH_ARM64,
    ANVIL_ARCH_PPC64,
    ANVIL_ARCH_PPC64LE,
    ANVIL_ARCH_ZARCH
};

static const anvil_arch_t unsupported_targets[] = {
    ANVIL_ARCH_X86,
    ANVIL_ARCH_S370,
    ANVIL_ARCH_S370_XA,
    ANVIL_ARCH_S390,
    ANVIL_ARCH_PPC32
};

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
    return abi == ANVIL_ABI_MVS ? "mvs" : "sysv";
}

static const char *syntax_name(anvil_syntax_t syntax)
{
    return syntax == ANVIL_SYNTAX_HLASM ? "hlasm" : "gas";
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

static void initialize_ready_profile(
    st_application_aot_profile_t *profile, bundle_storage_t *storage,
    anvil_arch_t target)
{
    st_artifact_bundle_t *bundle = &profile->bundle;
    const bool hlasm = target == ANVIL_ARCH_ZARCH;
    const char *extension = hlasm ? ".asm" : ".s";
    const char *metadata_name = hlasm ? "metadata.asm" : "metadata.s";
    const char *launch_name = hlasm ? "launch.asm" : "launch.s";
    char bundle_hash[ST_ARTIFACT_SHA256_SIZE * 2u + 1u];

    memset(storage, 0, sizeof(*storage));
    memset(bundle, 0, sizeof(*bundle));
    profile->target = target;
    profile->abi = hlasm ? ANVIL_ABI_MVS : ANVIL_ABI_SYSV;
    profile->syntax = hlasm ? ANVIL_SYNTAX_HLASM : ANVIL_SYNTAX_GAS;
    profile->optimization = ANVIL_OPT_STANDARD;
    profile->state = ST_APPLICATION_PROFILE_READY;

    snprintf(
        (char *)storage->metadata, sizeof(storage->metadata),
        "metadata-%s%s", target_name(target), extension);
    snprintf(
        (char *)storage->launch, sizeof(storage->launch),
        "launch-%s%s", target_name(target), extension);

    storage->artifacts[0].kind = ST_ARTIFACT_METADATA_ASSEMBLY;
    storage->artifacts[0].name = (char *)metadata_name;
    storage->artifacts[0].name_length = strlen(metadata_name);
    storage->artifacts[0].symbol = "st_test_descriptor";
    storage->artifacts[0].symbol_length = strlen("st_test_descriptor");
    storage->artifacts[0].bytes = storage->metadata;
    storage->artifacts[0].size = strlen((char *)storage->metadata);
    CHECK(st_artifact_sha256(
        storage->metadata, storage->artifacts[0].size,
        storage->artifacts[0].sha256));

    storage->artifacts[1].kind = ST_ARTIFACT_LAUNCH_ASSEMBLY;
    storage->artifacts[1].name = (char *)launch_name;
    storage->artifacts[1].name_length = strlen(launch_name);
    storage->artifacts[1].symbol = "st_test_launch_descriptor";
    storage->artifacts[1].symbol_length =
        strlen("st_test_launch_descriptor");
    storage->artifacts[1].bytes = storage->launch;
    storage->artifacts[1].size = strlen((char *)storage->launch);
    CHECK(st_artifact_sha256(
        storage->launch, storage->artifacts[1].size,
        storage->artifacts[1].sha256));

    for (size_t index = 0u; index < ST_ARTIFACT_SHA256_SIZE; index++) {
        bundle->bundle_sha256[index] =
            (uint8_t)((unsigned)target * 17u + index);
    }
    hash_hex(bundle->bundle_sha256, bundle_hash);
    snprintf(
        storage->manifest, sizeof(storage->manifest),
        "anvil-smalltalk-artifact-bundle-v2\n"
        "target=%s\nabi=%s\nsyntax=%s\noptimization=O2\n"
        "launch=present\nbundle-sha256=%s\n",
        target_name(target), abi_name(profile->abi),
        syntax_name(profile->syntax), bundle_hash);
    bundle->status = ST_ARTIFACT_BUNDLE_OK;
    bundle->target = profile->target;
    bundle->abi = profile->abi;
    bundle->syntax = profile->syntax;
    bundle->optimization = profile->optimization;
    bundle->artifacts = storage->artifacts;
    bundle->artifact_count = 2u;
    bundle->manifest = storage->manifest;
    bundle->manifest_length = strlen(storage->manifest);
    bundle->implementation = storage;
}

static void initialize_application(
    st_application_aot_result_t *application,
    bundle_storage_t storage[ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT],
    char matrix[4096])
{
    size_t used = 0u;

    memset(application, 0, sizeof(*application));
    application->status = ST_APPLICATION_AOT_OK;
    application->profile_count = ST_APPLICATION_AOT_PROFILE_COUNT;
    used += (size_t)snprintf(
        matrix + used, 4096u - used,
        "anvil-smalltalk-application-matrix-v1\n"
        "application=hello\nprofile-count=10\n");

    for (size_t index = 0u;
         index < ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT; index++) {
        st_application_aot_profile_t *profile = &application->profiles[index];
        char hash[ST_ARTIFACT_SHA256_SIZE * 2u + 1u];

        initialize_ready_profile(
            profile, &storage[index], supported_targets[index]);
        hash_hex(profile->bundle.bundle_sha256, hash);
        used += (size_t)snprintf(
            matrix + used, 4096u - used,
            "profile=%s|%s|%s|O2|ready|%s\n",
            target_name(profile->target), abi_name(profile->abi),
            syntax_name(profile->syntax), hash);
    }
    for (size_t index = 0u;
         index < sizeof(unsupported_targets) / sizeof(unsupported_targets[0]);
         index++) {
        st_application_aot_profile_t *profile =
            &application->profiles[
                ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT + index];

        profile->target = unsupported_targets[index];
        profile->abi = ANVIL_ABI_DEFAULT;
        profile->syntax = ANVIL_SYNTAX_DEFAULT;
        profile->optimization = ANVIL_OPT_STANDARD;
        profile->state = ST_APPLICATION_PROFILE_UNSUPPORTED;
        profile->reason = "tagged32-abi-unimplemented";
        used += (size_t)snprintf(
            matrix + used, 4096u - used,
            "profile=%s|default|default|O2|unsupported|"
            "tagged32-abi-unimplemented\n",
            target_name(profile->target));
    }
    CHECK(used < 4096u);
    application->matrix_manifest = matrix;
    application->matrix_manifest_length = used;
}

static ssize_t failing_write(
    void *user, int file_descriptor, const void *bytes, size_t length)
{
    write_failure_t *failure = user;

    if (failure->written >= failure->bytes_before_failure) {
        errno = ENOSPC;
        return -1;
    }
    if (length > failure->bytes_before_failure - failure->written) {
        length = failure->bytes_before_failure - failure->written;
    }
    {
        ssize_t amount = write(file_descriptor, bytes, length);
        if (amount > 0) {
            failure->written += (size_t)amount;
        }
        return amount;
    }
}

static bool make_directory(char path[160])
{
    int amount = snprintf(
        path, 160u, "/tmp/anvil-st-application-materialize-XXXXXX");

    return amount > 0 && amount < 160 && mkdtemp(path) != NULL;
}

static bool path_exists(const char *root, const char *suffix)
{
    char path[512];
    int amount = snprintf(path, sizeof(path), "%s/%s", root, suffix);

    return amount > 0 && (size_t)amount < sizeof(path)
        && access(path, F_OK) == 0;
}

static size_t staging_count(const char *root)
{
    DIR *directory = opendir(root);
    struct dirent *entry;
    size_t count = 0u;

    if (directory == NULL) {
        return SIZE_MAX;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(
                entry->d_name, ".anvil-application-staging-",
                sizeof(".anvil-application-staging-") - 1u) == 0) {
            count++;
        }
    }
    (void)closedir(directory);
    return count;
}

static void remove_application(
    const char *root, const st_application_aot_result_t *application)
{
    char path[512];

    for (size_t profile = 0u;
         profile < ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT; profile++) {
        const st_artifact_bundle_t *bundle =
            &application->profiles[profile].bundle;
        char profile_name[ST_ARTIFACT_PROFILE_NAME_MAX];
        int amount = snprintf(
            profile_name, sizeof(profile_name), "%s-%s-%s-O2",
            target_name(bundle->target), abi_name(bundle->abi),
            syntax_name(bundle->syntax));

        CHECK(amount > 0 && (size_t)amount < sizeof(profile_name));
        for (size_t artifact = 0u; artifact < bundle->artifact_count;
             artifact++) {
            snprintf(
                path, sizeof(path), "%s/hello/%s/%s", root, profile_name,
                bundle->artifacts[artifact].name);
            CHECK(unlink(path) == 0);
        }
        snprintf(
            path, sizeof(path), "%s/hello/%s/bundle.manifest",
            root, profile_name);
        CHECK(unlink(path) == 0);
        snprintf(path, sizeof(path), "%s/hello/%s", root, profile_name);
        CHECK(rmdir(path) == 0);
    }
    snprintf(path, sizeof(path), "%s/hello/matrix.manifest", root);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/hello", root);
    CHECK(rmdir(path) == 0);
    CHECK(rmdir(root) == 0);
}

static void test_complete_publication_and_collision(
    st_application_aot_result_t *application)
{
    char root[160];
    st_application_materialize_result_t result;
    st_application_materialize_options_t options = {0};

    CHECK(make_directory(root));
    st_application_materialize_result_init(&result);
    CHECK(st_application_aot_materialize(
              &result, application, "hello", root, &options)
          == ST_APPLICATION_MATERIALIZE_OK);
    CHECK(result.committed);
    CHECK(path_exists(root, "hello/matrix.manifest"));
    CHECK(path_exists(root, "hello/x86_64-sysv-gas-O2/launch.s"));
    CHECK(path_exists(root, "hello/arm64-sysv-gas-O2/metadata.s"));
    CHECK(path_exists(root, "hello/zarch-mvs-hlasm-O2/launch.asm"));
    CHECK(!path_exists(root, "hello/x86-default-default-O2"));
    CHECK(staging_count(root) == 0u);

    st_application_materialize_result_init(&result);
    CHECK(st_application_aot_materialize(
              &result, application, "hello", root, &options)
          == ST_APPLICATION_MATERIALIZE_ERR_COLLISION);
    CHECK(!result.committed && staging_count(root) == 0u);
    remove_application(root, application);
}

static void test_failure_rolls_back_whole_matrix(
    st_application_aot_result_t *application)
{
    char root[160];
    st_application_materialize_result_t result;
    write_failure_t failure = {160u, 0u};
    st_application_materialize_options_t options = {
        .artifact_options = {
            .write = failing_write,
            .write_user = &failure
        }
    };

    CHECK(make_directory(root));
    st_application_materialize_result_init(&result);
    CHECK(st_application_aot_materialize(
              &result, application, "hello", root, &options)
          == ST_APPLICATION_MATERIALIZE_ERR_PROFILE);
    CHECK(!result.committed);
    CHECK(!path_exists(root, "hello"));
    CHECK(staging_count(root) == 0u);
    CHECK(rmdir(root) == 0);
}

static void test_manifest_tamper_is_rejected(
    st_application_aot_result_t *application)
{
    char root[160];
    st_application_materialize_result_t result;
    st_application_materialize_options_t options = {0};
    char saved = application->matrix_manifest[0];

    CHECK(make_directory(root));
    application->matrix_manifest[0] = 'X';
    st_application_materialize_result_init(&result);
    CHECK(st_application_aot_materialize(
              &result, application, "hello", root, &options)
          == ST_APPLICATION_MATERIALIZE_ERR_INVALID_RESULT);
    application->matrix_manifest[0] = saved;
    CHECK(!path_exists(root, "hello"));
    CHECK(rmdir(root) == 0);
}

int main(void)
{
    st_application_aot_result_t application;
    bundle_storage_t storage[ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT];
    char matrix[4096];

    initialize_application(&application, storage, matrix);
    test_complete_publication_and_collision(&application);
    test_failure_rolls_back_whole_matrix(&application);
    test_manifest_tamper_is_rejected(&application);

    if (failures != 0u) {
        fprintf(
            stderr, "Smalltalk application materializer: %u failure(s)\n",
            failures);
        return 1;
    }
    puts("Smalltalk application materializer: PASS "
         "(atomic five-profile matrix, honest narrow records, rollback)");
    return 0;
}
