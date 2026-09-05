#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include "../fiber.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

struct st_native_fiber {
    ucontext_t context;
    void *mapping;
    size_t mapping_size;
    st_native_fiber_entry_fn entry;
    void *argument;
    int saved_errno;
};

/* makecontext's variadic parameters are int, not portable pointer carriers.
 * The transfer publishes the destination before its first trampoline runs. */
static _Thread_local st_native_fiber_t *entering_fiber;
static _Thread_local unsigned char owner_identity;

static void enter_fiber(void)
{
    st_native_fiber_t *fiber = entering_fiber;

    fiber->entry(fiber->argument);
    abort();
}

static st_native_fiber_t *capture(void)
{
    st_native_fiber_t *volatile fiber = calloc(1u, sizeof(*fiber));

    if (fiber == NULL)
        return NULL;

    if (getcontext(&fiber->context) != 0) {
        free(fiber);
        return NULL;
    }

    return fiber;
}

static st_native_fiber_t *create(size_t stack_size, st_native_fiber_entry_fn entry, void *argument)
{
    long page_size = sysconf(_SC_PAGESIZE);
    st_native_fiber_t *fiber;
    size_t page;

    if (entry == NULL || stack_size < 65536u || page_size <= 0)
        return NULL;

    page = (size_t)page_size;
    if (stack_size > SIZE_MAX - 3u * page)
        return NULL;

    stack_size = ((stack_size + page - 1u) / page) * page;
    fiber = capture();

    if (fiber == NULL)
        return NULL;

    fiber->mapping_size = stack_size + 2u * page;
    fiber->mapping = mmap(NULL, fiber->mapping_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (fiber->mapping == MAP_FAILED) {
        free(fiber);
        return NULL;
    }

    void *stack = (unsigned char *)fiber->mapping + page;

    if (mprotect(stack, stack_size, PROT_READ | PROT_WRITE) != 0) {
        munmap(fiber->mapping, fiber->mapping_size);
        free(fiber);
        return NULL;
    }

    fiber->entry = entry;
    fiber->argument = argument;
    fiber->context.uc_stack.ss_sp = stack;
    fiber->context.uc_stack.ss_size = stack_size;
    fiber->context.uc_link = NULL;
    makecontext(&fiber->context, enter_fiber, 0);
    return fiber;
}

static void transfer(st_native_fiber_t *from, st_native_fiber_t *to)
{
    from->saved_errno = errno;
    entering_fiber = to;
    errno = to->saved_errno;

    if (swapcontext(&from->context, &to->context) != 0)
        abort();

    errno = from->saved_errno;
}

static void destroy(st_native_fiber_t *fiber)
{
    if (fiber == NULL)
        return;

    munmap(fiber->mapping, fiber->mapping_size);
    free(fiber);
}

static bool release(st_native_fiber_t *root)
{
    if (root == NULL || root->mapping != NULL)
        return false;

    free(root);
    return true;
}

static uint64_t milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        abort();

    return (uint64_t)now.tv_sec * UINT64_C(1000) + (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static void sleep_milliseconds(uint32_t duration)
{
    struct timespec remaining = {
        .tv_sec = (time_t)(duration / 1000u),
        .tv_nsec = (long)(duration % 1000u) * 1000000L
    };

    while (nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR)
            abort();
    }
}

static uintptr_t thread_id(void)
{
    return (uintptr_t)&owner_identity;
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
