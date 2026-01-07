#ifndef ANVIL_PPC64_REGS_H
#define ANVIL_PPC64_REGS_H

#include "../backend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* General Purpose Registers */
    PPC64_R0 = 0,
    PPC64_R1,       /* Stack pointer */
    PPC64_R2,       /* TOC pointer */
    PPC64_R3,       /* First arg / return value */
    PPC64_R4,
    PPC64_R5,
    PPC64_R6,
    PPC64_R7,
    PPC64_R8,
    PPC64_R9,
    PPC64_R10,
    PPC64_R11,      /* Environment pointer */
    PPC64_R12,      /* Function entry address */
    PPC64_R13,      /* Thread pointer */
    PPC64_R14,
    PPC64_R15,
    PPC64_R16,
    PPC64_R17,
    PPC64_R18,
    PPC64_R19,
    PPC64_R20,
    PPC64_R21,
    PPC64_R22,
    PPC64_R23,
    PPC64_R24,
    PPC64_R25,
    PPC64_R26,
    PPC64_R27,
    PPC64_R28,
    PPC64_R29,
    PPC64_R30,
    PPC64_R31,      /* Frame pointer */
    
    /* Floating Point Registers */
    PPC64_F0 = 32,
    PPC64_F1,
    PPC64_F2,
    PPC64_F3,
    PPC64_F4,
    PPC64_F5,
    PPC64_F6,
    PPC64_F7,
    PPC64_F8,
    PPC64_F9,
    PPC64_F10,
    PPC64_F11,
    PPC64_F12,
    PPC64_F13,
    PPC64_F14,
    PPC64_F15,
    PPC64_F16,
    PPC64_F17,
    PPC64_F18,
    PPC64_F19,
    PPC64_F20,
    PPC64_F21,
    PPC64_F22,
    PPC64_F23,
    PPC64_F24,
    PPC64_F25,
    PPC64_F26,
    PPC64_F27,
    PPC64_F28,
    PPC64_F29,
    PPC64_F30,
    PPC64_F31,
    
    /* Condition Register */
    PPC64_CR0 = 64,
    PPC64_CR1,
    PPC64_CR2,
    PPC64_CR3,
    PPC64_CR4,
    PPC64_CR5,
    PPC64_CR6,
    PPC64_CR7,
    
    /* Special Registers */
    PPC64_LR = 72,   /* Link Register */
    PPC64_CTR = 73,  /* Count Register */
    PPC64_XER = 74,  /* Fixed-Point Exception Register */
    
    PPC64_NUM_REGS
} Ppc64Reg;

const char* ppc64_reg_name(int reg);
const char* ppc64_freg_name(int reg);

extern const AnvilRegSet ppc64_reg_set;

#ifdef __cplusplus
}
#endif

#endif
