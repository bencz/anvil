#include "runtime_helpers.h"

const anvil_runtime_helpers_t anvil_libgcc_helpers = {
    .mul64 = "__muldi3",
    .sdiv64 = "__divdi3",
    .udiv64 = "__udivdi3",
    .smod64 = "__moddi3",
    .umod64 = "__umoddi3",
    .shl64 = "__ashldi3",
    .lshr64 = "__lshrdi3",
    .ashr64 = "__ashrdi3",
    .int_to_fp = {{{"__floatsisf", "__floatsidf"}, {"__floatdisf", "__floatdidf"}}, {{"__floatunsisf", "__floatunsidf"}, {"__floatundisf", "__floatundidf"}}},
    .fp_to_int = {{{"__fixsfsi", "__fixdfsi"}, {"__fixsfdi", "__fixdfdi"}}, {{"__fixunssfsi", "__fixunsdfsi"}, {"__fixunssfdi", "__fixunsdfdi"}}},
};
