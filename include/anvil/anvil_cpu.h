/*
 * ANVIL - CPU Model System
 * 
 * This header defines CPU models, feature flags, and related types
 * for target-specific code generation and optimization.
 */

#ifndef ANVIL_CPU_H
#define ANVIL_CPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * CPU Model Enumeration
 * ============================================================================
 * Each architecture has specific CPU models with different instruction sets.
 * Setting a CPU model enables/disables specific features and optimizations.
 */

typedef enum {
    /* Generic/baseline for each architecture */
    ANVIL_CPU_GENERIC = 0,
    
    /* PowerPC 32-bit models (600-999 range) */
    ANVIL_CPU_PPC_G3 = 600,          /* PowerPC 750 (G3) */
    ANVIL_CPU_PPC_G4,                /* PowerPC 7400/7450 (G4) */
    ANVIL_CPU_PPC_G4E,               /* PowerPC 7450 (G4e) with AltiVec */
    
    /* PowerPC 64-bit models (1000-1499 range) */
    ANVIL_CPU_PPC64_970 = 1000,      /* PowerPC 970 (G5) */
    ANVIL_CPU_PPC64_970FX,           /* PowerPC 970FX */
    ANVIL_CPU_PPC64_970MP,           /* PowerPC 970MP */
    ANVIL_CPU_PPC64_POWER4,          /* IBM POWER4 */
    ANVIL_CPU_PPC64_POWER4P,         /* IBM POWER4+ */
    ANVIL_CPU_PPC64_POWER5,          /* IBM POWER5 */
    ANVIL_CPU_PPC64_POWER5P,         /* IBM POWER5+ */
    ANVIL_CPU_PPC64_POWER6,          /* IBM POWER6 */
    ANVIL_CPU_PPC64_POWER7,          /* IBM POWER7 */
    ANVIL_CPU_PPC64_POWER8,          /* IBM POWER8 */
    ANVIL_CPU_PPC64_POWER9,          /* IBM POWER9 */
    ANVIL_CPU_PPC64_POWER10,         /* IBM POWER10 */
    
    /* IBM Mainframe models (2000-2499 range) */
    ANVIL_CPU_S370_BASE = 2000,      /* S/370 base */
    ANVIL_CPU_S370_XA,               /* S/370-XA */
    ANVIL_CPU_S390_G1,               /* S/390 G1 (9672-R11) */
    ANVIL_CPU_S390_G2,               /* S/390 G2 (9672-R21) */
    ANVIL_CPU_S390_G3,               /* S/390 G3 (9672-R31) */
    ANVIL_CPU_S390_G4,               /* S/390 G4 (9672-R41) */
    ANVIL_CPU_S390_G5,               /* S/390 G5 (9672-R51) */
    ANVIL_CPU_S390_G6,               /* S/390 G6 (9672-R61) */
    ANVIL_CPU_ZARCH_Z900,            /* z900 (z/Architecture) */
    ANVIL_CPU_ZARCH_Z990,            /* z990 */
    ANVIL_CPU_ZARCH_Z9,              /* z9 EC/BC */
    ANVIL_CPU_ZARCH_Z10,             /* z10 EC/BC */
    ANVIL_CPU_ZARCH_Z196,            /* z196 */
    ANVIL_CPU_ZARCH_ZEC12,           /* zEC12 */
    ANVIL_CPU_ZARCH_Z13,             /* z13 */
    ANVIL_CPU_ZARCH_Z14,             /* z14 */
    ANVIL_CPU_ZARCH_Z15,             /* z15 */
    ANVIL_CPU_ZARCH_Z16,             /* z16 */
    
    /* ARM64 models (3000-3499 range) */
    ANVIL_CPU_ARM64_GENERIC = 3000,  /* Generic ARMv8-A */
    ANVIL_CPU_ARM64_CORTEX_A53,      /* Cortex-A53 */
    ANVIL_CPU_ARM64_CORTEX_A55,      /* Cortex-A55 */
    ANVIL_CPU_ARM64_CORTEX_A57,      /* Cortex-A57 */
    ANVIL_CPU_ARM64_CORTEX_A72,      /* Cortex-A72 */
    ANVIL_CPU_ARM64_CORTEX_A73,      /* Cortex-A73 */
    ANVIL_CPU_ARM64_CORTEX_A75,      /* Cortex-A75 */
    ANVIL_CPU_ARM64_CORTEX_A76,      /* Cortex-A76 */
    ANVIL_CPU_ARM64_CORTEX_A77,      /* Cortex-A77 */
    ANVIL_CPU_ARM64_CORTEX_A78,      /* Cortex-A78 */
    ANVIL_CPU_ARM64_CORTEX_X1,       /* Cortex-X1 */
    ANVIL_CPU_ARM64_CORTEX_X2,       /* Cortex-X2 */
    ANVIL_CPU_ARM64_NEOVERSE_N1,     /* Neoverse N1 */
    ANVIL_CPU_ARM64_NEOVERSE_V1,     /* Neoverse V1 */
    ANVIL_CPU_ARM64_APPLE_M1,        /* Apple M1 */
    ANVIL_CPU_ARM64_APPLE_M2,        /* Apple M2 */
    ANVIL_CPU_ARM64_APPLE_M3,        /* Apple M3 */
    ANVIL_CPU_ARM64_APPLE_M4,        /* Apple M4 */
    
    /* x86 models (4000-4499 range) */
    ANVIL_CPU_X86_I386 = 4000,       /* Intel 386 */
    ANVIL_CPU_X86_I486,              /* Intel 486 */
    ANVIL_CPU_X86_PENTIUM,           /* Intel Pentium */
    ANVIL_CPU_X86_PENTIUM_MMX,       /* Pentium MMX */
    ANVIL_CPU_X86_PENTIUM_PRO,       /* Pentium Pro */
    ANVIL_CPU_X86_PENTIUM2,          /* Pentium II */
    ANVIL_CPU_X86_PENTIUM3,          /* Pentium III */
    ANVIL_CPU_X86_PENTIUM4,          /* Pentium 4 */
    ANVIL_CPU_X86_K6,                /* AMD K6 */
    ANVIL_CPU_X86_ATHLON,            /* AMD Athlon */
    
    /* x86-64 models (4500-4999 range) */
    ANVIL_CPU_X86_64_GENERIC = 4500, /* Generic x86-64 */
    ANVIL_CPU_X86_64_NOCONA,         /* Intel Nocona */
    ANVIL_CPU_X86_64_CORE2,          /* Intel Core 2 */
    ANVIL_CPU_X86_64_NEHALEM,        /* Intel Nehalem */
    ANVIL_CPU_X86_64_WESTMERE,       /* Intel Westmere */
    ANVIL_CPU_X86_64_SANDYBRIDGE,    /* Intel Sandy Bridge */
    ANVIL_CPU_X86_64_IVYBRIDGE,      /* Intel Ivy Bridge */
    ANVIL_CPU_X86_64_HASWELL,        /* Intel Haswell */
    ANVIL_CPU_X86_64_BROADWELL,      /* Intel Broadwell */
    ANVIL_CPU_X86_64_SKYLAKE,        /* Intel Skylake */
    ANVIL_CPU_X86_64_ICELAKE,        /* Intel Ice Lake */
    ANVIL_CPU_X86_64_ALDERLAKE,      /* Intel Alder Lake */
    ANVIL_CPU_X86_64_K8,             /* AMD K8 (Opteron/Athlon64) */
    ANVIL_CPU_X86_64_K10,            /* AMD K10 (Barcelona) */
    ANVIL_CPU_X86_64_BULLDOZER,      /* AMD Bulldozer */
    ANVIL_CPU_X86_64_ZEN,            /* AMD Zen */
    ANVIL_CPU_X86_64_ZEN2,           /* AMD Zen 2 */
    ANVIL_CPU_X86_64_ZEN3,           /* AMD Zen 3 */
    ANVIL_CPU_X86_64_ZEN4,           /* AMD Zen 4 */
    
    ANVIL_CPU_COUNT
} anvil_cpu_model_t;

/* ============================================================================
 * CPU Feature Flags
 * ============================================================================
 * Bitfield for available CPU features. Each architecture uses different
 * bit ranges to avoid conflicts when combining features.
 */

typedef uint64_t anvil_cpu_features_t;

/* ----------------------------------------------------------------------------
 * PowerPC Features (bits 0-15)
 * ---------------------------------------------------------------------------- */
#define ANVIL_FEATURE_PPC_ALTIVEC       (1ULL << 0)   /* AltiVec/VMX SIMD */
#define ANVIL_FEATURE_PPC_VSX           (1ULL << 1)   /* VSX (Vector-Scalar) */
#define ANVIL_FEATURE_PPC_DFP           (1ULL << 2)   /* Decimal Floating Point */
#define ANVIL_FEATURE_PPC_POPCNTD       (1ULL << 3)   /* popcntd instruction */
#define ANVIL_FEATURE_PPC_CMPB          (1ULL << 4)   /* cmpb instruction */
#define ANVIL_FEATURE_PPC_FPRND         (1ULL << 5)   /* FP round instructions */
#define ANVIL_FEATURE_PPC_MFTB          (1ULL << 6)   /* mftb instruction */
#define ANVIL_FEATURE_PPC_ISEL          (1ULL << 7)   /* isel instruction */
#define ANVIL_FEATURE_PPC_LDBRX         (1ULL << 8)   /* ldbrx/stdbrx instructions */
#define ANVIL_FEATURE_PPC_FCPSGN        (1ULL << 9)   /* fcpsgn instruction */
#define ANVIL_FEATURE_PPC_HTM           (1ULL << 10)  /* Hardware Transactional Memory */
#define ANVIL_FEATURE_PPC_POWER8_VEC    (1ULL << 11)  /* POWER8 vector extensions */
#define ANVIL_FEATURE_PPC_POWER9_VEC    (1ULL << 12)  /* POWER9 vector extensions */
#define ANVIL_FEATURE_PPC_MMA           (1ULL << 13)  /* Matrix-Multiply Assist (POWER10) */
#define ANVIL_FEATURE_PPC_PCREL         (1ULL << 14)  /* PC-relative addressing (POWER10) */

/* ----------------------------------------------------------------------------
 * IBM Mainframe Features (bits 16-31)
 * ---------------------------------------------------------------------------- */
#define ANVIL_FEATURE_ZARCH_DFP         (1ULL << 16)  /* Decimal Floating Point */
#define ANVIL_FEATURE_ZARCH_EIMM        (1ULL << 17)  /* Extended Immediate */
#define ANVIL_FEATURE_ZARCH_GIE         (1ULL << 18)  /* General Instructions Extension */
#define ANVIL_FEATURE_ZARCH_HFP_EXT     (1ULL << 19)  /* HFP Extensions */
#define ANVIL_FEATURE_ZARCH_HIGHWORD    (1ULL << 20)  /* High-word facility */
#define ANVIL_FEATURE_ZARCH_INTERLOCKED (1ULL << 21)  /* Interlocked access */
#define ANVIL_FEATURE_ZARCH_LOADSTORE   (1ULL << 22)  /* Load/Store on Condition */
#define ANVIL_FEATURE_ZARCH_MISCEXT     (1ULL << 23)  /* Miscellaneous Extensions */
#define ANVIL_FEATURE_ZARCH_MISCEXT2    (1ULL << 24)  /* Miscellaneous Extensions 2 */
#define ANVIL_FEATURE_ZARCH_MISCEXT3    (1ULL << 25)  /* Miscellaneous Extensions 3 */
#define ANVIL_FEATURE_ZARCH_POPCOUNT    (1ULL << 26)  /* Population count */
#define ANVIL_FEATURE_ZARCH_VECTOR      (1ULL << 27)  /* Vector facility */
#define ANVIL_FEATURE_ZARCH_VECTOR_ENH1 (1ULL << 28)  /* Vector enhancements 1 */
#define ANVIL_FEATURE_ZARCH_VECTOR_ENH2 (1ULL << 29)  /* Vector enhancements 2 */
#define ANVIL_FEATURE_ZARCH_NNPA        (1ULL << 30)  /* Neural Network Processing Assist */

/* ----------------------------------------------------------------------------
 * ARM64 Features (bits 32-47)
 * ---------------------------------------------------------------------------- */
#define ANVIL_FEATURE_ARM64_NEON        (1ULL << 32)  /* NEON SIMD (always on ARMv8) */
#define ANVIL_FEATURE_ARM64_FP16        (1ULL << 33)  /* Half-precision FP */
#define ANVIL_FEATURE_ARM64_DOTPROD     (1ULL << 34)  /* Dot product instructions */
#define ANVIL_FEATURE_ARM64_ATOMICS     (1ULL << 35)  /* LSE atomics */
#define ANVIL_FEATURE_ARM64_CRC32       (1ULL << 36)  /* CRC32 instructions */
#define ANVIL_FEATURE_ARM64_SHA1        (1ULL << 37)  /* SHA-1 crypto */
#define ANVIL_FEATURE_ARM64_SHA256      (1ULL << 38)  /* SHA-256 crypto */
#define ANVIL_FEATURE_ARM64_AES         (1ULL << 39)  /* AES crypto */
#define ANVIL_FEATURE_ARM64_SVE         (1ULL << 40)  /* Scalable Vector Extension */
#define ANVIL_FEATURE_ARM64_SVE2        (1ULL << 41)  /* SVE2 */
#define ANVIL_FEATURE_ARM64_BF16        (1ULL << 42)  /* BFloat16 */
#define ANVIL_FEATURE_ARM64_I8MM        (1ULL << 43)  /* Int8 matrix multiply */
#define ANVIL_FEATURE_ARM64_RCPC        (1ULL << 44)  /* Release Consistent processor consistent */
#define ANVIL_FEATURE_ARM64_JSCVT       (1ULL << 45)  /* JavaScript conversion */
#define ANVIL_FEATURE_ARM64_FCMA        (1ULL << 46)  /* Complex number multiply-add */
#define ANVIL_FEATURE_ARM64_SME         (1ULL << 47)  /* Scalable Matrix Extension */

/* ----------------------------------------------------------------------------
 * x86/x86-64 Features (bits 48-63)
 * ---------------------------------------------------------------------------- */
#define ANVIL_FEATURE_X86_MMX           (1ULL << 48)  /* MMX */
#define ANVIL_FEATURE_X86_SSE           (1ULL << 49)  /* SSE */
#define ANVIL_FEATURE_X86_SSE2          (1ULL << 50)  /* SSE2 */
#define ANVIL_FEATURE_X86_SSE3          (1ULL << 51)  /* SSE3 */
#define ANVIL_FEATURE_X86_SSSE3         (1ULL << 52)  /* SSSE3 */
#define ANVIL_FEATURE_X86_SSE41         (1ULL << 53)  /* SSE4.1 */
#define ANVIL_FEATURE_X86_SSE42         (1ULL << 54)  /* SSE4.2 */
#define ANVIL_FEATURE_X86_AVX           (1ULL << 55)  /* AVX */
#define ANVIL_FEATURE_X86_AVX2          (1ULL << 56)  /* AVX2 */
#define ANVIL_FEATURE_X86_AVX512F       (1ULL << 57)  /* AVX-512 Foundation */
#define ANVIL_FEATURE_X86_FMA           (1ULL << 58)  /* FMA3 */
#define ANVIL_FEATURE_X86_BMI1          (1ULL << 59)  /* Bit Manipulation 1 */
#define ANVIL_FEATURE_X86_BMI2          (1ULL << 60)  /* Bit Manipulation 2 */
#define ANVIL_FEATURE_X86_POPCNT        (1ULL << 61)  /* Population count */
#define ANVIL_FEATURE_X86_LZCNT         (1ULL << 62)  /* Leading zero count */
#define ANVIL_FEATURE_X86_MOVBE         (1ULL << 63)  /* MOVBE instruction */

/* ============================================================================
 * Feature Helper Macros
 * ============================================================================ */

/* Check if a feature is present */
#define ANVIL_HAS_FEATURE(features, feature)  (((features) & (feature)) != 0)

/* Combine multiple features */
#define ANVIL_FEATURES(...)  (0ULL | __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* ANVIL_CPU_H */
