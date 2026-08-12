#include "st_application_startup.h"

#include <stdio.h>

#ifndef ST_APPLICATION_LAUNCH_SYMBOL
#error "ST_APPLICATION_LAUNCH_SYMBOL must name the generated launch descriptor"
#endif

extern const st_application_launch_descriptor_t
    ST_APPLICATION_LAUNCH_SYMBOL;

int main(void)
{
    st_application_startup_context_t startup = {0};
    st_application_startup_options_t options = {
        .launch = &ST_APPLICATION_LAUNCH_SYMBOL
    };
    st_value_t result = ST_VALUE_INVALID;
    int exit_code = 0;
    st_application_startup_status_t status =
        st_application_startup_context_init(&startup, &options);

    if (status == ST_APPLICATION_STARTUP_OK) {
        status = st_application_startup_run(&startup, &result);
    }
    if (status == ST_APPLICATION_STARTUP_OK) {
        status = st_application_startup_exit_code(result, &exit_code);
    }
    if (status != ST_APPLICATION_STARTUP_OK) {
        fprintf(
            stderr, "Anvil Smalltalk startup failed: %s\n",
            st_application_startup_status_string(status));
        exit_code = 70;
    }
    {
        st_application_startup_status_t cleanup =
            st_application_startup_context_destroy(&startup);

        if (cleanup != ST_APPLICATION_STARTUP_OK) {
            fprintf(
                stderr, "Anvil Smalltalk cleanup failed: %s\n",
                st_application_startup_status_string(cleanup));
            exit_code = 70;
        }
    }
    return exit_code;
}
