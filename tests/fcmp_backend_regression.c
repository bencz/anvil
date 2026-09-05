#include <anvil/anvil.h>
#include <anvil/anvil_arm64_mir.h>
#include <anvil/anvil_mainframe_mir.h>
#include <anvil/anvil_machine.h>
#include <anvil/anvil_ppc_mir.h>
#include <anvil/anvil_x86_64_mir.h>
#include <anvil/anvil_x86_mir.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/host.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "[FAIL] %s\n", (m)); failures++; } } while (0)

static void cross_assemble(const char *assembly, const char *target, const char *message)
{
    CHECK(anvil_test_host.cross_assemble(assembly, target) <= 0, message);
}

static bool run_target_pipeline(anvil_arch_t arch, anvil_func_t *fn,
                                anvil_mir_func_t **out_mir,
                                char **out_text, size_t *out_len)
{
    anvil_mir_func_t *mir = NULL;
    bool allocated = false;
    if (arch == ANVIL_ARCH_X86) {
        mir = anvil_x86_lower_func_to_mir(fn);
        allocated = mir && anvil_x86_regalloc_mir(mir);
        if (allocated) allocated = anvil_x86_emit_mir(mir, out_text, out_len);
    } else if (arch == ANVIL_ARCH_X86_64) {
        mir = anvil_x86_64_lower_func_to_mir(fn);
        allocated = mir && anvil_x86_64_regalloc_mir(mir);
        if (allocated) allocated = anvil_x86_64_emit_mir(mir, out_text, out_len);
    } else if (arch == ANVIL_ARCH_ARM64) {
        mir = anvil_arm64_lower_func_to_mir(fn);
        allocated = mir && anvil_arm64_regalloc_mir(mir);
        if (allocated) allocated = anvil_arm64_emit_mir(mir, out_text, out_len);
    } else if (arch == ANVIL_ARCH_PPC32) {
        mir = anvil_ppc_lower_func_to_mir(fn, ANVIL_PPC_VARIANT_PPC32);
        allocated = mir && anvil_ppc_regalloc_mir(mir, ANVIL_PPC_VARIANT_PPC32);
        if (allocated) allocated = anvil_ppc_emit_mir(
            mir, ANVIL_PPC_VARIANT_PPC32, out_text, out_len);
    } else {
        anvil_mainframe_variant_t variant = arch == ANVIL_ARCH_S370
            ? ANVIL_MAINFRAME_VARIANT_S370 : ANVIL_MAINFRAME_VARIANT_ZARCH;
        mir = anvil_mainframe_lower_func_to_mir(fn, variant);
        allocated = mir && anvil_mainframe_regalloc_mir(mir, variant);
        if (allocated) allocated = anvil_mainframe_emit_mir(
            mir, variant, out_text, out_len);
    }
    *out_mir = mir;
    return allocated;
}

static void check_target(anvil_arch_t arch, anvil_fcmp_pred_t pred)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx && anvil_ctx_set_target(ctx, arch) == ANVIL_OK,
          "FCMP target context should initialize");
    if (!ctx || !anvil_ctx_has_target(ctx)) return;
    if (arch == ANVIL_ARCH_ZARCH)
        CHECK(anvil_ctx_set_fp_format(ctx, ANVIL_FP_IEEE754) == ANVIL_OK,
              "zArch FCMP should select IEEE BFP");
    anvil_module_t *mod = anvil_module_create(ctx, "fcmp_all");
    anvil_type_t *f64 = anvil_type_f64(ctx);
    anvil_type_t *params[] = { f64, f64 };
    char name[32];
    snprintf(name, sizeof(name), "fcmp_%d_%d", (int)arch, (int)pred);
    anvil_func_t *fn = anvil_func_create(mod, name,
        anvil_type_func(ctx, anvil_type_i1(ctx), params, 2, false),
        ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *value = anvil_build_fcmp(ctx, pred,
        anvil_func_get_param(fn, 0), anvil_func_get_param(fn, 1), "cmp");
    CHECK(value && anvil_build_ret(ctx, value), "FCMP IR should build and return i1");

    anvil_mir_func_t *mir = NULL;
    bool allocated = false, emitted = false;
    char *text = NULL;
    size_t len = 0;
    if (arch == ANVIL_ARCH_X86) {
        mir = anvil_x86_lower_func_to_mir(fn);
        if (mir) {
            char error[256];
            if (!anvil_x86_verify_mir_legal(mir, error, sizeof(error)))
                fprintf(stderr, "x86 legal: %s\n", error);
        }
        allocated = mir && anvil_x86_regalloc_mir(mir);
        emitted = allocated && anvil_x86_emit_mir(mir, &text, &len);
    } else if (arch == ANVIL_ARCH_X86_64) {
        mir = anvil_x86_64_lower_func_to_mir(fn);
        allocated = mir && anvil_x86_64_regalloc_mir(mir);
        emitted = allocated && anvil_x86_64_emit_mir(mir, &text, &len);
    } else if (arch == ANVIL_ARCH_ARM64) {
        mir = anvil_arm64_lower_func_to_mir(fn);
        allocated = mir && anvil_arm64_regalloc_mir(mir);
        emitted = allocated && anvil_arm64_emit_mir(mir, &text, &len);
    } else if (arch == ANVIL_ARCH_PPC32) {
        mir = anvil_ppc_lower_func_to_mir(fn, ANVIL_PPC_VARIANT_PPC32);
        allocated = mir && anvil_ppc_regalloc_mir(mir, ANVIL_PPC_VARIANT_PPC32);
        emitted = allocated && anvil_ppc_emit_mir(
            mir, ANVIL_PPC_VARIANT_PPC32, &text, &len);
    } else {
        mir = anvil_mainframe_lower_func_to_mir(
            fn, ANVIL_MAINFRAME_VARIANT_ZARCH);
        allocated = mir && anvil_mainframe_regalloc_mir(
            mir, ANVIL_MAINFRAME_VARIANT_ZARCH);
        emitted = allocated && anvil_mainframe_emit_mir(
            mir, ANVIL_MAINFRAME_VARIANT_ZARCH, &text, &len);
    }
    if (!(mir && allocated && emitted && text && len)) {
        fprintf(stderr, "[FAIL] FCMP target=%d predicate=%d lower=%d alloc=%d emit=%d\n",
                (int)arch, (int)pred, mir != NULL, allocated, emitted);
        failures++;
    }
    bool retained = false;
    if (mir) for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        anvil_mir_get_instr_info(mir, i, &info);
        if (info.op == ANVIL_MIR_OP_FCMP) {
            const anvil_mir_vreg_info_t *def =
                anvil_mir_get_vreg_info(mir, info.def);
            retained = info.has_imm && info.imm == pred && def &&
                       def->reg_class == ANVIL_MIR_REG_GPR && def->size_bits == 8;
        }
    }
    CHECK(retained, "FCMP predicate and normalized GPR8 result must survive MIR");
    if (text && pred == ANVIL_FCMP_UNO) {
        if (arch == ANVIL_ARCH_X86 || arch == ANVIL_ARCH_X86_64)
            CHECK(strstr(text, "\tjp "), "x86 UNO must test parity/unordered");
        else if (arch == ANVIL_ARCH_ARM64)
            CHECK(strstr(text, "\tb.vs "), "ARM64 UNO must test V/unordered");
        else if (arch == ANVIL_ARCH_PPC32)
            CHECK(strstr(text, "\tbun "), "PPC UNO must test CR unordered");
        else
            CHECK(strstr(text, "BC    1,"), "zArch UNO must branch on CC3 only");
        if (arch == ANVIL_ARCH_ARM64)
            cross_assemble(text, "aarch64-linux-gnu",
                           "ARM64 FCMP unordered assembly should cross-assemble");
        else if (arch == ANVIL_ARCH_PPC32)
            cross_assemble(text, "powerpc-linux-gnu",
                           "PPC FCMP unordered assembly should cross-assemble");
    }
    if (text && arch == ANVIL_ARCH_ZARCH)
        CHECK(strstr(text, "CDBR"), "zArch IEEE FCMP must use BFP CDBR");
    free(text);
    anvil_mir_func_destroy(mir);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void check_i1_memory_target(anvil_arch_t arch)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx && anvil_ctx_set_target(ctx, arch) == ANVIL_OK,
          "i1 memory target context should initialize");
    if (!ctx || !anvil_ctx_has_target(ctx)) return;
    anvil_module_t *mod = anvil_module_create(ctx, "i1_memory");
    anvil_type_t *i1 = anvil_type_i1(ctx);
    anvil_type_t *params[] = { anvil_type_ptr(ctx, i1) };
    anvil_func_t *fn = anvil_func_create(mod, "load_i1",
        anvil_type_func(ctx, i1, params, 1, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *loaded = anvil_build_load(
        ctx, i1, anvil_func_get_param(fn, 0), "loaded");
    CHECK(loaded && anvil_build_ret(ctx, loaded), "i1 memory function should build");

    anvil_mir_func_t *mir = NULL;
    char *text = NULL;
    size_t len = 0;
    CHECK(run_target_pipeline(arch, fn, &mir, &text, &len),
          "i1 byte load must lower, allocate, and emit on every target");
    bool saw_load8 = false, saw_normalize = false;
    if (mir) for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) continue;
        const anvil_mir_vreg_info_t *def = info.def == ANVIL_MIR_NO_VREG
            ? NULL : anvil_mir_get_vreg_info(mir, info.def);
        if (info.op == ANVIL_MIR_OP_LOAD && def && def->size_bits == 8)
            saw_load8 = true;
        if (info.op == ANVIL_MIR_OP_AND && def && def->size_bits == 8)
            saw_normalize = true;
    }
    CHECK(saw_load8 && saw_normalize,
          "i1 memory load must read one byte and normalize its low bit");
    free(text);
    anvil_mir_func_destroy(mir);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void check_s370_hfp_compare(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx && anvil_ctx_set_target(ctx, ANVIL_ARCH_S370) == ANVIL_OK,
          "S370 HFP context should initialize");
    if (!ctx || !anvil_ctx_has_target(ctx)) return;
    anvil_module_t *mod = anvil_module_create(ctx, "hfp_fcmp");
    anvil_type_t *f32 = anvil_type_f32(ctx);
    anvil_type_t *params[] = { f32, f32 };
    anvil_func_t *fn = anvil_func_create(mod, "hfp_oeq",
        anvil_type_func(ctx, anvil_type_i1(ctx), params, 2, false),
        ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *cmp = anvil_build_fcmp(ctx, ANVIL_FCMP_OEQ,
        anvil_func_get_param(fn, 0), anvil_func_get_param(fn, 1), "cmp");
    CHECK(cmp && anvil_build_ret(ctx, cmp), "S370 ordered HFP compare should build");
    anvil_mir_func_t *mir = NULL;
    char *text = NULL;
    size_t len = 0;
    CHECK(run_target_pipeline(ANVIL_ARCH_S370, fn, &mir, &text, &len),
          "S370 ordered HFP compare should emit");
    CHECK(text && strstr(text, "CER"), "S370 f32 compare must use HFP CER");
    CHECK(!text || !strstr(text, "CEBR"), "S370 HFP compare must not use BFP CEBR");
    free(text);
    anvil_mir_func_destroy(mir);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    const anvil_arch_t targets[] = {
        ANVIL_ARCH_X86, ANVIL_ARCH_X86_64, ANVIL_ARCH_ARM64,
        ANVIL_ARCH_PPC32, ANVIL_ARCH_ZARCH
    };
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++)
        for (int p = ANVIL_FCMP_FALSE; p <= ANVIL_FCMP_TRUE; p++)
            check_target(targets[t], (anvil_fcmp_pred_t)p);
    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++)
        check_i1_memory_target(targets[t]);
    check_s370_hfp_compare();
    if (failures) return 1;
    puts("FCMP cross-backend regression tests passed");
    return 0;
}
