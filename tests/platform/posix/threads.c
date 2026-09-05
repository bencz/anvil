#include "../threads.h"
#include <pthread.h>

typedef struct {
    anvil_test_thread_fn function;
    void *argument;
    size_t index;
} thread_argument;

static void *run_thread(void *opaque)
{
    thread_argument *argument = opaque;
    argument->function(argument->argument, argument->index);
    return NULL;
}

bool anvil_test_run_threads(size_t count, anvil_test_thread_fn function, void *argument)
{
    pthread_t threads[8];
    thread_argument arguments[8];
    if (!count || count > 8 || !function)
        return false;

    size_t started = 0;
    for (; started < count; started++)
    {
        arguments[started] = (thread_argument){ function, argument, started };
        if (pthread_create(&threads[started], NULL, run_thread, &arguments[started]) != 0)
            break;
    }

    bool success = started == count;
    for (size_t index = 0; index < started; index++)
        success &= pthread_join(threads[index], NULL) == 0;

    return success;
}
