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
    
    X86_64_YMM0 = 32,
    X86_64_YMM1 = 33,
    X86_64_YMM2 = 34,
    X86_64_YMM3 = 35,
    X86_64_YMM4 = 36,
    X86_64_YMM5 = 37,
    X86_64_YMM6 = 38,
    X86_64_YMM7 = 39,
    X86_64_YMM8 = 40,
    X86_64_YMM9 = 41,
    X86_64_YMM10 = 42,
    X86_64_YMM11 = 43,
    X86_64_YMM12 = 44,
    X86_64_YMM13 = 45,
    X86_64_YMM14 = 46,
    X86_64_YMM15 = 47,
    
    X86_64_ZMM0 = 48,
    X86_64_ZMM1 = 49,
    X86_64_ZMM2 = 50,
    X86_64_ZMM3 = 51,
    X86_64_ZMM4 = 52,
    X86_64_ZMM5 = 53,
    X86_64_ZMM6 = 54,
    X86_64_ZMM7 = 55,
    X86_64_ZMM8 = 56,
    X86_64_ZMM9 = 57,
    X86_64_ZMM10 = 58,
    X86_64_ZMM11 = 59,
    X86_64_ZMM12 = 60,
    X86_64_ZMM13 = 61,
    X86_64_ZMM14 = 62,
    X86_64_ZMM15 = 63,
    X86_64_ZMM16 = 64,
    X86_64_ZMM17 = 65,
    X86_64_ZMM18 = 66,
    X86_64_ZMM19 = 67,
    X86_64_ZMM20 = 68,
    X86_64_ZMM21 = 69,
    X86_64_ZMM22 = 70,
    X86_64_ZMM23 = 71,
    X86_64_ZMM24 = 72,
    X86_64_ZMM25 = 73,
    X86_64_ZMM26 = 74,
    X86_64_ZMM27 = 75,
    X86_64_ZMM28 = 76,
    X86_64_ZMM29 = 77,
    X86_64_ZMM30 = 78,
    X86_64_ZMM31 = 79,
    
    X86_64_K0 = 80,
    X86_64_K1 = 81,
    X86_64_K2 = 82,
    X86_64_K3 = 83,
    X86_64_K4 = 84,
    X86_64_K5 = 85,
    X86_64_K6 = 86,
    X86_64_K7 = 87,
    
    X86_64_ST0 = 88,
    X86_64_ST1 = 89,
    X86_64_ST2 = 90,
    X86_64_ST3 = 91,
    X86_64_ST4 = 92,
    X86_64_ST5 = 93,
    X86_64_ST6 = 94,
    X86_64_ST7 = 95,
    
    X86_64_MM0 = 96,
    X86_64_MM1 = 97,
    X86_64_MM2 = 98,
    X86_64_MM3 = 99,
    X86_64_MM4 = 100,
    X86_64_MM5 = 101,
    X86_64_MM6 = 102,
    X86_64_MM7 = 103,
    
    X86_64_EFLAGS = 104,
    X86_64_RIP = 105,
    X86_64_MXCSR = 106,
    
    X86_64_NUM_REGS = 107,
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
