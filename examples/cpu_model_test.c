/*
 * ANVIL - CPU Model Test
 * 
 * Demonstrates the CPU model system for target-specific code generation.
 * Each architecture has specific CPU models with different instruction sets.
 */

#include "anvil/anvil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Print CPU features for PowerPC */
static void print_ppc_features(anvil_cpu_features_t features)
{
    printf("  PowerPC Features:\n");
    if (features & ANVIL_FEATURE_PPC_ALTIVEC)    printf("    - AltiVec/VMX SIMD\n");
    if (features & ANVIL_FEATURE_PPC_VSX)        printf("    - VSX (Vector-Scalar)\n");
    if (features & ANVIL_FEATURE_PPC_DFP)        printf("    - Decimal Floating Point\n");
    if (features & ANVIL_FEATURE_PPC_POPCNTD)    printf("    - popcntd instruction\n");
    if (features & ANVIL_FEATURE_PPC_CMPB)       printf("    - cmpb instruction\n");
    if (features & ANVIL_FEATURE_PPC_FPRND)      printf("    - FP round instructions\n");
    if (features & ANVIL_FEATURE_PPC_MFTB)       printf("    - mftb instruction\n");
    if (features & ANVIL_FEATURE_PPC_ISEL)       printf("    - isel instruction\n");
    if (features & ANVIL_FEATURE_PPC_LDBRX)      printf("    - ldbrx/stdbrx instructions\n");
    if (features & ANVIL_FEATURE_PPC_FCPSGN)     printf("    - fcpsgn instruction\n");
    if (features & ANVIL_FEATURE_PPC_HTM)        printf("    - Hardware Transactional Memory\n");
    if (features & ANVIL_FEATURE_PPC_POWER8_VEC) printf("    - POWER8 vector extensions\n");
    if (features & ANVIL_FEATURE_PPC_POWER9_VEC) printf("    - POWER9 vector extensions\n");
    if (features & ANVIL_FEATURE_PPC_MMA)        printf("    - Matrix-Multiply Assist (POWER10)\n");
    if (features & ANVIL_FEATURE_PPC_PCREL)      printf("    - PC-relative addressing (POWER10)\n");
}

/* Print CPU features for z/Architecture */
static void print_zarch_features(anvil_cpu_features_t features)
{
    printf("  z/Architecture Features:\n");
    if (features & ANVIL_FEATURE_ZARCH_DFP)         printf("    - Decimal Floating Point\n");
    if (features & ANVIL_FEATURE_ZARCH_EIMM)        printf("    - Extended Immediate\n");
    if (features & ANVIL_FEATURE_ZARCH_GIE)         printf("    - General Instructions Extension\n");
    if (features & ANVIL_FEATURE_ZARCH_HIGHWORD)    printf("    - High-word facility\n");
    if (features & ANVIL_FEATURE_ZARCH_INTERLOCKED) printf("    - Interlocked access\n");
    if (features & ANVIL_FEATURE_ZARCH_LOADSTORE)   printf("    - Load/Store on Condition\n");
    if (features & ANVIL_FEATURE_ZARCH_MISCEXT)     printf("    - Miscellaneous Extensions\n");
    if (features & ANVIL_FEATURE_ZARCH_MISCEXT2)    printf("    - Miscellaneous Extensions 2\n");
    if (features & ANVIL_FEATURE_ZARCH_MISCEXT3)    printf("    - Miscellaneous Extensions 3\n");
    if (features & ANVIL_FEATURE_ZARCH_POPCOUNT)    printf("    - Population count\n");
    if (features & ANVIL_FEATURE_ZARCH_VECTOR)      printf("    - Vector facility\n");
    if (features & ANVIL_FEATURE_ZARCH_VECTOR_ENH1) printf("    - Vector enhancements 1\n");
    if (features & ANVIL_FEATURE_ZARCH_VECTOR_ENH2) printf("    - Vector enhancements 2\n");
    if (features & ANVIL_FEATURE_ZARCH_NNPA)        printf("    - Neural Network Processing Assist\n");
}

/* Print CPU features for ARM64 */
static void print_arm64_features(anvil_cpu_features_t features)
{
    printf("  ARM64 Features:\n");
    if (features & ANVIL_FEATURE_ARM64_NEON)    printf("    - NEON SIMD\n");
    if (features & ANVIL_FEATURE_ARM64_FP16)    printf("    - Half-precision FP\n");
    if (features & ANVIL_FEATURE_ARM64_DOTPROD) printf("    - Dot product instructions\n");
    if (features & ANVIL_FEATURE_ARM64_ATOMICS) printf("    - LSE atomics\n");
    if (features & ANVIL_FEATURE_ARM64_CRC32)   printf("    - CRC32 instructions\n");
    if (features & ANVIL_FEATURE_ARM64_SHA1)    printf("    - SHA-1 crypto\n");
    if (features & ANVIL_FEATURE_ARM64_SHA256)  printf("    - SHA-256 crypto\n");
    if (features & ANVIL_FEATURE_ARM64_AES)     printf("    - AES crypto\n");
    if (features & ANVIL_FEATURE_ARM64_SVE)     printf("    - Scalable Vector Extension\n");
    if (features & ANVIL_FEATURE_ARM64_SVE2)    printf("    - SVE2\n");
    if (features & ANVIL_FEATURE_ARM64_BF16)    printf("    - BFloat16\n");
    if (features & ANVIL_FEATURE_ARM64_I8MM)    printf("    - Int8 matrix multiply\n");
    if (features & ANVIL_FEATURE_ARM64_RCPC)    printf("    - RCPC\n");
    if (features & ANVIL_FEATURE_ARM64_JSCVT)   printf("    - JavaScript conversion\n");
    if (features & ANVIL_FEATURE_ARM64_FCMA)    printf("    - Complex number multiply-add\n");
}

/* Print CPU features for x86/x86-64 */
static void print_x86_features(anvil_cpu_features_t features)
{
    printf("  x86/x86-64 Features:\n");
    if (features & ANVIL_FEATURE_X86_MMX)     printf("    - MMX\n");
    if (features & ANVIL_FEATURE_X86_SSE)     printf("    - SSE\n");
    if (features & ANVIL_FEATURE_X86_SSE2)    printf("    - SSE2\n");
    if (features & ANVIL_FEATURE_X86_SSE3)    printf("    - SSE3\n");
    if (features & ANVIL_FEATURE_X86_SSSE3)   printf("    - SSSE3\n");
    if (features & ANVIL_FEATURE_X86_SSE41)   printf("    - SSE4.1\n");
    if (features & ANVIL_FEATURE_X86_SSE42)   printf("    - SSE4.2\n");
    if (features & ANVIL_FEATURE_X86_AVX)     printf("    - AVX\n");
    if (features & ANVIL_FEATURE_X86_AVX2)    printf("    - AVX2\n");
    if (features & ANVIL_FEATURE_X86_AVX512F) printf("    - AVX-512 Foundation\n");
    if (features & ANVIL_FEATURE_X86_FMA)     printf("    - FMA3\n");
    if (features & ANVIL_FEATURE_X86_BMI1)    printf("    - Bit Manipulation 1\n");
    if (features & ANVIL_FEATURE_X86_BMI2)    printf("    - Bit Manipulation 2\n");
    if (features & ANVIL_FEATURE_X86_POPCNT)  printf("    - Population count\n");
    if (features & ANVIL_FEATURE_X86_LZCNT)   printf("    - Leading zero count\n");
    if (features & ANVIL_FEATURE_X86_MOVBE)   printf("    - MOVBE instruction\n");
}

/* Test PowerPC 64-bit CPU models */
static void test_ppc64_models(void)
{
    printf("\n=== PowerPC 64-bit CPU Models ===\n\n");
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_PPC64);
    
    anvil_cpu_model_t ppc64_models[] = {
        ANVIL_CPU_PPC64_970,
        ANVIL_CPU_PPC64_POWER5,
        ANVIL_CPU_PPC64_POWER6,
        ANVIL_CPU_PPC64_POWER7,
        ANVIL_CPU_PPC64_POWER8,
        ANVIL_CPU_PPC64_POWER9,
        ANVIL_CPU_PPC64_POWER10
    };
    
    for (size_t i = 0; i < sizeof(ppc64_models) / sizeof(ppc64_models[0]); i++) {
        anvil_ctx_set_cpu(ctx, ppc64_models[i]);
        
        printf("CPU: %s\n", anvil_cpu_model_name(ppc64_models[i]));
        print_ppc_features(anvil_ctx_get_cpu_features(ctx));
        printf("\n");
    }
    
    anvil_ctx_destroy(ctx);
}

/* Test z/Architecture CPU models */
static void test_zarch_models(void)
{
    printf("\n=== z/Architecture CPU Models ===\n\n");
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_ZARCH);
    
    anvil_cpu_model_t zarch_models[] = {
        ANVIL_CPU_ZARCH_Z900,
        ANVIL_CPU_ZARCH_Z9,
        ANVIL_CPU_ZARCH_Z10,
        ANVIL_CPU_ZARCH_Z196,
        ANVIL_CPU_ZARCH_ZEC12,
        ANVIL_CPU_ZARCH_Z13,
        ANVIL_CPU_ZARCH_Z14,
        ANVIL_CPU_ZARCH_Z15,
        ANVIL_CPU_ZARCH_Z16
    };
    
    for (size_t i = 0; i < sizeof(zarch_models) / sizeof(zarch_models[0]); i++) {
        anvil_ctx_set_cpu(ctx, zarch_models[i]);
        
        printf("CPU: %s\n", anvil_cpu_model_name(zarch_models[i]));
        print_zarch_features(anvil_ctx_get_cpu_features(ctx));
        printf("\n");
    }
    
    anvil_ctx_destroy(ctx);
}

/* Test ARM64 CPU models */
static void test_arm64_models(void)
{
    printf("\n=== ARM64 CPU Models ===\n\n");
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_ARM64);
    
    anvil_cpu_model_t arm64_models[] = {
        ANVIL_CPU_ARM64_GENERIC,
        ANVIL_CPU_ARM64_CORTEX_A53,
        ANVIL_CPU_ARM64_CORTEX_A72,
        ANVIL_CPU_ARM64_CORTEX_A76,
        ANVIL_CPU_ARM64_NEOVERSE_N1,
        ANVIL_CPU_ARM64_NEOVERSE_V1,
        ANVIL_CPU_ARM64_APPLE_M1,
        ANVIL_CPU_ARM64_APPLE_M2,
        ANVIL_CPU_ARM64_APPLE_M3
    };
    
    for (size_t i = 0; i < sizeof(arm64_models) / sizeof(arm64_models[0]); i++) {
        anvil_ctx_set_cpu(ctx, arm64_models[i]);
        
        printf("CPU: %s\n", anvil_cpu_model_name(arm64_models[i]));
        print_arm64_features(anvil_ctx_get_cpu_features(ctx));
        printf("\n");
    }
    
    anvil_ctx_destroy(ctx);
}

/* Test x86-64 CPU models */
static void test_x86_64_models(void)
{
    printf("\n=== x86-64 CPU Models ===\n\n");
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_X86_64);
    
    anvil_cpu_model_t x86_64_models[] = {
        ANVIL_CPU_X86_64_GENERIC,
        ANVIL_CPU_X86_64_CORE2,
        ANVIL_CPU_X86_64_NEHALEM,
        ANVIL_CPU_X86_64_SANDYBRIDGE,
        ANVIL_CPU_X86_64_HASWELL,
        ANVIL_CPU_X86_64_SKYLAKE,
        ANVIL_CPU_X86_64_ICELAKE,
        ANVIL_CPU_X86_64_ZEN,
        ANVIL_CPU_X86_64_ZEN3,
        ANVIL_CPU_X86_64_ZEN4
    };
    
    for (size_t i = 0; i < sizeof(x86_64_models) / sizeof(x86_64_models[0]); i++) {
        anvil_ctx_set_cpu(ctx, x86_64_models[i]);
        
        printf("CPU: %s\n", anvil_cpu_model_name(x86_64_models[i]));
        print_x86_features(anvil_ctx_get_cpu_features(ctx));
        printf("\n");
    }
    
    anvil_ctx_destroy(ctx);
}

/* Test feature override functionality */
static void test_feature_override(void)
{
    printf("\n=== Feature Override Test ===\n\n");
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_PPC64);
    anvil_ctx_set_cpu(ctx, ANVIL_CPU_PPC64_POWER7);
    
    printf("POWER7 default features:\n");
    print_ppc_features(anvil_ctx_get_cpu_features(ctx));
    
    /* Disable VSX */
    printf("\nAfter disabling VSX:\n");
    anvil_ctx_disable_feature(ctx, ANVIL_FEATURE_PPC_VSX);
    print_ppc_features(anvil_ctx_get_cpu_features(ctx));
    
    /* Check specific feature */
    printf("\nHas AltiVec: %s\n", anvil_ctx_has_feature(ctx, ANVIL_FEATURE_PPC_ALTIVEC) ? "yes" : "no");
    printf("Has VSX: %s\n", anvil_ctx_has_feature(ctx, ANVIL_FEATURE_PPC_VSX) ? "yes" : "no");
    printf("Has HTM: %s\n", anvil_ctx_has_feature(ctx, ANVIL_FEATURE_PPC_HTM) ? "yes" : "no");
    
    /* Enable HTM (not available on POWER7, but we can force it) */
    printf("\nAfter enabling HTM (forced):\n");
    anvil_ctx_enable_feature(ctx, ANVIL_FEATURE_PPC_HTM);
    print_ppc_features(anvil_ctx_get_cpu_features(ctx));
    
    anvil_ctx_destroy(ctx);
}

/* Test code generation with CPU-specific features */
static void test_codegen_with_cpu(void)
{
    printf("\n=== Code Generation with CPU Model ===\n\n");
    
    anvil_ctx_t *ctx = anvil_ctx_create();
    anvil_ctx_set_target(ctx, ANVIL_ARCH_PPC64);
    anvil_ctx_set_cpu(ctx, ANVIL_CPU_PPC64_POWER9);
    
    printf("Generating code for: %s\n", anvil_cpu_model_name(anvil_ctx_get_cpu(ctx)));
    printf("Available features:\n");
    print_ppc_features(anvil_ctx_get_cpu_features(ctx));
    
    /* Create a simple function */
    anvil_module_t *mod = anvil_module_create(ctx, "test");
    
    anvil_type_t *i64_type = anvil_type_i64(ctx);
    anvil_type_t *params[] = { i64_type, i64_type };
    anvil_type_t *func_type = anvil_type_func(ctx, i64_type, params, 2, false);
    
    anvil_func_t *func = anvil_func_create(mod, "add_values", func_type, ANVIL_LINK_EXTERNAL);
    anvil_block_t *entry = anvil_func_get_entry(func);
    anvil_set_insert_point(ctx, entry);
    
    anvil_value_t *a = anvil_func_get_param(func, 0);
    anvil_value_t *b = anvil_func_get_param(func, 1);
    anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
    anvil_build_ret(ctx, sum);
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_module_codegen(mod, &output, &len);
    
    printf("\nGenerated assembly:\n%s\n", output);
    
    free(output);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    printf("ANVIL CPU Model System Demo\n");
    printf("===========================\n");
    
    test_ppc64_models();
    test_zarch_models();
    test_arm64_models();
    test_x86_64_models();
    test_feature_override();
    test_codegen_with_cpu();
    
    printf("\n=== All tests completed ===\n");
    return 0;
}
