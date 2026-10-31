#include "target.h"

#define MACROS(list) .macros = list, .macro_count = sizeof(list) / sizeof(list[0])

/* Architecture, data model and byte order, spelled as GCC and Clang
 * predefine them for the same target. */

static const mcc_target_macro_t x86_macros[] = {
    { "__i386__", "1" },
    { "__i386", "1" },
    { "__ILP32__", "1" },
    { "__SIZEOF_POINTER__", "4" },
    { "__SIZEOF_LONG__", "4" },
    { "__LITTLE_ENDIAN__", "1" },
};

static const mcc_target_macro_t x86_64_macros[] = {
    { "__x86_64__", "1" },
    { "__x86_64", "1" },
    { "__amd64__", "1" },
    { "__amd64", "1" },
    { "__LP64__", "1" },
    { "_LP64", "1" },
    { "__SIZEOF_POINTER__", "8" },
    { "__SIZEOF_LONG__", "8" },
    { "__LITTLE_ENDIAN__", "1" },
};

static const mcc_target_macro_t windows_macros[] = {
    { "_WIN32", "1" },
    { "_WIN64", "1" },
    { "_M_X64", "100" },
    { "_M_AMD64", "100" },
    { "__x86_64__", "1" },
    { "__SIZEOF_POINTER__", "8" },
    { "__SIZEOF_LONG__", "4" },
    { "__LITTLE_ENDIAN__", "1" },
};

static const mcc_target_macro_t arm64_macros[] = {
    { "__aarch64__", "1" },
    { "__LP64__", "1" },
    { "_LP64", "1" },
    { "__SIZEOF_POINTER__", "8" },
    { "__SIZEOF_LONG__", "8" },
    { "__LITTLE_ENDIAN__", "1" },
};

static const mcc_target_macro_t darwin_macros[] = {
    { "__aarch64__", "1" },
    { "__arm64__", "1" },
    { "__APPLE__", "1" },
    { "__MACH__", "1" },
    { "__LP64__", "1" },
    { "_LP64", "1" },
    { "__SIZEOF_POINTER__", "8" },
    { "__SIZEOF_LONG__", "8" },
    { "__LITTLE_ENDIAN__", "1" },
};

static const mcc_target_macro_t ppc32_macros[] = {
    { "__powerpc__", "1" },
    { "__PPC__", "1" },
    { "_ARCH_PPC", "1" },
    { "__ILP32__", "1" },
    { "__SIZEOF_POINTER__", "4" },
    { "__SIZEOF_LONG__", "4" },
    { "__BIG_ENDIAN__", "1" },
};

static const mcc_target_macro_t ppc64_macros[] = {
    { "__powerpc__", "1" },
    { "__powerpc64__", "1" },
    { "__PPC64__", "1" },
    { "_ARCH_PPC", "1" },
    { "_ARCH_PPC64", "1" },
    { "_CALL_ELF", "1" },
    { "__LP64__", "1" },
    { "_LP64", "1" },
    { "__SIZEOF_POINTER__", "8" },
    { "__SIZEOF_LONG__", "8" },
    { "__BIG_ENDIAN__", "1" },
};

static const mcc_target_macro_t ppc64le_macros[] = {
    { "__powerpc__", "1" },
    { "__powerpc64__", "1" },
    { "__PPC64__", "1" },
    { "_ARCH_PPC", "1" },
    { "_ARCH_PPC64", "1" },
    { "_CALL_ELF", "2" },
    { "__LP64__", "1" },
    { "_LP64", "1" },
    { "__SIZEOF_POINTER__", "8" },
    { "__SIZEOF_LONG__", "8" },
    { "__LITTLE_ENDIAN__", "1" },
};

static const mcc_target_macro_t mainframe31_macros[] = {
    { "__MVS__", "1" },
    { "__s390__", "1" },
    { "__SIZEOF_POINTER__", "4" },
    { "__SIZEOF_LONG__", "4" },
    { "__BIG_ENDIAN__", "1" },
};

static const mcc_target_macro_t zarch_macros[] = {
    { "__MVS__", "1" },
    { "__s390__", "1" },
    { "__s390x__", "1" },
    { "__LP64__", "1" },
    { "__SIZEOF_POINTER__", "8" },
    { "__SIZEOF_LONG__", "8" },
    { "__BIG_ENDIAN__", "1" },
};

static const mcc_target_model_t x86_model = {
    .abi = ANVIL_ABI_DEFAULT,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    MACROS(x86_macros),
};

static const mcc_target_model_t sysv_x64_model = {
    .abi = ANVIL_ABI_SYSV,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    .include_subdirectory = "targets/x86_64_sysv",
    MACROS(x86_64_macros),
};

static const mcc_target_model_t windows_x64_model = {
    .abi = ANVIL_ABI_WIN64,
    .long_matches_pointer = false,
    .native_aggregate_plans = true,
    .size_type = "unsigned long long",
    .ptrdiff_type = "long long",
    .wchar_type = "unsigned short",
    .include_subdirectory = "targets/windows",
    MACROS(windows_macros),
};

static const mcc_target_model_t aapcs64_model = {
    .abi = ANVIL_ABI_SYSV,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    .include_subdirectory = "targets/aapcs64",
    MACROS(arm64_macros),
};

static const mcc_target_model_t darwin_model = {
    .abi = ANVIL_ABI_DARWIN,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    MACROS(darwin_macros),
};

static const mcc_target_model_t ppc32_model = {
    .abi = ANVIL_ABI_DEFAULT,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    .include_subdirectory = "targets/ppc32_sysv",
    MACROS(ppc32_macros),
};

static const mcc_target_model_t ppc64_model = {
    .abi = ANVIL_ABI_DEFAULT,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    MACROS(ppc64_macros),
};

static const mcc_target_model_t ppc64le_model = {
    .abi = ANVIL_ABI_DEFAULT,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    MACROS(ppc64le_macros),
};

static const mcc_target_model_t mainframe31_model = {
    .abi = ANVIL_ABI_DEFAULT,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    MACROS(mainframe31_macros),
};

static const mcc_target_model_t zarch_model = {
    .abi = ANVIL_ABI_DEFAULT,
    .long_matches_pointer = true,
    .size_type = "unsigned long",
    .ptrdiff_type = "long",
    .wchar_type = "int",
    MACROS(zarch_macros),
};

const mcc_target_model_t *mcc_target_model(mcc_arch_t arch)
{
    switch (arch)
    {
    case MCC_ARCH_X86:
        return &x86_model;
    case MCC_ARCH_X86_64:
        return &sysv_x64_model;
    case MCC_ARCH_X86_64_WINDOWS:
        return &windows_x64_model;
    case MCC_ARCH_ARM64:
        return &aapcs64_model;
    case MCC_ARCH_ARM64_MACOS:
        return &darwin_model;
    case MCC_ARCH_PPC32:
        return &ppc32_model;
    case MCC_ARCH_PPC64:
        return &ppc64_model;
    case MCC_ARCH_PPC64LE:
        return &ppc64le_model;
    case MCC_ARCH_ZARCH:
        return &zarch_model;
    case MCC_ARCH_S370:
    case MCC_ARCH_S370_XA:
    case MCC_ARCH_S390:
    default:
        return &mainframe31_model;
    }
}
