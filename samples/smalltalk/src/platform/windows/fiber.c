#include "../fiber.h"

#include <windows.h>
#include <errno.h>
#include <stdlib.h>

struct st_native_fiber {
    void *handle;
    st_native_fiber_entry_fn entry;
    void *argument;
    int saved_errno;
    bool converted;
};

static VOID WINAPI enter_fiber(void *argument)
{
    st_native_fiber_t *fiber = argument;

    fiber->entry(fiber->argument);
    abort();
}

static st_native_fiber_t *capture(void)
{
    st_native_fiber_t *fiber = calloc(1u, sizeof(*fiber));

    if (fiber == NULL)
        return NULL;

    fiber->converted = !IsThreadAFiber();
    fiber->handle = fiber->converted
        ? ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH) : GetCurrentFiber();

    if (fiber->handle == NULL) {
        free(fiber);
        return NULL;
    }

    return fiber;
}

static st_native_fiber_t *create(size_t stack_size, st_native_fiber_entry_fn entry, void *argument)
{
    st_native_fiber_t *fiber;

    if (entry == NULL || stack_size < 65536u)
        return NULL;

    fiber = calloc(1u, sizeof(*fiber));
    if (fiber == NULL)
        return NULL;

    fiber->entry = entry;
    fiber->argument = argument;
    fiber->handle = CreateFiberEx(65536u, stack_size, FIBER_FLAG_FLOAT_SWITCH, enter_fiber, fiber);

    if (fiber->handle == NULL) {
        free(fiber);
        return NULL;
    }

    return fiber;
}

static void transfer(st_native_fiber_t *from, st_native_fiber_t *to)
{
    from->saved_errno = errno;
    SwitchToFiber(to->handle);
    errno = from->saved_errno;
}

static void destroy(st_native_fiber_t *fiber)
{
    if (fiber == NULL)
        return;

    DeleteFiber(fiber->handle);
    free(fiber);
}

static bool release(st_native_fiber_t *root)
{
    if (root == NULL || GetCurrentFiber() != root->handle)
        return false;

    if (root->converted && !ConvertFiberToThread())
        return false;

    free(root);
    return true;
}

static uint64_t milliseconds(void)
{
    return GetTickCount64();
}

static void sleep_milliseconds(uint32_t duration)
{
    Sleep(duration);
}

static uintptr_t thread_id(void)
{
    return (uintptr_t)GetCurrentThreadId();
}

const st_fiber_platform_ops_t st_fiber_platform = {
    .capture = capture,
    .create = create,
    .transfer = transfer,
    .destroy = destroy,
    .release = release,
    .milliseconds = milliseconds,
    .sleep = sleep_milliseconds,
    .thread_id = thread_id
};
