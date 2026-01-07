#ifndef ANVIL_TARGET_H
#define ANVIL_TARGET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANVIL_ARCH_X86_64   0
#define ANVIL_ARCH_ARM64    1
#define ANVIL_ARCH_X86      2
#define ANVIL_ARCH_I8086    3
#define ANVIL_ARCH_ARM32    4
#define ANVIL_ARCH_RISCV64  5
#define ANVIL_ARCH_RISCV32  6
#define ANVIL_ARCH_MIPS     7
#define ANVIL_ARCH_S370     8
#define ANVIL_ARCH_Z80      9
#define ANVIL_ARCH_PPC64    10

#define ANVIL_OS_NONE       0
#define ANVIL_OS_LINUX      1
#define ANVIL_OS_WINDOWS    2
#define ANVIL_OS_MACOS      3
#define ANVIL_OS_BSD        4
#define ANVIL_OS_DOS        5
#define ANVIL_OS_MVS        6

#define ANVIL_OPT_NONE       0
#define ANVIL_OPT_DEBUG      1
#define ANVIL_OPT_BASIC      2
#define ANVIL_OPT_STANDARD   3
#define ANVIL_OPT_AGGRESSIVE 4
#define ANVIL_OPT_SIZE       5

typedef struct AnvilTarget {
    int arch;
    int os;
    const char* abi_name;
    uint64_t features;
} AnvilTarget;

AnvilTarget anvil_target_x86_64_linux(void);
AnvilTarget anvil_target_x86_64_windows(void);
AnvilTarget anvil_target_arm64_linux(void);
AnvilTarget anvil_target_arm64_macos(void);
AnvilTarget anvil_target_ppc64_linux(void);
AnvilTarget anvil_target_from_triple(const char* triple);

#ifdef __cplusplus
}
#endif

#endif
