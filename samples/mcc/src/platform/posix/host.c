#include "platform/host.h"

static const char *last_path_separator(const char *path)
{
    return strrchr(path, '/');
}

const mcc_host_ops_t mcc_host = {
    .default_target = MCC_ARCH_X86_64,
    .last_path_separator = last_path_separator,
};
