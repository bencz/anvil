#include "platform/host.h"

static const char *last_path_separator(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');

    if (backslash && (!slash || backslash > slash))
        return backslash;

    return slash;
}

const mcc_host_ops_t mcc_host = {
    .default_target = MCC_ARCH_X86_64_WINDOWS,
    .last_path_separator = last_path_separator,
};
