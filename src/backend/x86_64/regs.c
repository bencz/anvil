#include "regs.h"

const AnvilRegInfo x86_64_regs[] = {
    { "rax", X86_64_RAX, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 0 },
    { "rcx", X86_64_RCX, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 2 },
    { "rdx", X86_64_RDX, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 1 },
    { "rbx", X86_64_RBX, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 3 },
    { "rsp", X86_64_RSP, 64, ANVIL_REG_CLASS_SPECIAL, -1, 0, 64, 7 },
    { "rbp", X86_64_RBP, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 6 },
    { "rsi", X86_64_RSI, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 4 },
    { "rdi", X86_64_RDI, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 5 },
    { "r8",  X86_64_R8,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 8 },
    { "r9",  X86_64_R9,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 9 },
    { "r10", X86_64_R10, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 10 },
    { "r11", X86_64_R11, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 11 },
    { "r12", X86_64_R12, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 12 },
    { "r13", X86_64_R13, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 13 },
    { "r14", X86_64_R14, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 14 },
    { "r15", X86_64_R15, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 15 },
    
    { "xmm0",  X86_64_XMM0,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 17 },
    { "xmm1",  X86_64_XMM1,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 18 },
    { "xmm2",  X86_64_XMM2,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 19 },
    { "xmm3",  X86_64_XMM3,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 20 },
    { "xmm4",  X86_64_XMM4,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 21 },
    { "xmm5",  X86_64_XMM5,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 22 },
    { "xmm6",  X86_64_XMM6,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 23 },
    { "xmm7",  X86_64_XMM7,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 24 },
    { "xmm8",  X86_64_XMM8,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 25 },
    { "xmm9",  X86_64_XMM9,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 26 },
    { "xmm10", X86_64_XMM10, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 27 },
    { "xmm11", X86_64_XMM11, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 28 },
    { "xmm12", X86_64_XMM12, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 29 },
    { "xmm13", X86_64_XMM13, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 30 },
    { "xmm14", X86_64_XMM14, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 31 },
    { "xmm15", X86_64_XMM15, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 32 },
    
    { "eflags", X86_64_EFLAGS, 64, ANVIL_REG_CLASS_SPECIAL, -1, 0, 64, 49 },
    { "rip", X86_64_RIP, 64, ANVIL_REG_CLASS_SPECIAL, -1, 0, 64, 16 },
};

static const int x86_64_gp_regs[] = {
    X86_64_RAX, X86_64_RCX, X86_64_RDX, X86_64_RBX,
    X86_64_RSI, X86_64_RDI, X86_64_R8, X86_64_R9,
    X86_64_R10, X86_64_R11, X86_64_R12, X86_64_R13,
    X86_64_R14, X86_64_R15, X86_64_RBP
};

static const int x86_64_fp_regs[] = {
    X86_64_XMM0, X86_64_XMM1, X86_64_XMM2, X86_64_XMM3,
    X86_64_XMM4, X86_64_XMM5, X86_64_XMM6, X86_64_XMM7,
    X86_64_XMM8, X86_64_XMM9, X86_64_XMM10, X86_64_XMM11,
    X86_64_XMM12, X86_64_XMM13, X86_64_XMM14, X86_64_XMM15
};

static const int x86_64_scratch_regs[] = {
    X86_64_RAX, X86_64_RCX, X86_64_RDX, X86_64_RSI, X86_64_RDI,
    X86_64_R8, X86_64_R9, X86_64_R10, X86_64_R11
};

const AnvilRegSet x86_64_reg_set = {
    .name = "x86_64_regs",
    .regs = x86_64_regs,
    .num_regs = X86_64_NUM_REGS,
    
    .gp_regs = x86_64_gp_regs,
    .num_gp_regs = sizeof(x86_64_gp_regs) / sizeof(x86_64_gp_regs[0]),
    .fp_regs = x86_64_fp_regs,
    .num_fp_regs = sizeof(x86_64_fp_regs) / sizeof(x86_64_fp_regs[0]),
    .simd_regs = x86_64_fp_regs,
    .num_simd_regs = sizeof(x86_64_fp_regs) / sizeof(x86_64_fp_regs[0]),
    
    .stack_pointer = X86_64_RSP,
    .frame_pointer = X86_64_RBP,
    .link_register = -1,
    .program_counter = X86_64_RIP,
    .flags_register = X86_64_EFLAGS,
    
    .scratch_regs = x86_64_scratch_regs,
    .num_scratch_regs = sizeof(x86_64_scratch_regs) / sizeof(x86_64_scratch_regs[0]),
};

static const char* reg_names_64[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

static const char* reg_names_32[] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
    "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d"
};

static const char* reg_names_16[] = {
    "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
    "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w"
};

static const char* reg_names_8[] = {
    "al", "cl", "dl", "bl", "spl", "bpl", "sil", "dil",
    "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b"
};

static const char* xmm_names[] = {
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
    "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15"
};

static const char* ymm_names[] = {
    "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5", "ymm6", "ymm7",
    "ymm8", "ymm9", "ymm10", "ymm11", "ymm12", "ymm13", "ymm14", "ymm15"
};

static const char* zmm_names[] = {
    "zmm0", "zmm1", "zmm2", "zmm3", "zmm4", "zmm5", "zmm6", "zmm7",
    "zmm8", "zmm9", "zmm10", "zmm11", "zmm12", "zmm13", "zmm14", "zmm15",
    "zmm16", "zmm17", "zmm18", "zmm19", "zmm20", "zmm21", "zmm22", "zmm23",
    "zmm24", "zmm25", "zmm26", "zmm27", "zmm28", "zmm29", "zmm30", "zmm31"
};

static const char* k_names[] = {
    "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7"
};

static const char* st_names[] = {
    "st(0)", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)"
};

static const char* mm_names[] = {
    "mm0", "mm1", "mm2", "mm3", "mm4", "mm5", "mm6", "mm7"
};

const char* x86_64_reg_name(int reg, int size) {
    if (reg >= X86_64_XMM0 && reg <= X86_64_XMM15) {
        return xmm_names[reg - X86_64_XMM0];
    }
    if (reg >= X86_64_YMM0 && reg <= X86_64_YMM15) {
        return ymm_names[reg - X86_64_YMM0];
    }
    if (reg >= X86_64_ZMM0 && reg <= X86_64_ZMM31) {
        return zmm_names[reg - X86_64_ZMM0];
    }
    if (reg >= X86_64_K0 && reg <= X86_64_K7) {
        return k_names[reg - X86_64_K0];
    }
    if (reg >= X86_64_ST0 && reg <= X86_64_ST7) {
        return st_names[reg - X86_64_ST0];
    }
    if (reg >= X86_64_MM0 && reg <= X86_64_MM7) {
        return mm_names[reg - X86_64_MM0];
    }
    if (reg == X86_64_MXCSR) return "mxcsr";
    if (reg < 0 || reg > 15) return "???";
    
    switch (size) {
        case 64: return reg_names_64[reg];
        case 32: return reg_names_32[reg];
        case 16: return reg_names_16[reg];
        case 8:  return reg_names_8[reg];
        default: return reg_names_64[reg];
    }
}

const char* x86_64_reg_name_64(int reg) { return x86_64_reg_name(reg, 64); }
const char* x86_64_reg_name_32(int reg) { return x86_64_reg_name(reg, 32); }
const char* x86_64_reg_name_16(int reg) { return x86_64_reg_name(reg, 16); }
const char* x86_64_reg_name_8(int reg) { return x86_64_reg_name(reg, 8); }
