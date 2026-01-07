#include "regs.h"

const AnvilRegInfo arm64_regs[] = {
    { "x0",  ARM64_X0,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 0 },
    { "x1",  ARM64_X1,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 1 },
    { "x2",  ARM64_X2,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 2 },
    { "x3",  ARM64_X3,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 3 },
    { "x4",  ARM64_X4,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 4 },
    { "x5",  ARM64_X5,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 5 },
    { "x6",  ARM64_X6,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 6 },
    { "x7",  ARM64_X7,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 7 },
    { "x8",  ARM64_X8,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 8 },
    { "x9",  ARM64_X9,  64, ANVIL_REG_CLASS_GP, -1, 0, 64, 9 },
    { "x10", ARM64_X10, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 10 },
    { "x11", ARM64_X11, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 11 },
    { "x12", ARM64_X12, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 12 },
    { "x13", ARM64_X13, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 13 },
    { "x14", ARM64_X14, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 14 },
    { "x15", ARM64_X15, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 15 },
    { "x16", ARM64_X16, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 16 },
    { "x17", ARM64_X17, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 17 },
    { "x18", ARM64_X18, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 18 },
    { "x19", ARM64_X19, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 19 },
    { "x20", ARM64_X20, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 20 },
    { "x21", ARM64_X21, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 21 },
    { "x22", ARM64_X22, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 22 },
    { "x23", ARM64_X23, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 23 },
    { "x24", ARM64_X24, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 24 },
    { "x25", ARM64_X25, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 25 },
    { "x26", ARM64_X26, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 26 },
    { "x27", ARM64_X27, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 27 },
    { "x28", ARM64_X28, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 28 },
    { "x29", ARM64_X29, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 29 },
    { "x30", ARM64_X30, 64, ANVIL_REG_CLASS_GP, -1, 0, 64, 30 },
    { "sp",  ARM64_SP,  64, ANVIL_REG_CLASS_SPECIAL, -1, 0, 64, 31 },
    { "xzr", ARM64_XZR, 64, ANVIL_REG_CLASS_SPECIAL, -1, 0, 64, 31 },
    
    { "v0",  ARM64_V0,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 64 },
    { "v1",  ARM64_V1,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 65 },
    { "v2",  ARM64_V2,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 66 },
    { "v3",  ARM64_V3,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 67 },
    { "v4",  ARM64_V4,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 68 },
    { "v5",  ARM64_V5,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 69 },
    { "v6",  ARM64_V6,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 70 },
    { "v7",  ARM64_V7,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 71 },
    { "v8",  ARM64_V8,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 72 },
    { "v9",  ARM64_V9,  128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 73 },
    { "v10", ARM64_V10, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 74 },
    { "v11", ARM64_V11, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 75 },
    { "v12", ARM64_V12, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 76 },
    { "v13", ARM64_V13, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 77 },
    { "v14", ARM64_V14, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 78 },
    { "v15", ARM64_V15, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 79 },
    { "v16", ARM64_V16, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 80 },
    { "v17", ARM64_V17, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 81 },
    { "v18", ARM64_V18, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 82 },
    { "v19", ARM64_V19, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 83 },
    { "v20", ARM64_V20, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 84 },
    { "v21", ARM64_V21, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 85 },
    { "v22", ARM64_V22, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 86 },
    { "v23", ARM64_V23, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 87 },
    { "v24", ARM64_V24, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 88 },
    { "v25", ARM64_V25, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 89 },
    { "v26", ARM64_V26, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 90 },
    { "v27", ARM64_V27, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 91 },
    { "v28", ARM64_V28, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 92 },
    { "v29", ARM64_V29, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 93 },
    { "v30", ARM64_V30, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 94 },
    { "v31", ARM64_V31, 128, ANVIL_REG_CLASS_SIMD, -1, 0, 128, 95 },
    
    { "nzcv", ARM64_NZCV, 32, ANVIL_REG_CLASS_SPECIAL, -1, 0, 32, 0 },
    { "pc",   ARM64_PC,   64, ANVIL_REG_CLASS_SPECIAL, -1, 0, 64, 0 },
};

static const int arm64_gp_regs[] = {
    ARM64_X0, ARM64_X1, ARM64_X2, ARM64_X3, ARM64_X4, ARM64_X5, ARM64_X6, ARM64_X7,
    ARM64_X8, ARM64_X9, ARM64_X10, ARM64_X11, ARM64_X12, ARM64_X13, ARM64_X14, ARM64_X15,
    ARM64_X16, ARM64_X17, ARM64_X18, ARM64_X19, ARM64_X20, ARM64_X21, ARM64_X22, ARM64_X23,
    ARM64_X24, ARM64_X25, ARM64_X26, ARM64_X27, ARM64_X28, ARM64_X29, ARM64_X30
};

static const int arm64_fp_regs[] = {
    ARM64_V0, ARM64_V1, ARM64_V2, ARM64_V3, ARM64_V4, ARM64_V5, ARM64_V6, ARM64_V7,
    ARM64_V8, ARM64_V9, ARM64_V10, ARM64_V11, ARM64_V12, ARM64_V13, ARM64_V14, ARM64_V15,
    ARM64_V16, ARM64_V17, ARM64_V18, ARM64_V19, ARM64_V20, ARM64_V21, ARM64_V22, ARM64_V23,
    ARM64_V24, ARM64_V25, ARM64_V26, ARM64_V27, ARM64_V28, ARM64_V29, ARM64_V30, ARM64_V31
};

static const int arm64_scratch_regs[] = {
    ARM64_X0, ARM64_X1, ARM64_X2, ARM64_X3, ARM64_X4, ARM64_X5, ARM64_X6, ARM64_X7,
    ARM64_X8, ARM64_X9, ARM64_X10, ARM64_X11, ARM64_X12, ARM64_X13, ARM64_X14, ARM64_X15,
    ARM64_X16, ARM64_X17
};

const AnvilRegSet arm64_reg_set = {
    .name = "arm64_regs",
    .regs = arm64_regs,
    .num_regs = ARM64_NUM_REGS,
    
    .gp_regs = arm64_gp_regs,
    .num_gp_regs = sizeof(arm64_gp_regs) / sizeof(arm64_gp_regs[0]),
    .fp_regs = arm64_fp_regs,
    .num_fp_regs = sizeof(arm64_fp_regs) / sizeof(arm64_fp_regs[0]),
    .simd_regs = arm64_fp_regs,
    .num_simd_regs = sizeof(arm64_fp_regs) / sizeof(arm64_fp_regs[0]),
    
    .stack_pointer = ARM64_SP,
    .frame_pointer = ARM64_X29,
    .link_register = ARM64_X30,
    .program_counter = ARM64_PC,
    .flags_register = ARM64_NZCV,
    
    .scratch_regs = arm64_scratch_regs,
    .num_scratch_regs = sizeof(arm64_scratch_regs) / sizeof(arm64_scratch_regs[0]),
};

static const char* gp_names_64[] = {
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp", "xzr"
};

static const char* gp_names_32[] = {
    "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7",
    "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15",
    "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23",
    "w24", "w25", "w26", "w27", "w28", "w29", "w30", "wsp", "wzr"
};

static const char* fp_names[] = {
    "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
    "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
    "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
    "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"
};

const char* arm64_reg_name(int reg, int size) {
    if (reg >= ARM64_V0 && reg <= ARM64_V31) {
        return fp_names[reg - ARM64_V0];
    }
    if (reg < 0 || reg > ARM64_XZR) return "???";
    
    switch (size) {
        case 64: return gp_names_64[reg];
        case 32: return gp_names_32[reg];
        default: return gp_names_64[reg];
    }
}

const char* arm64_reg_name_64(int reg) { return arm64_reg_name(reg, 64); }
const char* arm64_reg_name_32(int reg) { return arm64_reg_name(reg, 32); }
