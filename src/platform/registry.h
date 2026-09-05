#ifndef ANVIL_PLATFORM_REGISTRY_H
#define ANVIL_PLATFORM_REGISTRY_H

/* Host services for the process-wide backend registry. The build selects one
 * implementation; target ABI selection never changes these operations. */
typedef struct
{
    void (*initialize_once)(void (*initialize)(void));
    void (*lock)(void);
    void (*unlock)(void);
} anvil_registry_platform_ops_t;

extern const anvil_registry_platform_ops_t anvil_registry_platform;

#endif
