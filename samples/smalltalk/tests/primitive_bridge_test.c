#include "st_primitive_bridge.h"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }
    if (child == 0) {
        StFrame frame = {0};
        (void)st_aot_core_primitive_contract_violation(
            ST_INTRINSIC_IDENTITY,
            ST_CORE_PRIMITIVE_ERR_INVALID_VALUE, &frame);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status)
            || WTERMSIG(status) != SIGABRT) {
        fputs("primitive contract violation did not abort\n", stderr);
        return 1;
    }
    puts("smalltalk AOT primitive bridge: ok");
    return 0;
}
