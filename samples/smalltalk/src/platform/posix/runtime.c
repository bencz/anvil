#include "../runtime.h"

#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

static int mutex_init(st_platform_mutex_t *mutex)
{
    pthread_mutex_t *native = malloc(sizeof(*native));
    if (!native)
        return -1;

    int status = pthread_mutex_init(native, NULL);
    if (status != 0)
    {
        free(native);
        return status;
    }

    mutex->native = native;
    return 0;
}

static int mutex_destroy(st_platform_mutex_t *mutex)
{
    int status = pthread_mutex_destroy(mutex->native);
    if (status != 0)
        return status;

    free(mutex->native);
    mutex->native = NULL;
    return 0;
}

static int mutex_lock(st_platform_mutex_t *mutex)
{
    return pthread_mutex_lock(mutex->native);
}

static int mutex_unlock(st_platform_mutex_t *mutex)
{
    return pthread_mutex_unlock(mutex->native);
}

static int64_t write_bytes(void *user, int descriptor, const void *bytes, size_t byte_count, int *os_error_out)
{
    (void)user;

    ssize_t result = write(descriptor, bytes, byte_count);

    *os_error_out = result < 0 ? (errno != 0 ? errno : EIO) : 0;
    return result;
}

const st_runtime_platform_ops_t st_runtime_platform = {
    .mutex_init = mutex_init,
    .mutex_destroy = mutex_destroy,
    .mutex_lock = mutex_lock,
    .mutex_unlock = mutex_unlock,
    .aligned_alloc = aligned_alloc,
    .aligned_free = free,
    .write_bytes = write_bytes,
};
