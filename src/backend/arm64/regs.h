#ifndef ANVIL_ARM64_REGS_H
#define ANVIL_ARM64_REGS_H

#include "../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ARM64_Reg {
    ARM64_X0 = 0,
    ARM64_X1 = 1,
    ARM64_X2 = 2,
    ARM64_X3 = 3,
    ARM64_X4 = 4,
    ARM64_X5 = 5,
    ARM64_X6 = 6,
    ARM64_X7 = 7,
    ARM64_X8 = 8,
    ARM64_X9 = 9,
    ARM64_X10 = 10,
    ARM64_X11 = 11,
    ARM64_X12 = 12,
    ARM64_X13 = 13,
    ARM64_X14 = 14,
    ARM64_X15 = 15,
    ARM64_X16 = 16,
    ARM64_X17 = 17,
    ARM64_X18 = 18,
    ARM64_X19 = 19,
    ARM64_X20 = 20,
    ARM64_X21 = 21,
    ARM64_X22 = 22,
    ARM64_X23 = 23,
    ARM64_X24 = 24,
    ARM64_X25 = 25,
    ARM64_X26 = 26,
    ARM64_X27 = 27,
    ARM64_X28 = 28,
    ARM64_X29 = 29,
    ARM64_X30 = 30,
    ARM64_SP = 31,
    ARM64_XZR = 32,
    
    ARM64_V0 = 33,
    ARM64_V1 = 34,
    ARM64_V2 = 35,
    ARM64_V3 = 36,
    ARM64_V4 = 37,
    ARM64_V5 = 38,
    ARM64_V6 = 39,
    ARM64_V7 = 40,
    ARM64_V8 = 41,
    ARM64_V9 = 42,
    ARM64_V10 = 43,
    ARM64_V11 = 44,
    ARM64_V12 = 45,
    ARM64_V13 = 46,
    ARM64_V14 = 47,
    ARM64_V15 = 48,
    ARM64_V16 = 49,
    ARM64_V17 = 50,
    ARM64_V18 = 51,
    ARM64_V19 = 52,
    ARM64_V20 = 53,
    ARM64_V21 = 54,
    ARM64_V22 = 55,
    ARM64_V23 = 56,
    ARM64_V24 = 57,
    ARM64_V25 = 58,
    ARM64_V26 = 59,
    ARM64_V27 = 60,
    ARM64_V28 = 61,
    ARM64_V29 = 62,
    ARM64_V30 = 63,
    ARM64_V31 = 64,
    
    ARM64_NZCV = 65,
    ARM64_PC = 66,
    
    ARM64_NUM_REGS = 67,
} ARM64_Reg;

extern const AnvilRegSet arm64_reg_set;
extern const AnvilRegInfo arm64_regs[];

const char* arm64_reg_name(int reg, int size);
const char* arm64_reg_name_64(int reg);
const char* arm64_reg_name_32(int reg);

#ifdef __cplusplus
}
#endif

#endif
