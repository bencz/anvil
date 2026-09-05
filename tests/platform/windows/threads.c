#include "../threads.h"
#include <windows.h>

typedef struct {
    anvil_test_thread_fn function;
    void *argument;
    size_t index;
} thread_argument;

static DWORD WINAPI run_thread(LPVOID opaque)
{
    thread_argument *argument = opaque;
    argument->function(argument->argument, argument->index);
    return 0;
}

bool anvil_test_run_threads(size_t count, anvil_test_thread_fn function, void *argument)
{
    HANDLE threads[8];
    thread_argument arguments[8];
    if (!count || count > 8 || !function)
        return false;

    size_t started = 0;
    for (; started < count; started++)
    {
        arguments[started] = (thread_argument){ function, argument, started };
        threads[started] = CreateThread(NULL, 0, run_thread, &arguments[started], 0, NULL);
        if (!threads[started])
            break;
    }

    bool success = started == count;
    for (size_t index = 0; index < started; index++)
    {
        success &= WaitForSingleObject(threads[index], INFINITE) == WAIT_OBJECT_0;
        CloseHandle(threads[index]);
    }

    return success;
}
