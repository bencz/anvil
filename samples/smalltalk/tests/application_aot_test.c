#include "st_application_aot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n",                 \
                    __FILE__, __LINE__, #condition);                         \
            failures++;                                                     \
        }                                                                    \
    } while (0)

typedef struct {
    size_t calls;
    size_t live;
    size_t fail_at;
} fault_allocator_t;

static void *fault_allocate(void *user, size_t size)
{
    fault_allocator_t *fault = user;
    void *pointer;

    fault->calls++;
    if (fault->calls == fault->fail_at) {
        return NULL;
    }
    pointer = malloc(size);
    if (pointer != NULL) {
        fault->live++;
    }
    return pointer;
}

static void fault_deallocate(void *user, void *pointer)
{
    fault_allocator_t *fault = user;
    if (pointer != NULL) {
        CHECK(fault->live != 0u);
        fault->live--;
        free(pointer);
    }
}

static const char *existing_directory(const char *local, const char *root)
{
    if (access(local, R_OK) == 0) {
        return local;
    }
    if (access(root, R_OK) == 0) {
        return root;
    }
    return NULL;
}

static size_t artifact_kind_count(
    const st_artifact_bundle_t *bundle, st_artifact_kind_t kind)
{
    size_t count = 0u;
    for (size_t index = 0u; index < bundle->artifact_count; index++) {
        if (bundle->artifacts[index].kind == kind) {
            count++;
        }
    }
    return count;
}

static st_application_aot_options_t options(void)
{
    return (st_application_aot_options_t) {
        .image_directory = existing_directory(
            "st-image", "samples/smalltalk/st-image"),
        .application_directory = existing_directory(
            "examples/hello", "samples/smalltalk/examples/hello"),
        .application_name = "hello",
        .entry_class_name = "HelloApplication",
        .entry_selector = "run",
        .optimization = ANVIL_OPT_STANDARD
    };
}

static void test_hello_matrix(void)
{
    st_application_aot_options_t configuration = options();
    st_application_aot_result_t result;
    size_t ready = 0u;
    size_t unsupported = 0u;

    CHECK(configuration.image_directory != NULL);
    CHECK(configuration.application_directory != NULL);
    st_application_aot_result_init(&result);
    CHECK(st_application_aot_compile(&result, &configuration)
          == ST_APPLICATION_AOT_OK);
    CHECK(result.profile_count == ST_APPLICATION_AOT_PROFILE_COUNT);
    CHECK(result.matrix_manifest != NULL
          && result.matrix_manifest_length != 0u);
    CHECK(strstr(
              result.matrix_manifest,
              "anvil-smalltalk-application-matrix-v1\n")
          == result.matrix_manifest);
    CHECK(strstr(result.matrix_manifest, "application=hello\n") != NULL);
    CHECK(strstr(
              result.matrix_manifest,
              "x86|default|default|O2|unsupported|"
              "tagged32-abi-unimplemented") != NULL);

    for (size_t index = 0u; index < result.profile_count; index++) {
        const st_application_aot_profile_t *profile =
            &result.profiles[index];
        if (profile->state == ST_APPLICATION_PROFILE_READY) {
            ready++;
            CHECK(profile->bundle.status == ST_ARTIFACT_BUNDLE_OK);
            CHECK(profile->bundle.artifact_count > 300u);
            CHECK(artifact_kind_count(
                      &profile->bundle, ST_ARTIFACT_METADATA_ASSEMBLY)
                  == 1u);
            CHECK(artifact_kind_count(
                      &profile->bundle, ST_ARTIFACT_LAUNCH_ASSEMBLY)
                  == 1u);
            CHECK(profile->bundle.manifest != NULL
                  && strstr(profile->bundle.manifest, "launch=present\n")
                      != NULL);
        } else {
            unsupported++;
            CHECK(profile->reason != NULL
                  && strcmp(
                      profile->reason,
                      "tagged32-abi-unimplemented") == 0);
            CHECK(profile->bundle.artifacts == NULL
                  && profile->bundle.artifact_count == 0u);
        }
    }
    CHECK(ready == ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT);
    CHECK(unsupported == ST_APPLICATION_AOT_PROFILE_COUNT
          - ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT);
    st_application_aot_result_destroy(&result);
}

static void test_invalid_role_and_early_oom(void)
{
    st_application_aot_options_t configuration = options();
    st_application_aot_result_t result;
    fault_allocator_t fault = {0u, 0u, 1u};

    configuration.entry_class_name = "MissingApplication";
    st_application_aot_result_init(&result);
    CHECK(st_application_aot_compile(&result, &configuration)
          == ST_APPLICATION_AOT_ERR_ROLE);
    CHECK(result.failed_stage == ST_APPLICATION_AOT_STAGE_ROLES);
    CHECK(result.profile_count == 0u && result.matrix_manifest == NULL);
    st_application_aot_result_destroy(&result);

    configuration = options();
    configuration.allocator = (st_application_aot_allocator_t) {
        fault_allocate, fault_deallocate, &fault
    };
    st_application_aot_result_init(&result);
    CHECK(st_application_aot_compile(&result, &configuration)
          == ST_APPLICATION_AOT_ERR_SOURCE);
    CHECK(result.source_status == ST_SOURCE_LOAD_ERR_OUT_OF_MEMORY);
    CHECK(result.profile_count == 0u && result.matrix_manifest == NULL);
    st_application_aot_result_destroy(&result);
    CHECK(fault.live == 0u);
}

int main(void)
{
    test_hello_matrix();
    test_invalid_role_and_early_oom();
    if (failures != 0u) {
        fprintf(stderr, "Smalltalk application AOT: %u failure(s)\n",
                failures);
        return 1;
    }
    puts("Smalltalk application AOT: PASS "
         "(Hello full image, 5 ready + 5 honest unsupported profiles)");
    return 0;
}
