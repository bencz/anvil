#ifndef MCC_PLATFORM_HOST_H
#define MCC_PLATFORM_HOST_H

#include "mcc.h"

typedef struct
{
    mcc_arch_t default_target;
    const char *(*last_path_separator)(const char *path);
} mcc_host_ops_t;

extern const mcc_host_ops_t mcc_host;

#endif
