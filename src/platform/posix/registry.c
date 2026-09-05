#include "../registry.h"

#include <pthread.h>

static pthread_once_t registry_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

static void initialize_once(void (*initialize)(void))
{
    pthread_once(&registry_once, initialize);
}

static void lock_registry(void)
{
    pthread_mutex_lock(&registry_mutex);
}

static void unlock_registry(void)
{
    pthread_mutex_unlock(&registry_mutex);
}

const anvil_registry_platform_ops_t anvil_registry_platform = {
    .initialize_once = initialize_once,
    .lock = lock_registry,
    .unlock = unlock_registry,
};
