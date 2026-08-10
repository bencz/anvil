/* Cross-backend regression for the formal typed GEP walk. */
#include <anvil/anvil.h>
#include <anvil/anvil_machine.h>
#include <anvil/anvil_arm64_mir.h>
#include <anvil/anvil_x86_mir.h>
#include <anvil/anvil_x86_64_mir.h>
#include <anvil/anvil_ppc_mir.h>
#include <anvil/anvil_mainframe_mir.h>

#include <stdio.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "[FAIL] %s\n", (m)); failures++; } } while (0)

static anvil_func_t *build_nested_gep(anvil_ctx_t *ctx,
                                      anvil_module_t **out_mod)
{
    anvil_module_t *mod = anvil_module_create(ctx, "typed_gep_backend");
    if (!mod) return NULL;
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *array = anvil_type_array(ctx, i32, 4);
    anvil_type_t *fields[] = { i32, anvil_type_i8(ctx), array };
    anvil_type_t *record = anvil_type_literal_struct(ctx, fields, 3, false);
    anvil_type_t *params[] = { i32, i32, anvil_type_i8(ctx),
                               anvil_type_u16(ctx), anvil_type_i64(ctx) };
    anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 5, false);
    anvil_func_t *fn = anvil_func_create(mod, "nested_gep", fn_type,
                                         ANVIL_LINK_EXTERNAL);
    if (!fn) { anvil_module_destroy(mod); return NULL; }
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *base = anvil_build_alloca(ctx, record, "records");
    anvil_value_t *indices[] = {
        anvil_func_get_param(fn, 0), anvil_const_i32(ctx, 2),
        anvil_func_get_param(fn, 1)
    };
    anvil_value_t *element =
        anvil_build_gep(ctx, record, base, indices, 3, "element");
    anvil_build_store(ctx, anvil_const_i32(ctx, 99), element);
    anvil_value_t *signed_indices[] = {
        anvil_func_get_param(fn, 2), anvil_const_i32(ctx, 2),
        anvil_func_get_param(fn, 1)
    };
    anvil_value_t *unsigned_indices[] = {
        anvil_func_get_param(fn, 3), anvil_const_i32(ctx, 2),
        anvil_func_get_param(fn, 1)
    };
    anvil_build_store(ctx, anvil_const_i32(ctx, 11),
        anvil_build_gep(ctx, record, base, signed_indices, 3, "signed.index"));
    anvil_build_store(ctx, anvil_const_i32(ctx, 12),
        anvil_build_gep(ctx, record, base, unsigned_indices, 3, "unsigned.index"));
    anvil_value_t *wide_indices[] = {
        anvil_func_get_param(fn, 4), anvil_const_i32(ctx, 2),
        anvil_func_get_param(fn, 1)
    };
    anvil_build_store(ctx, anvil_const_i32(ctx, 14),
        anvil_build_gep(ctx, record, base, wide_indices, 3, "wide.index"));
    /* Exact constant offset: 1*sizeof(record=24) + field offset 8 +
     * 3*sizeof(i32) = 44. This must remain one checked folded displacement. */
    anvil_value_t *constant_indices[] = {
        anvil_const_i32(ctx, 1), anvil_const_i32(ctx, 2),
        anvil_const_i32(ctx, 3)
    };
    anvil_value_t *constant_element = anvil_build_gep(
        ctx, record, base, constant_indices, 3, "constant.element");
    anvil_build_store(ctx, anvil_const_i32(ctx, 77), constant_element);
    anvil_value_t *negative_indices[] = {
        anvil_const_i64(ctx, -1), anvil_const_i32(ctx, 2),
        anvil_const_i32(ctx, 0)
    };
    anvil_build_store(ctx, anvil_const_i32(ctx, 13),
        anvil_build_gep(ctx, record, base, negative_indices, 3,
                        "negative.constant"));
    anvil_build_ret(ctx, anvil_build_load(ctx, i32, element, "loaded"));
    *out_mod = mod;
    return fn;
}

static void check_mir_walk(anvil_mir_func_t *mir, const char *backend,
                           uint16_t pointer_bits)
{
    char message[160];
    snprintf(message, sizeof(message), "%s nested typed GEP should lower", backend);
    CHECK(mir != NULL, message);
    if (!mir) return;
    bool scale_source = false, scale_element = false;
    bool field_offset = false, exact_constant_offset = false;
    bool signed_normalized = false, unsigned_normalized = false;
    bool negative_constant = false;
    bool mul_widths_normalized = true;
    size_t muls = 0;
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(mir, i, &info)) continue;
        if (info.op == ANVIL_MIR_OP_MOV && info.has_imm && info.imm == 24)
            scale_source = true;
        if (info.op == ANVIL_MIR_OP_MOV && info.has_imm && info.imm == 4)
            scale_element = true;
        if (info.op == ANVIL_MIR_OP_MOV && info.has_imm && info.imm == 8)
            field_offset = true;
        if (info.has_imm && info.imm == 44)
            exact_constant_offset = true;
        if (info.op == ANVIL_MIR_OP_SEXT) signed_normalized = true;
        if (info.op == ANVIL_MIR_OP_ZEXT) unsigned_normalized = true;
        if (info.has_imm && info.imm == -16) negative_constant = true;
        if (info.op == ANVIL_MIR_OP_MUL) {
            muls++;
            const anvil_mir_vreg_info_t *def =
                anvil_mir_get_vreg_info(mir, info.def);
            if (!def || def->size_bits != pointer_bits)
                mul_widths_normalized = false;
            for (size_t u = 0; u < info.num_uses; u++) {
                anvil_mir_vreg_t use = anvil_mir_get_instr_use(mir, i, u);
                const anvil_mir_vreg_info_t *use_info =
                    anvil_mir_get_vreg_info(mir, use);
                if (!use_info || use_info->size_bits != pointer_bits)
                    mul_widths_normalized = false;
            }
        }
    }
    snprintf(message, sizeof(message),
             "%s GEP must scale the first index by sizeof(source)=24", backend);
    CHECK(scale_source, message);
    snprintf(message, sizeof(message),
             "%s GEP must scale nested array index by sizeof(i32)=4", backend);
    CHECK(scale_element, message);
    snprintf(message, sizeof(message),
             "%s GEP must apply the distinct struct field offset 8", backend);
    CHECK(field_offset, message);
    snprintf(message, sizeof(message),
             "%s GEP must fold the all-constant nested offset to exactly 44", backend);
    CHECK(exact_constant_offset, message);
    snprintf(message, sizeof(message),
             "%s GEP must sign-extend a dynamic i8 index to pointer width", backend);
    CHECK(signed_normalized, message);
    snprintf(message, sizeof(message),
             "%s GEP must zero-extend a dynamic u16 index to pointer width", backend);
    CHECK(unsigned_normalized, message);
    snprintf(message, sizeof(message),
             "%s GEP must preserve checked negative i64 constant offset -16", backend);
    CHECK(negative_constant, message);
    snprintf(message, sizeof(message),
             "%s GEP dynamic indices/scales must have pointer-width MIR types",
             backend);
    CHECK(mul_widths_normalized, message);
    snprintf(message, sizeof(message),
             "%s GEP must materialize both dynamic scale steps", backend);
    CHECK(muls >= 2, message);
    anvil_mir_func_destroy(mir);
}

static void run_arch(anvil_arch_t arch, const char *name)
{
    anvil_ctx_t *ctx = anvil_ctx_create_for_target(arch);
    CHECK(ctx != NULL, "typed GEP backend context should be created");
    if (!ctx) return;
    anvil_module_t *mod = NULL;
    anvil_func_t *fn = build_nested_gep(ctx, &mod);
    CHECK(fn != NULL, "typed GEP backend fixture should build");
    if (fn) {
        anvil_mir_func_t *mir = NULL;
        switch (arch) {
            case ANVIL_ARCH_ARM64:
                mir = anvil_arm64_lower_func_to_mir(fn); break;
            case ANVIL_ARCH_X86:
                mir = anvil_x86_lower_func_to_mir(fn); break;
            case ANVIL_ARCH_X86_64:
                mir = anvil_x86_64_lower_func_to_mir(fn); break;
            case ANVIL_ARCH_PPC32:
                mir = anvil_ppc_lower_func_to_mir(fn, ANVIL_PPC_VARIANT_PPC32); break;
            case ANVIL_ARCH_S370:
                mir = anvil_mainframe_lower_func_to_mir(
                    fn, ANVIL_MAINFRAME_VARIANT_S370); break;
            default: break;
        }
        if (!mir && anvil_ctx_get_error(ctx)[0])
            fprintf(stderr, "[%s diagnostic] %s\n", name,
                    anvil_ctx_get_error(ctx));
        const anvil_arch_info_t *info = anvil_ctx_get_arch_info(ctx);
        check_mir_walk(mir, name, (uint16_t)(info->ptr_size * 8));
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    run_arch(ANVIL_ARCH_ARM64, "ARM64");
    run_arch(ANVIL_ARCH_X86, "x86");
    run_arch(ANVIL_ARCH_X86_64, "x86-64");
    run_arch(ANVIL_ARCH_PPC32, "PPC32");
    run_arch(ANVIL_ARCH_S370, "mainframe");
    if (failures) {
        fprintf(stderr, "typed GEP backend regressions failed: %d\n", failures);
        return 1;
    }
    puts("typed GEP backend regressions passed");
    return 0;
}
