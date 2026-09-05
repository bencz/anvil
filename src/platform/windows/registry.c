#include "../registry.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static INIT_ONCE registry_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK registry_mutex = SRWLOCK_INIT;

typedef struct
{
    void (*initialize)(void);
} registry_initializer_t;

static BOOL CALLBACK initialize_registry(PINIT_ONCE once, PVOID argument, PVOID *context)
{
    registry_initializer_t *initializer = argument;
    (void)once;
    (void)context;

    initializer->initialize();
    return TRUE;
}

static void initialize_once(void (*initialize)(void))
{
    registry_initializer_t initializer = {initialize};
    InitOnceExecuteOnce(&registry_once, initialize_registry, &initializer, NULL);
}

static void lock_registry(void)
{
    AcquireSRWLockExclusive(&registry_mutex);
}

static void unlock_registry(void)
{
    ReleaseSRWLockExclusive(&registry_mutex);
}

const anvil_registry_platform_ops_t anvil_registry_platform = {
    .initialize_once = initialize_once,
    .lock = lock_registry,
    .unlock = unlock_registry,
};
