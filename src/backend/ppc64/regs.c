#include "regs.h"

static const char* gpr_names[] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
    "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31"
};

static const char* fpr_names[] = {
    "f0", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
    "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15",
    "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",
    "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31"
};

const char* ppc64_reg_name(int reg) {
    if (reg >= PPC64_R0 && reg <= PPC64_R31) {
        return gpr_names[reg - PPC64_R0];
    }
    if (reg >= PPC64_F0 && reg <= PPC64_F31) {
        return fpr_names[reg - PPC64_F0];
    }
    if (reg == PPC64_LR) return "lr";
    if (reg == PPC64_CTR) return "ctr";
    return "???";
}

const char* ppc64_freg_name(int reg) {
    if (reg >= PPC64_F0 && reg <= PPC64_F31) {
        return fpr_names[reg - PPC64_F0];
    }
    return "???";
}

static const int ppc64_allocatable_regs[] = {
    PPC64_R3, PPC64_R4, PPC64_R5, PPC64_R6, PPC64_R7, PPC64_R8, PPC64_R9, PPC64_R10,
    PPC64_R14, PPC64_R15, PPC64_R16, PPC64_R17, PPC64_R18, PPC64_R19, PPC64_R20,
    PPC64_R21, PPC64_R22, PPC64_R23, PPC64_R24, PPC64_R25, PPC64_R26, PPC64_R27,
    PPC64_R28, PPC64_R29, PPC64_R30, PPC64_R31
};

static const int ppc64_callee_saved[] = {
    PPC64_R14, PPC64_R15, PPC64_R16, PPC64_R17, PPC64_R18, PPC64_R19, PPC64_R20,
    PPC64_R21, PPC64_R22, PPC64_R23, PPC64_R24, PPC64_R25, PPC64_R26, PPC64_R27,
    PPC64_R28, PPC64_R29, PPC64_R30, PPC64_R31
};

static const int ppc64_caller_saved[] = {
    PPC64_R0, PPC64_R3, PPC64_R4, PPC64_R5, PPC64_R6, PPC64_R7, PPC64_R8, PPC64_R9,
    PPC64_R10, PPC64_R11, PPC64_R12
};

const AnvilRegSet ppc64_reg_set = {
    .name = "ppc64_regs",
    .regs = NULL,
    .num_regs = 0,
    
    .gp_regs = ppc64_allocatable_regs,
    .num_gp_regs = sizeof(ppc64_allocatable_regs) / sizeof(ppc64_allocatable_regs[0]),
    .fp_regs = NULL,
    .num_fp_regs = 0,
    .simd_regs = NULL,
    .num_simd_regs = 0,
    
    .stack_pointer = PPC64_R1,
    .frame_pointer = PPC64_R31,
    .link_register = PPC64_LR,
    .program_counter = -1,
    .flags_register = -1,
    
    .scratch_regs = ppc64_caller_saved,
    .num_scratch_regs = sizeof(ppc64_caller_saved) / sizeof(ppc64_caller_saved[0])
};
