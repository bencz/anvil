#include "target.h"

const AnvilTargetInfo arm64_target_info = {
    .name = "arm64",
    
    .word_size = 64,
    .ptr_size = 64,
    .min_align = 1,
    .max_align = 16,
    
    .sizeof_bool = 1,
    .sizeof_char = 1,
    .sizeof_short = 2,
    .sizeof_int = 4,
    .sizeof_long = 8,
    .sizeof_longlong = 8,
    .sizeof_float = 4,
    .sizeof_double = 8,
    .sizeof_longdouble = 16,
    .sizeof_ptr = 8,
    
    .alignof_bool = 1,
    .alignof_char = 1,
    .alignof_short = 2,
    .alignof_int = 4,
    .alignof_long = 8,
    .alignof_longlong = 8,
    .alignof_float = 4,
    .alignof_double = 8,
    .alignof_ptr = 8,
    .alignof_stack = 16,
    
    .endianness = ANVIL_ENDIAN_LITTLE,
    .stack_direction = ANVIL_STACK_GROWS_DOWN,
    .float_format = ANVIL_FLOAT_IEEE754,
    
    .features = {
        .has_fpu = true,
        .has_simd = true,
        .has_atomic = true,
        .has_unaligned_access = true,
        .has_div = true,
        .has_mul = true,
        .has_barrel_shifter = true,
        .has_conditional_move = true,
        .has_branch_delay_slot = false,
    },
    
    .max_imm_bits = 12,
    .max_displacement = 4095,
};
