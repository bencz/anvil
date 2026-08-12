#include "st_application_aot.h"
#include "st_application_materialize.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *program)
{
    fprintf(
        stderr,
        "usage: %s IMAGE_DIR APPLICATION_DIR APPLICATION_NAME "
        "ENTRY_CLASS ENTRY_SELECTOR OUTPUT_DIR\n",
        program);
}

int main(int argc, char **argv)
{
    st_application_aot_result_t application;
    st_application_materialize_result_t publication;
    st_application_aot_options_t compile_options;
    st_application_materialize_options_t materialize_options = {0};
    st_application_aot_status_t compile_status;
    st_application_materialize_status_t materialize_status;

    if (argc != 7) {
        usage(argv[0]);
        return 64;
    }
    memset(&compile_options, 0, sizeof(compile_options));
    compile_options.image_directory = argv[1];
    compile_options.application_directory = argv[2];
    compile_options.application_name = argv[3];
    compile_options.entry_class_name = argv[4];
    compile_options.entry_selector = argv[5];
    compile_options.optimization = ANVIL_OPT_STANDARD;

    st_application_aot_result_init(&application);
    compile_status = st_application_aot_compile(
        &application, &compile_options);
    if (compile_status != ST_APPLICATION_AOT_OK) {
        fprintf(
            stderr, "st-aotc: %s during %s",
            st_application_aot_status_string(compile_status),
            st_application_aot_stage_string(application.failed_stage));
        if (application.failed_target != ANVIL_ARCH_NONE) {
            fprintf(stderr, " for target %u", application.failed_target);
        }
        if (application.diagnostic[0] != '\0') {
            fprintf(stderr, ": %s", application.diagnostic);
        }
        fputc('\n', stderr);
        st_application_aot_result_destroy(&application);
        return 1;
    }

    st_application_materialize_result_init(&publication);
    materialize_status = st_application_aot_materialize(
        &publication, &application, argv[3], argv[6],
        &materialize_options);
    if (materialize_status != ST_APPLICATION_MATERIALIZE_OK) {
        fprintf(
            stderr, "st-aotc: %s",
            st_application_materialize_status_string(materialize_status));
        if (publication.failed_profile_index != SIZE_MAX) {
            fprintf(
                stderr, " at profile %zu (%s: %s)",
                publication.failed_profile_index, publication.profile,
                st_artifact_materialize_status_string(
                    publication.profile_status));
        }
        if (publication.system_error != 0) {
            fprintf(
                stderr, ": %s", strerror(publication.system_error));
        }
        fputc('\n', stderr);
        st_application_aot_result_destroy(&application);
        return publication.committed ? 74 : 1;
    }

    printf(
        "Published %s/%s: %u ready assembly profiles, "
        "%u explicit unsupported profiles.\n",
        argv[6], publication.application,
        ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT,
        ST_APPLICATION_AOT_PROFILE_COUNT
            - ST_APPLICATION_AOT_SUPPORTED_PROFILE_COUNT);
    st_application_aot_result_destroy(&application);
    return 0;
}
