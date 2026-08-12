#include "st_aot_toolchain.h"

#include <stdio.h>
#include <string.h>

static void usage(const char *program)
{
    fprintf(
        stderr,
        "usage: %s PROFILE_DIR OUTPUT_DIR PUBLICATION_NAME "
        "EXECUTABLE_NAME LINK_INPUT...\n",
        program);
}

int main(int argc, char **argv)
{
    static const char *const allowed_compilers[] = {
        "/usr/bin/gcc"
    };
    static const char *const link_arguments[] = {
        "-pthread", "-lm"
    };
    st_aot_toolchain_options_t options;
    st_aot_toolchain_result_t result;
    st_aot_toolchain_status_t status;

    if (argc < 6) {
        usage(argv[0]);
        return 64;
    }
    memset(&options, 0, sizeof(options));
    options.profile_directory = argv[1];
    options.output_directory = argv[2];
    options.publication_name = argv[3];
    options.executable_name = argv[4];
    options.compiler_driver = "/usr/bin/gcc";
    options.allowed_compiler_drivers = allowed_compilers;
    options.allowed_compiler_driver_count = 1u;
    options.runtime_link_inputs = (const char *const *)&argv[5];
    options.runtime_link_input_count = (size_t)argc - 5u;
    options.link_arguments = link_arguments;
    options.link_argument_count = 2u;

    st_aot_toolchain_result_init(&result);
    status = st_aot_toolchain_link(&result, &options);
    if (status != ST_AOT_TOOLCHAIN_OK) {
        bool committed = result.committed;

        fprintf(
            stderr, "st-aot-link: %s during %s",
            st_aot_toolchain_status_string(status),
            st_aot_toolchain_stage_string(result.failed_stage));
        if (result.system_error != 0) {
            fprintf(stderr, ": system error %d", result.system_error);
        }
        if (result.child_exit_code != 0) {
            fprintf(stderr, ": tool exit %d", result.child_exit_code);
        }
        if (result.child_signal != 0) {
            fprintf(stderr, ": tool signal %d", result.child_signal);
        }
        fputc('\n', stderr);
        if (result.tool_stderr_length != 0u) {
            (void)fwrite(
                result.tool_stderr, 1u,
                result.tool_stderr_length, stderr);
            if (result.tool_stderr[
                    result.tool_stderr_length - 1u] != '\n') {
                fputc('\n', stderr);
            }
        }
        st_aot_toolchain_result_destroy(&result);
        return committed ? 74 : 1;
    }
    printf("Linked %s\n", result.published_executable);
    st_aot_toolchain_result_destroy(&result);
    return 0;
}
