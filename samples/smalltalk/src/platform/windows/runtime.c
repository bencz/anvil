#include "../runtime.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <malloc.h>
#include <errno.h>
#include <io.h>
#include <limits.h>
#include <stdlib.h>

static int mutex_init(st_platform_mutex_t *mutex)
{
    SRWLOCK *native = malloc(sizeof(*native));
    if (!native)
        return -1;

    InitializeSRWLock(native);
    mutex->native = native;
    return 0;
}

static int mutex_destroy(st_platform_mutex_t *mutex)
{
    free(mutex->native);
    mutex->native = NULL;
    return 0;
}

static int mutex_lock(st_platform_mutex_t *mutex)
{
    AcquireSRWLockExclusive(mutex->native);
    return 0;
}

static int mutex_unlock(st_platform_mutex_t *mutex)
{
    ReleaseSRWLockExclusive(mutex->native);
    return 0;
}

static void *allocate_aligned(size_t alignment, size_t size)
{
    return _aligned_malloc(size, alignment);
}

static int64_t write_bytes(void *user, int descriptor, const void *bytes, size_t byte_count, int *os_error_out)
{
    (void)user;

    unsigned int request = byte_count > UINT_MAX ? UINT_MAX : (unsigned int)byte_count;
    int result = _write(descriptor, bytes, request);

    *os_error_out = result < 0 ? (errno != 0 ? errno : EIO) : 0;
    return result;
}

const st_runtime_platform_ops_t st_runtime_platform = {
    .mutex_init = mutex_init,
    .mutex_destroy = mutex_destroy,
    .mutex_lock = mutex_lock,
    .mutex_unlock = mutex_unlock,
    .aligned_alloc = allocate_aligned,
    .aligned_free = _aligned_free,
    .write_bytes = write_bytes,
};
