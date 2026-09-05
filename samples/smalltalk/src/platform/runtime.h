#ifndef ST_PLATFORM_RUNTIME_H
#define ST_PLATFORM_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

/* Native synchronization objects stay inside the selected implementation. */
typedef struct
{
    void *native;
} st_platform_mutex_t;

typedef struct
{
    int (*mutex_init)(st_platform_mutex_t *mutex);
    int (*mutex_destroy)(st_platform_mutex_t *mutex);
    int (*mutex_lock)(st_platform_mutex_t *mutex);
    int (*mutex_unlock)(st_platform_mutex_t *mutex);
    void *(*aligned_alloc)(size_t alignment, size_t size);
    void (*aligned_free)(void *pointer);
    int64_t (*write_bytes)(void *user, int descriptor, const void *bytes, size_t byte_count, int *os_error_out);
} st_runtime_platform_ops_t;

extern const st_runtime_platform_ops_t st_runtime_platform;

#endif
