#ifndef ANVIL_X86_64_REGS_H
#define ANVIL_X86_64_REGS_H

#include "../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum X86_64_Reg {
    X86_64_RAX = 0,
    X86_64_RCX = 1,
    X86_64_RDX = 2,
    X86_64_RBX = 3,
    X86_64_RSP = 4,
    X86_64_RBP = 5,
    X86_64_RSI = 6,
    X86_64_RDI = 7,
    X86_64_R8  = 8,
    X86_64_R9  = 9,
    X86_64_R10 = 10,
    X86_64_R11 = 11,
    X86_64_R12 = 12,
    X86_64_R13 = 13,
    X86_64_R14 = 14,
    X86_64_R15 = 15,
    
    X86_64_XMM0 = 16,
    X86_64_XMM1 = 17,
    X86_64_XMM2 = 18,
    X86_64_XMM3 = 19,
    X86_64_XMM4 = 20,
    X86_64_XMM5 = 21,
    X86_64_XMM6 = 22,
    X86_64_XMM7 = 23,
    X86_64_XMM8 = 24,
    X86_64_XMM9 = 25,
    X86_64_XMM10 = 26,
    X86_64_XMM11 = 27,
    X86_64_XMM12 = 28,
    X86_64_XMM13 = 29,
    X86_64_XMM14 = 30,
    X86_64_XMM15 = 31,
    
    X86_64_EFLAGS = 32,
    X86_64_RIP = 33,
    
    X86_64_NUM_REGS = 34,
} X86_64_Reg;

extern const AnvilRegSet x86_64_reg_set;
extern const AnvilRegInfo x86_64_regs[];

const char* x86_64_reg_name(int reg, int size);
const char* x86_64_reg_name_64(int reg);
const char* x86_64_reg_name_32(int reg);
const char* x86_64_reg_name_16(int reg);
const char* x86_64_reg_name_8(int reg);

#ifdef __cplusplus
}
#endif

#endif
