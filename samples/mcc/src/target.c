#include "target.h"

static const mcc_target_macro_t windows_macros[] = {
    { "_WIN32", "1" },
    { "_WIN64", "1" },
    { "__SIZEOF_POINTER__", "8" },
    { "__SIZEOF_LONG__", "4" },
};

static const mcc_target_model_t windows_x64_model = {
    .abi = ANVIL_ABI_WIN64,
    .long_matches_pointer = false,
    .native_aggregate_plans = true,
    .size_type = "unsigned long long",
    .ptrdiff_type = "long long",
    .wchar_type = "unsigned short",
    .include_subdirectory = "targets/windows",
    .macros = windows_macros,
    .macro_count = sizeof(windows_macros) / sizeof(windows_macros[0]),
};

static const mcc_target_model_t darwin_model = {
    .abi = ANVIL_ABI_DARWIN,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
};

static const mcc_target_model_t default_model = {
    .abi = ANVIL_ABI_DEFAULT,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
};

static const mcc_target_model_t sysv_x64_model = {
    .abi = ANVIL_ABI_SYSV,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    .include_subdirectory = "targets/x86_64_sysv",
};

static const mcc_target_model_t aapcs64_model = {
    .abi = ANVIL_ABI_SYSV,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    .include_subdirectory = "targets/aapcs64",
};

const mcc_target_model_t *mcc_target_model(mcc_arch_t arch)
{
    switch (arch)
    {
    case MCC_ARCH_X86_64:
        return &sysv_x64_model;

    case MCC_ARCH_ARM64:
        return &aapcs64_model;

    case MCC_ARCH_X86_64_WINDOWS:
        return &windows_x64_model;

    case MCC_ARCH_ARM64_MACOS:
        return &darwin_model;

    default:
        return &default_model;
    }
}
