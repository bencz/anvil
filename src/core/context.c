/*
 * ANVIL - Context implementation
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ============================================================================
 * CPU Model Information Tables
 * ============================================================================ */

/* CPU model name table */
typedef struct {
    anvil_cpu_model_t model;
    const char *name;
    anvil_arch_t arch;
    anvil_cpu_features_t features;
} cpu_model_info_t;

static const cpu_model_info_t cpu_model_table[] = {
    /* Generic */
    { ANVIL_CPU_GENERIC, "generic", ANVIL_ARCH_COUNT, 0 },
    
    /* PowerPC 32-bit */
    { ANVIL_CPU_PPC_G3, "g3", ANVIL_ARCH_PPC32, 0 },
    { ANVIL_CPU_PPC_G4, "g4", ANVIL_ARCH_PPC32, ANVIL_FEATURE_PPC_ALTIVEC },
    { ANVIL_CPU_PPC_G4E, "g4e", ANVIL_ARCH_PPC32, ANVIL_FEATURE_PPC_ALTIVEC },
    
    /* PowerPC 64-bit */
    { ANVIL_CPU_PPC64_970, "970", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_ALTIVEC | ANVIL_FEATURE_PPC_MFTB },
    { ANVIL_CPU_PPC64_970FX, "970fx", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_ALTIVEC | ANVIL_FEATURE_PPC_MFTB },
    { ANVIL_CPU_PPC64_970MP, "970mp", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_ALTIVEC | ANVIL_FEATURE_PPC_MFTB },
    { ANVIL_CPU_PPC64_POWER4, "power4", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_MFTB },
    { ANVIL_CPU_PPC64_POWER4P, "power4+", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_MFTB },
    { ANVIL_CPU_PPC64_POWER5, "power5", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_MFTB | ANVIL_FEATURE_PPC_POPCNTD },
    { ANVIL_CPU_PPC64_POWER5P, "power5+", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_MFTB | ANVIL_FEATURE_PPC_POPCNTD | ANVIL_FEATURE_PPC_FPRND },
    { ANVIL_CPU_PPC64_POWER6, "power6", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_ALTIVEC | ANVIL_FEATURE_PPC_MFTB | ANVIL_FEATURE_PPC_POPCNTD | 
        ANVIL_FEATURE_PPC_CMPB | ANVIL_FEATURE_PPC_FPRND | ANVIL_FEATURE_PPC_DFP },
    { ANVIL_CPU_PPC64_POWER7, "power7", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_ALTIVEC | ANVIL_FEATURE_PPC_VSX | ANVIL_FEATURE_PPC_MFTB | 
        ANVIL_FEATURE_PPC_POPCNTD | ANVIL_FEATURE_PPC_CMPB | ANVIL_FEATURE_PPC_FPRND | 
        ANVIL_FEATURE_PPC_DFP | ANVIL_FEATURE_PPC_ISEL | ANVIL_FEATURE_PPC_LDBRX |
        ANVIL_FEATURE_PPC_FCPSGN },
    { ANVIL_CPU_PPC64_POWER8, "power8", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_ALTIVEC | ANVIL_FEATURE_PPC_VSX | ANVIL_FEATURE_PPC_MFTB | 
        ANVIL_FEATURE_PPC_POPCNTD | ANVIL_FEATURE_PPC_CMPB | ANVIL_FEATURE_PPC_FPRND | 
        ANVIL_FEATURE_PPC_DFP | ANVIL_FEATURE_PPC_ISEL | ANVIL_FEATURE_PPC_LDBRX |
        ANVIL_FEATURE_PPC_FCPSGN | ANVIL_FEATURE_PPC_HTM | ANVIL_FEATURE_PPC_POWER8_VEC },
    { ANVIL_CPU_PPC64_POWER9, "power9", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_ALTIVEC | ANVIL_FEATURE_PPC_VSX | ANVIL_FEATURE_PPC_MFTB | 
        ANVIL_FEATURE_PPC_POPCNTD | ANVIL_FEATURE_PPC_CMPB | ANVIL_FEATURE_PPC_FPRND | 
        ANVIL_FEATURE_PPC_DFP | ANVIL_FEATURE_PPC_ISEL | ANVIL_FEATURE_PPC_LDBRX |
        ANVIL_FEATURE_PPC_FCPSGN | ANVIL_FEATURE_PPC_POWER8_VEC | ANVIL_FEATURE_PPC_POWER9_VEC },
    { ANVIL_CPU_PPC64_POWER10, "power10", ANVIL_ARCH_PPC64, 
        ANVIL_FEATURE_PPC_ALTIVEC | ANVIL_FEATURE_PPC_VSX | ANVIL_FEATURE_PPC_MFTB | 
        ANVIL_FEATURE_PPC_POPCNTD | ANVIL_FEATURE_PPC_CMPB | ANVIL_FEATURE_PPC_FPRND | 
        ANVIL_FEATURE_PPC_DFP | ANVIL_FEATURE_PPC_ISEL | ANVIL_FEATURE_PPC_LDBRX |
        ANVIL_FEATURE_PPC_FCPSGN | ANVIL_FEATURE_PPC_POWER8_VEC | ANVIL_FEATURE_PPC_POWER9_VEC |
        ANVIL_FEATURE_PPC_MMA | ANVIL_FEATURE_PPC_PCREL },
    
    /* IBM Mainframe */
    { ANVIL_CPU_S370_BASE, "s370", ANVIL_ARCH_S370, 0 },
    { ANVIL_CPU_S370_XA, "s370-xa", ANVIL_ARCH_S370_XA, 0 },
    { ANVIL_CPU_S390_G1, "g1", ANVIL_ARCH_S390, 0 },
    { ANVIL_CPU_S390_G2, "g2", ANVIL_ARCH_S390, 0 },
    { ANVIL_CPU_S390_G3, "g3", ANVIL_ARCH_S390, 0 },
    { ANVIL_CPU_S390_G4, "g4", ANVIL_ARCH_S390, 0 },
    { ANVIL_CPU_S390_G5, "g5", ANVIL_ARCH_S390, ANVIL_FEATURE_ZARCH_HFP_EXT },
    { ANVIL_CPU_S390_G6, "g6", ANVIL_ARCH_S390, ANVIL_FEATURE_ZARCH_HFP_EXT },
    { ANVIL_CPU_ZARCH_Z900, "z900", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM },
    { ANVIL_CPU_ZARCH_Z990, "z990", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM },
    { ANVIL_CPU_ZARCH_Z9, "z9", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM | ANVIL_FEATURE_ZARCH_GIE | ANVIL_FEATURE_ZARCH_DFP },
    { ANVIL_CPU_ZARCH_Z10, "z10", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM | ANVIL_FEATURE_ZARCH_GIE | ANVIL_FEATURE_ZARCH_DFP },
    { ANVIL_CPU_ZARCH_Z196, "z196", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM | ANVIL_FEATURE_ZARCH_GIE | ANVIL_FEATURE_ZARCH_DFP |
        ANVIL_FEATURE_ZARCH_HIGHWORD | ANVIL_FEATURE_ZARCH_INTERLOCKED |
        ANVIL_FEATURE_ZARCH_LOADSTORE | ANVIL_FEATURE_ZARCH_POPCOUNT },
    { ANVIL_CPU_ZARCH_ZEC12, "zec12", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM | ANVIL_FEATURE_ZARCH_GIE | ANVIL_FEATURE_ZARCH_DFP |
        ANVIL_FEATURE_ZARCH_HIGHWORD | ANVIL_FEATURE_ZARCH_INTERLOCKED |
        ANVIL_FEATURE_ZARCH_LOADSTORE | ANVIL_FEATURE_ZARCH_POPCOUNT |
        ANVIL_FEATURE_ZARCH_MISCEXT },
    { ANVIL_CPU_ZARCH_Z13, "z13", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM | ANVIL_FEATURE_ZARCH_GIE | ANVIL_FEATURE_ZARCH_DFP |
        ANVIL_FEATURE_ZARCH_HIGHWORD | ANVIL_FEATURE_ZARCH_INTERLOCKED |
        ANVIL_FEATURE_ZARCH_LOADSTORE | ANVIL_FEATURE_ZARCH_POPCOUNT |
        ANVIL_FEATURE_ZARCH_MISCEXT | ANVIL_FEATURE_ZARCH_VECTOR },
    { ANVIL_CPU_ZARCH_Z14, "z14", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM | ANVIL_FEATURE_ZARCH_GIE | ANVIL_FEATURE_ZARCH_DFP |
        ANVIL_FEATURE_ZARCH_HIGHWORD | ANVIL_FEATURE_ZARCH_INTERLOCKED |
        ANVIL_FEATURE_ZARCH_LOADSTORE | ANVIL_FEATURE_ZARCH_POPCOUNT |
        ANVIL_FEATURE_ZARCH_MISCEXT | ANVIL_FEATURE_ZARCH_MISCEXT2 |
        ANVIL_FEATURE_ZARCH_VECTOR | ANVIL_FEATURE_ZARCH_VECTOR_ENH1 },
    { ANVIL_CPU_ZARCH_Z15, "z15", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM | ANVIL_FEATURE_ZARCH_GIE | ANVIL_FEATURE_ZARCH_DFP |
        ANVIL_FEATURE_ZARCH_HIGHWORD | ANVIL_FEATURE_ZARCH_INTERLOCKED |
        ANVIL_FEATURE_ZARCH_LOADSTORE | ANVIL_FEATURE_ZARCH_POPCOUNT |
        ANVIL_FEATURE_ZARCH_MISCEXT | ANVIL_FEATURE_ZARCH_MISCEXT2 | ANVIL_FEATURE_ZARCH_MISCEXT3 |
        ANVIL_FEATURE_ZARCH_VECTOR | ANVIL_FEATURE_ZARCH_VECTOR_ENH1 | ANVIL_FEATURE_ZARCH_VECTOR_ENH2 },
    { ANVIL_CPU_ZARCH_Z16, "z16", ANVIL_ARCH_ZARCH, 
        ANVIL_FEATURE_ZARCH_EIMM | ANVIL_FEATURE_ZARCH_GIE | ANVIL_FEATURE_ZARCH_DFP |
        ANVIL_FEATURE_ZARCH_HIGHWORD | ANVIL_FEATURE_ZARCH_INTERLOCKED |
        ANVIL_FEATURE_ZARCH_LOADSTORE | ANVIL_FEATURE_ZARCH_POPCOUNT |
        ANVIL_FEATURE_ZARCH_MISCEXT | ANVIL_FEATURE_ZARCH_MISCEXT2 | ANVIL_FEATURE_ZARCH_MISCEXT3 |
        ANVIL_FEATURE_ZARCH_VECTOR | ANVIL_FEATURE_ZARCH_VECTOR_ENH1 | ANVIL_FEATURE_ZARCH_VECTOR_ENH2 |
        ANVIL_FEATURE_ZARCH_NNPA },
    
    /* ARM64 */
    { ANVIL_CPU_ARM64_GENERIC, "generic", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON },
    { ANVIL_CPU_ARM64_CORTEX_A53, "cortex-a53", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 },
    { ANVIL_CPU_ARM64_CORTEX_A55, "cortex-a55", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC },
    { ANVIL_CPU_ARM64_CORTEX_A57, "cortex-a57", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 },
    { ANVIL_CPU_ARM64_CORTEX_A72, "cortex-a72", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 },
    { ANVIL_CPU_ARM64_CORTEX_A73, "cortex-a73", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 },
    { ANVIL_CPU_ARM64_CORTEX_A75, "cortex-a75", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC },
    { ANVIL_CPU_ARM64_CORTEX_A76, "cortex-a76", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_JSCVT | ANVIL_FEATURE_ARM64_FCMA },
    { ANVIL_CPU_ARM64_CORTEX_A77, "cortex-a77", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_JSCVT | ANVIL_FEATURE_ARM64_FCMA },
    { ANVIL_CPU_ARM64_CORTEX_A78, "cortex-a78", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_JSCVT | ANVIL_FEATURE_ARM64_FCMA },
    { ANVIL_CPU_ARM64_CORTEX_X1, "cortex-x1", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_JSCVT | ANVIL_FEATURE_ARM64_FCMA },
    { ANVIL_CPU_ARM64_CORTEX_X2, "cortex-x2", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_JSCVT | ANVIL_FEATURE_ARM64_FCMA | ANVIL_FEATURE_ARM64_SVE2 |
        ANVIL_FEATURE_ARM64_BF16 | ANVIL_FEATURE_ARM64_I8MM },
    { ANVIL_CPU_ARM64_NEOVERSE_N1, "neoverse-n1", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC },
    { ANVIL_CPU_ARM64_NEOVERSE_V1, "neoverse-v1", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_SVE | ANVIL_FEATURE_ARM64_BF16 | ANVIL_FEATURE_ARM64_I8MM },
    { ANVIL_CPU_ARM64_APPLE_M1, "apple-m1", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_JSCVT | ANVIL_FEATURE_ARM64_FCMA },
    { ANVIL_CPU_ARM64_APPLE_M2, "apple-m2", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_JSCVT | ANVIL_FEATURE_ARM64_FCMA | ANVIL_FEATURE_ARM64_BF16 },
    { ANVIL_CPU_ARM64_APPLE_M3, "apple-m3", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_JSCVT | ANVIL_FEATURE_ARM64_FCMA | ANVIL_FEATURE_ARM64_BF16 |
        ANVIL_FEATURE_ARM64_I8MM },
    { ANVIL_CPU_ARM64_APPLE_M4, "apple-m4", ANVIL_ARCH_ARM64, 
        ANVIL_FEATURE_ARM64_NEON | ANVIL_FEATURE_ARM64_CRC32 | ANVIL_FEATURE_ARM64_AES |
        ANVIL_FEATURE_ARM64_SHA1 | ANVIL_FEATURE_ARM64_SHA256 | ANVIL_FEATURE_ARM64_ATOMICS |
        ANVIL_FEATURE_ARM64_DOTPROD | ANVIL_FEATURE_ARM64_FP16 | ANVIL_FEATURE_ARM64_RCPC |
        ANVIL_FEATURE_ARM64_JSCVT | ANVIL_FEATURE_ARM64_FCMA | ANVIL_FEATURE_ARM64_BF16 |
        ANVIL_FEATURE_ARM64_I8MM | ANVIL_FEATURE_ARM64_SME },
    
    /* x86 32-bit */
    { ANVIL_CPU_X86_I386, "i386", ANVIL_ARCH_X86, 0 },
    { ANVIL_CPU_X86_I486, "i486", ANVIL_ARCH_X86, 0 },
    { ANVIL_CPU_X86_PENTIUM, "pentium", ANVIL_ARCH_X86, 0 },
    { ANVIL_CPU_X86_PENTIUM_MMX, "pentium-mmx", ANVIL_ARCH_X86, ANVIL_FEATURE_X86_MMX },
    { ANVIL_CPU_X86_PENTIUM_PRO, "pentium-pro", ANVIL_ARCH_X86, 0 },
    { ANVIL_CPU_X86_PENTIUM2, "pentium2", ANVIL_ARCH_X86, ANVIL_FEATURE_X86_MMX },
    { ANVIL_CPU_X86_PENTIUM3, "pentium3", ANVIL_ARCH_X86, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE },
    { ANVIL_CPU_X86_PENTIUM4, "pentium4", ANVIL_ARCH_X86, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 },
    { ANVIL_CPU_X86_K6, "k6", ANVIL_ARCH_X86, ANVIL_FEATURE_X86_MMX },
    { ANVIL_CPU_X86_ATHLON, "athlon", ANVIL_ARCH_X86, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE },
    
    /* x86-64 */
    { ANVIL_CPU_X86_64_GENERIC, "x86-64", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 },
    { ANVIL_CPU_X86_64_NOCONA, "nocona", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 },
    { ANVIL_CPU_X86_64_CORE2, "core2", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 },
    { ANVIL_CPU_X86_64_NEHALEM, "nehalem", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT },
    { ANVIL_CPU_X86_64_WESTMERE, "westmere", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT },
    { ANVIL_CPU_X86_64_SANDYBRIDGE, "sandybridge", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX },
    { ANVIL_CPU_X86_64_IVYBRIDGE, "ivybridge", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX },
    { ANVIL_CPU_X86_64_HASWELL, "haswell", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_AVX2 | ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_BMI1 |
        ANVIL_FEATURE_X86_BMI2 | ANVIL_FEATURE_X86_LZCNT | ANVIL_FEATURE_X86_MOVBE },
    { ANVIL_CPU_X86_64_BROADWELL, "broadwell", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_AVX2 | ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_BMI1 |
        ANVIL_FEATURE_X86_BMI2 | ANVIL_FEATURE_X86_LZCNT | ANVIL_FEATURE_X86_MOVBE },
    { ANVIL_CPU_X86_64_SKYLAKE, "skylake", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_AVX2 | ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_BMI1 |
        ANVIL_FEATURE_X86_BMI2 | ANVIL_FEATURE_X86_LZCNT | ANVIL_FEATURE_X86_MOVBE },
    { ANVIL_CPU_X86_64_ICELAKE, "icelake", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_AVX2 | ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_BMI1 |
        ANVIL_FEATURE_X86_BMI2 | ANVIL_FEATURE_X86_LZCNT | ANVIL_FEATURE_X86_MOVBE |
        ANVIL_FEATURE_X86_AVX512F },
    { ANVIL_CPU_X86_64_ALDERLAKE, "alderlake", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_AVX2 | ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_BMI1 |
        ANVIL_FEATURE_X86_BMI2 | ANVIL_FEATURE_X86_LZCNT | ANVIL_FEATURE_X86_MOVBE },
    { ANVIL_CPU_X86_64_K8, "k8", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 },
    { ANVIL_CPU_X86_64_K10, "k10", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSE41 | ANVIL_FEATURE_X86_SSE42 |
        ANVIL_FEATURE_X86_POPCNT },
    { ANVIL_CPU_X86_64_BULLDOZER, "bulldozer", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_FMA },
    { ANVIL_CPU_X86_64_ZEN, "zen", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_AVX2 | ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_BMI1 |
        ANVIL_FEATURE_X86_BMI2 | ANVIL_FEATURE_X86_LZCNT | ANVIL_FEATURE_X86_MOVBE },
    { ANVIL_CPU_X86_64_ZEN2, "zen2", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_AVX2 | ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_BMI1 |
        ANVIL_FEATURE_X86_BMI2 | ANVIL_FEATURE_X86_LZCNT | ANVIL_FEATURE_X86_MOVBE },
    { ANVIL_CPU_X86_64_ZEN3, "zen3", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_AVX2 | ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_BMI1 |
        ANVIL_FEATURE_X86_BMI2 | ANVIL_FEATURE_X86_LZCNT | ANVIL_FEATURE_X86_MOVBE },
    { ANVIL_CPU_X86_64_ZEN4, "zen4", ANVIL_ARCH_X86_64, 
        ANVIL_FEATURE_X86_MMX | ANVIL_FEATURE_X86_SSE | ANVIL_FEATURE_X86_SSE2 |
        ANVIL_FEATURE_X86_SSE3 | ANVIL_FEATURE_X86_SSSE3 | ANVIL_FEATURE_X86_SSE41 |
        ANVIL_FEATURE_X86_SSE42 | ANVIL_FEATURE_X86_POPCNT | ANVIL_FEATURE_X86_AVX |
        ANVIL_FEATURE_X86_AVX2 | ANVIL_FEATURE_X86_FMA | ANVIL_FEATURE_X86_BMI1 |
        ANVIL_FEATURE_X86_BMI2 | ANVIL_FEATURE_X86_LZCNT | ANVIL_FEATURE_X86_MOVBE |
        ANVIL_FEATURE_X86_AVX512F },
    
    /* Sentinel */
    { 0, NULL, 0, 0 }
};

/* Find CPU model info */
static const cpu_model_info_t *find_cpu_model_info(anvil_cpu_model_t model)
{
    for (size_t i = 0; cpu_model_table[i].name != NULL; i++) {
        if (cpu_model_table[i].model == model) {
            return &cpu_model_table[i];
        }
    }
    return NULL;
}

/* Update effective CPU features based on model and overrides */
static void update_cpu_features(anvil_ctx_t *ctx)
{
    anvil_cpu_features_t base_features = 0;
    
    const cpu_model_info_t *info = find_cpu_model_info(ctx->cpu_model);
    if (info) {
        base_features = info->features;
    }
    
    /* Apply overrides: enable first, then disable */
    ctx->cpu_features = (base_features | ctx->features_enabled) & ~ctx->features_disabled;
}

/* Architecture information table */
static const anvil_arch_info_t arch_info_table[ANVIL_ARCH_COUNT] = {
    [ANVIL_ARCH_X86] = {
        .arch = ANVIL_ARCH_X86,
        .name = "x86",
        .ptr_size = 4,
        .addr_bits = 32,
        .word_size = 4,
        .num_gpr = 8,
        .num_fpr = 8,
        .endian = ANVIL_ENDIAN_LITTLE,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_X86_64] = {
        .arch = ANVIL_ARCH_X86_64,
        .name = "x86-64",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 16,
        .num_fpr = 16,
        .endian = ANVIL_ENDIAN_LITTLE,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_S370] = {
        .arch = ANVIL_ARCH_S370,
        .name = "S/370",
        .ptr_size = 4,
        .addr_bits = 24,
        .word_size = 4,
        .num_gpr = 16,
        .num_fpr = 4,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_UP,
        .fp_format = ANVIL_FP_HFP,           /* IBM Hexadecimal FP only */
        .abi = ANVIL_ABI_MVS,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_S370_XA] = {
        .arch = ANVIL_ARCH_S370_XA,
        .name = "S/370-XA",
        .ptr_size = 4,
        .addr_bits = 31,
        .word_size = 4,
        .num_gpr = 16,
        .num_fpr = 4,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_UP,
        .fp_format = ANVIL_FP_HFP,           /* IBM Hexadecimal FP only */
        .abi = ANVIL_ABI_MVS,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_S390] = {
        .arch = ANVIL_ARCH_S390,
        .name = "S/390",
        .ptr_size = 4,
        .addr_bits = 31,
        .word_size = 4,
        .num_gpr = 16,
        .num_fpr = 16,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_UP,
        .fp_format = ANVIL_FP_HFP,           /* HFP default, some models have IEEE */
        .abi = ANVIL_ABI_MVS,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_ZARCH] = {
        .arch = ANVIL_ARCH_ZARCH,
        .name = "z/Architecture",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 16,
        .num_fpr = 16,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_UP,
        .fp_format = ANVIL_FP_HFP_IEEE,      /* Both HFP and IEEE 754 supported */
        .abi = ANVIL_ABI_MVS,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_PPC32] = {
        .arch = ANVIL_ARCH_PPC32,
        .name = "PowerPC 32-bit",
        .ptr_size = 4,
        .addr_bits = 32,
        .word_size = 4,
        .num_gpr = 32,
        .num_fpr = 32,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_PPC64] = {
        .arch = ANVIL_ARCH_PPC64,
        .name = "PowerPC 64-bit",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 32,
        .num_fpr = 32,
        .endian = ANVIL_ENDIAN_BIG,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_PPC64LE] = {
        .arch = ANVIL_ARCH_PPC64LE,
        .name = "PowerPC 64-bit LE",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 32,
        .num_fpr = 32,
        .endian = ANVIL_ENDIAN_LITTLE,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,
        .has_condition_codes = true,
        .has_delay_slots = false
    },
    [ANVIL_ARCH_ARM64] = {
        .arch = ANVIL_ARCH_ARM64,
        .name = "ARM64",
        .ptr_size = 8,
        .addr_bits = 64,
        .word_size = 8,
        .num_gpr = 31,
        .num_fpr = 32,
        .endian = ANVIL_ENDIAN_LITTLE,
        .stack_dir = ANVIL_STACK_DOWN,
        .fp_format = ANVIL_FP_IEEE754,
        .abi = ANVIL_ABI_SYSV,               /* Default to Linux, can be changed to DARWIN */
        .has_condition_codes = true,
        .has_delay_slots = false
    }
};

anvil_ctx_t *anvil_ctx_create(void)
{
    anvil_ctx_t *ctx = calloc(1, sizeof(anvil_ctx_t));
    if (!ctx) return NULL;
    
    /* Default to x86-64 */
    ctx->arch = ANVIL_ARCH_X86_64;
    ctx->output = ANVIL_OUTPUT_ASM;
    ctx->syntax = ANVIL_SYNTAX_DEFAULT;
    
    /* Initialize type cache */
    anvil_type_init_sizes(ctx);
    
    /* Initialize backends */
    anvil_init_backends();
    
    return ctx;
}

void anvil_ctx_destroy(anvil_ctx_t *ctx)
{
    if (!ctx) return;
    
    /* Reset backend state FIRST (while IR values are still valid)
     * This clears any cached pointers to anvil_value_t in stack_slots/strings */
    if (ctx->backend) {
        if (ctx->backend->ops && ctx->backend->ops->reset) {
            ctx->backend->ops->reset(ctx->backend);
        }
    }
    
    /* Destroy all modules (frees all IR values) */
    anvil_module_t *mod = ctx->modules;
    while (mod) {
        anvil_module_t *next = mod->next;
        anvil_module_destroy(mod);
        mod = next;
    }
    
    /* Destroy memory pool */
    if (ctx->pool) {
        anvil_pool_destroy(ctx->pool);
    }
    
    /* Cleanup backend (now safe - no dangling pointers) */
    if (ctx->backend) {
        if (ctx->backend->ops && ctx->backend->ops->cleanup) {
            ctx->backend->ops->cleanup(ctx->backend);
        }
        free(ctx->backend);
    }
    
    /* Cleanup pass manager */
    if (ctx->pass_manager) {
        anvil_pass_manager_destroy(ctx->pass_manager);
    }
    
    free(ctx);
}

anvil_error_t anvil_ctx_set_target(anvil_ctx_t *ctx, anvil_arch_t arch)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    if (arch >= ANVIL_ARCH_COUNT) return ANVIL_ERR_INVALID_ARG;
    
    ctx->arch = arch;
    
    /* Set default FP format based on architecture */
    ctx->fp_format = arch_info_table[arch].fp_format;
    
    /* Set default ABI based on architecture */
    ctx->abi = arch_info_table[arch].abi;
    
    /* Set default CPU model for this architecture */
    ctx->cpu_model = anvil_arch_default_cpu(arch);
    ctx->features_enabled = 0;
    ctx->features_disabled = 0;
    update_cpu_features(ctx);
    
    /* Update type sizes for new architecture */
    anvil_type_init_sizes(ctx);
    
    /* Get backend for this architecture */
    ctx->backend = anvil_get_backend(ctx, arch);
    if (!ctx->backend) {
        anvil_set_error(ctx, ANVIL_ERR_NO_BACKEND, 
                        "No backend available for architecture %s",
                        arch_info_table[arch].name);
        return ANVIL_ERR_NO_BACKEND;
    }
    
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_set_output(anvil_ctx_t *ctx, anvil_output_t output)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    ctx->output = output;
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_set_syntax(anvil_ctx_t *ctx, anvil_syntax_t syntax)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    ctx->syntax = syntax;
    if (ctx->backend) {
        ctx->backend->syntax = syntax;
    }
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_set_abi(anvil_ctx_t *ctx, anvil_abi_t abi)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    ctx->abi = abi;
    return ANVIL_OK;
}

anvil_abi_t anvil_ctx_get_abi(anvil_ctx_t *ctx)
{
    if (!ctx) return ANVIL_ABI_DEFAULT;
    return ctx->abi;
}

anvil_error_t anvil_ctx_set_fp_format(anvil_ctx_t *ctx, anvil_fp_format_t fp_format)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    
    /* Validate FP format for the current architecture */
    const anvil_arch_info_t *arch_info = &arch_info_table[ctx->arch];
    
    switch (ctx->arch) {
        case ANVIL_ARCH_S370:
        case ANVIL_ARCH_S370_XA:
            /* S/370 and S/370-XA only support HFP */
            if (fp_format != ANVIL_FP_HFP) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                    "Architecture %s only supports HFP floating-point format",
                    arch_info->name);
                return ANVIL_ERR_INVALID_ARG;
            }
            break;
            
        case ANVIL_ARCH_S390:
            /* S/390 supports HFP (default) and IEEE on some models */
            if (fp_format != ANVIL_FP_HFP && fp_format != ANVIL_FP_IEEE754) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                    "Architecture %s supports HFP or IEEE754 floating-point format",
                    arch_info->name);
                return ANVIL_ERR_INVALID_ARG;
            }
            break;
            
        case ANVIL_ARCH_ZARCH:
            /* z/Architecture supports both HFP and IEEE */
            if (fp_format != ANVIL_FP_HFP && fp_format != ANVIL_FP_IEEE754 && 
                fp_format != ANVIL_FP_HFP_IEEE) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                    "Architecture %s supports HFP, IEEE754, or HFP_IEEE floating-point format",
                    arch_info->name);
                return ANVIL_ERR_INVALID_ARG;
            }
            break;
            
        default:
            /* Other architectures only support IEEE 754 */
            if (fp_format != ANVIL_FP_IEEE754) {
                anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
                    "Architecture %s only supports IEEE754 floating-point format",
                    arch_info->name);
                return ANVIL_ERR_INVALID_ARG;
            }
            break;
    }
    
    ctx->fp_format = fp_format;
    return ANVIL_OK;
}

anvil_fp_format_t anvil_ctx_get_fp_format(anvil_ctx_t *ctx)
{
    if (!ctx) return ANVIL_FP_IEEE754;
    return ctx->fp_format;
}

const anvil_arch_info_t *anvil_ctx_get_arch_info(anvil_ctx_t *ctx)
{
    if (!ctx) return NULL;
    return &arch_info_table[ctx->arch];
}

const anvil_arch_info_t *anvil_arch_get_info(anvil_arch_t arch)
{
    if (arch >= ANVIL_ARCH_COUNT) return NULL;
    return &arch_info_table[arch];
}

const char *anvil_ctx_get_error(anvil_ctx_t *ctx)
{
    if (!ctx) return "Invalid context";
    return ctx->error_msg;
}

void anvil_set_error(anvil_ctx_t *ctx, anvil_error_t err, const char *fmt, ...)
{
    if (!ctx) return;
    
    ctx->last_error = err;
    
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->error_msg, sizeof(ctx->error_msg), fmt, args);
    va_end(args);
}

void anvil_set_insert_point(anvil_ctx_t *ctx, anvil_block_t *block)
{
    if (!ctx) return;
    ctx->insert_block = block;
    ctx->insert_point = block ? block->last : NULL;
}

/* ============================================================================
 * CPU Model API Implementation
 * ============================================================================ */

anvil_error_t anvil_ctx_set_cpu(anvil_ctx_t *ctx, anvil_cpu_model_t cpu)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    
    /* Validate CPU model exists */
    const cpu_model_info_t *info = find_cpu_model_info(cpu);
    if (!info && cpu != ANVIL_CPU_GENERIC) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
            "Unknown CPU model: %d", cpu);
        return ANVIL_ERR_INVALID_ARG;
    }
    
    /* Validate CPU model is compatible with current architecture */
    if (info && info->arch != ANVIL_ARCH_COUNT && info->arch != ctx->arch) {
        anvil_set_error(ctx, ANVIL_ERR_INVALID_ARG,
            "CPU model '%s' is not compatible with current architecture",
            info->name);
        return ANVIL_ERR_INVALID_ARG;
    }
    
    ctx->cpu_model = cpu;
    update_cpu_features(ctx);
    
    return ANVIL_OK;
}

anvil_cpu_model_t anvil_ctx_get_cpu(anvil_ctx_t *ctx)
{
    if (!ctx) return ANVIL_CPU_GENERIC;
    return ctx->cpu_model;
}

anvil_cpu_features_t anvil_ctx_get_cpu_features(anvil_ctx_t *ctx)
{
    if (!ctx) return 0;
    return ctx->cpu_features;
}

bool anvil_ctx_has_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature)
{
    if (!ctx) return false;
    return (ctx->cpu_features & feature) == feature;
}

anvil_error_t anvil_ctx_enable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    
    ctx->features_enabled |= feature;
    ctx->features_disabled &= ~feature;  /* Remove from disabled if present */
    update_cpu_features(ctx);
    
    return ANVIL_OK;
}

anvil_error_t anvil_ctx_disable_feature(anvil_ctx_t *ctx, anvil_cpu_features_t feature)
{
    if (!ctx) return ANVIL_ERR_INVALID_ARG;
    
    ctx->features_disabled |= feature;
    ctx->features_enabled &= ~feature;  /* Remove from enabled if present */
    update_cpu_features(ctx);
    
    return ANVIL_OK;
}

const char *anvil_cpu_model_name(anvil_cpu_model_t cpu)
{
    const cpu_model_info_t *info = find_cpu_model_info(cpu);
    if (info) {
        return info->name;
    }
    return "unknown";
}

anvil_cpu_model_t anvil_arch_default_cpu(anvil_arch_t arch)
{
    switch (arch) {
        case ANVIL_ARCH_X86:
            return ANVIL_CPU_X86_I386;
        case ANVIL_ARCH_X86_64:
            return ANVIL_CPU_X86_64_GENERIC;
        case ANVIL_ARCH_S370:
            return ANVIL_CPU_S370_BASE;
        case ANVIL_ARCH_S370_XA:
            return ANVIL_CPU_S370_XA;
        case ANVIL_ARCH_S390:
            return ANVIL_CPU_S390_G5;
        case ANVIL_ARCH_ZARCH:
            return ANVIL_CPU_ZARCH_Z900;
        case ANVIL_ARCH_PPC32:
            return ANVIL_CPU_PPC_G3;
        case ANVIL_ARCH_PPC64:
            return ANVIL_CPU_PPC64_POWER4;
        case ANVIL_ARCH_PPC64LE:
            return ANVIL_CPU_PPC64_POWER8;
        case ANVIL_ARCH_ARM64:
            return ANVIL_CPU_ARM64_GENERIC;
        default:
            return ANVIL_CPU_GENERIC;
    }
}

anvil_cpu_features_t anvil_cpu_model_features(anvil_cpu_model_t cpu)
{
    const cpu_model_info_t *info = find_cpu_model_info(cpu);
    if (info) {
        return info->features;
    }
    return 0;
}
