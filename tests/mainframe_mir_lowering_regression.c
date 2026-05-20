/*
 * Regression tests for the shared IBM mainframe -> MachineIR backend path.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_machine.h>
#include <anvil/anvil_mainframe_mir.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s\n", msg); \
        failures++; \
    } \
} while (0)

static void check_contains(const char *text, const char *needle, const char *msg)
{
    CHECK(text && needle && strstr(text, needle) != NULL, msg);
}

static bool hlasm_has_lowercase_outside_literals(const char *text)
{
    bool in_literal = false;
    if (!text) return false;

    for (const char *p = text; *p; p++) {
        if (*p == '\'') {
            if (in_literal && p[1] == '\'') {
                p++;
                continue;
            }
            in_literal = !in_literal;
            continue;
        }
        if (!in_literal && *p >= 'a' && *p <= 'z') return true;
    }
    return false;
}

static anvil_ctx_t *new_mainframe_ctx(anvil_arch_t arch)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx != NULL, "context should be created");
    if (!ctx) return NULL;

    CHECK(anvil_ctx_set_target(ctx, arch) == ANVIL_OK,
          "mainframe target should be available");
    return ctx;
}

static anvil_func_t *build_iadd_func(anvil_ctx_t *ctx, anvil_module_t **out_mod,
                                     const char *module_name,
                                     const char *func_name,
                                     anvil_type_t *int_type)
{
    anvil_module_t *mod = anvil_module_create(ctx, module_name);
    CHECK(mod != NULL, "module should be created");
    if (!mod) return NULL;

    anvil_type_t *params[] = { int_type, int_type };
    anvil_type_t *fn_type = anvil_type_func(ctx, int_type, params, 2, false);
    anvil_func_t *fn = anvil_func_create(mod, func_name, fn_type,
                                         ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "integer add function should be created");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *a = anvil_func_get_param(fn, 0);
        anvil_value_t *b = anvil_func_get_param(fn, 1);
        anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
        anvil_build_ret(ctx, sum);
    }

    *out_mod = mod;
    return fn;
}

static void test_mainframe_descriptors_model_real_variants(void)
{
    const anvil_mainframe_target_desc_t *s370 =
        anvil_mainframe_get_target_desc(ANVIL_MAINFRAME_VARIANT_S370);
    const anvil_mainframe_target_desc_t *s370_xa =
        anvil_mainframe_get_target_desc(ANVIL_MAINFRAME_VARIANT_S370_XA);
    const anvil_mainframe_target_desc_t *s390 =
        anvil_mainframe_get_target_desc(ANVIL_MAINFRAME_VARIANT_S390);
    const anvil_mainframe_target_desc_t *zarch =
        anvil_mainframe_get_target_desc(ANVIL_MAINFRAME_VARIANT_ZARCH);

    CHECK(s370 && s370_xa && s390 && zarch,
          "all mainframe target descriptors should be available");
    if (!s370 || !s370_xa || !s390 || !zarch) return;

    CHECK(s370->arch == ANVIL_ARCH_S370 &&
          s370->addr_bits == 24 &&
          s370->word_size == 4 &&
          s370->ptr_size == 4 &&
          s370->num_fpr == 4 &&
          s370->save_area_size == 72 &&
          s370->fp_format == ANVIL_FP_HFP &&
          s370->big_endian &&
          !s370->has_64bit_gprs &&
          !s370->supports_ieee_fp,
          "S/370 descriptor should model 24-bit HFP-only MVS linkage");

    CHECK(s370_xa->arch == ANVIL_ARCH_S370_XA &&
          s370_xa->addr_bits == 31 &&
          s370_xa->word_size == 4 &&
          s370_xa->ptr_size == 4 &&
          s370_xa->save_area_size == 72 &&
          s370_xa->big_endian &&
          !s370_xa->has_64bit_gprs,
          "S/370-XA descriptor should model 31-bit MVS linkage");

    CHECK(s390->arch == ANVIL_ARCH_S390 &&
          s390->addr_bits == 31 &&
          s390->word_size == 4 &&
          s390->ptr_size == 4 &&
          s390->num_fpr == 16 &&
          s390->supports_ieee_fp &&
          s390->has_relative_branches,
          "S/390 descriptor should model 31-bit ESA/390 capabilities");

    CHECK(zarch->arch == ANVIL_ARCH_ZARCH &&
          zarch->addr_bits == 64 &&
          zarch->word_size == 8 &&
          zarch->ptr_size == 8 &&
          zarch->num_fpr == 16 &&
          zarch->save_area_size == 144 &&
          zarch->big_endian &&
          zarch->has_64bit_gprs &&
          zarch->supports_ieee_fp &&
          zarch->supports_dfp,
          "z/Architecture descriptor should model s390x/z 64-bit linkage");
}

static void test_s370_lowers_i32_add_through_mvs_parameter_list(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_S370);
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = build_iadd_func(ctx, &mod, "s370_mir_iadd",
                                       "s370_iadd", anvil_type_i32(ctx));
    if (fn) {
        anvil_mir_func_t *mir =
            anvil_mainframe_lower_func_to_mir(fn, ANVIL_MAINFRAME_VARIANT_S370);
        CHECK(mir != NULL, "S/370 integer add should lower to MachineIR");
        if (mir) {
            CHECK(anvil_mir_num_instrs(mir) == 5,
                  "S/370 add should load two MVS params, add, copy return, ret");

            anvil_mir_instr_info_t arg0;
            anvil_mir_instr_info_t arg1;
            anvil_mir_instr_info_t add;
            anvil_mir_instr_info_t ret_copy;
            CHECK(anvil_mir_get_instr_info(mir, 0, &arg0),
                  "first S/370 parameter load should be inspectable");
            CHECK(anvil_mir_get_instr_info(mir, 1, &arg1),
                  "second S/370 parameter load should be inspectable");
            CHECK(anvil_mir_get_instr_info(mir, 2, &add),
                  "S/370 add instruction should be inspectable");
            CHECK(anvil_mir_get_instr_info(mir, 3, &ret_copy),
                  "S/370 return copy should be inspectable");

            CHECK(arg0.op == ANVIL_MIR_OP_INCOMING_STACK_ARG &&
                  arg0.has_imm && arg0.imm == 0,
                  "first S/370 argument should come from R1 parameter slot 0");
            CHECK(arg1.op == ANVIL_MIR_OP_INCOMING_STACK_ARG &&
                  arg1.has_imm && arg1.imm == 4,
                  "second S/370 argument should come from R1 parameter slot 4");
            CHECK(add.op == ANVIL_MIR_OP_ADD,
                  "S/370 integer add should use canonical MachineIR add");

            const anvil_mir_vreg_info_t *ret_info =
                anvil_mir_get_vreg_info(mir, ret_copy.def);
            CHECK(ret_info &&
                  ret_info->reg_class == ANVIL_MIR_REG_GPR &&
                  ret_info->size_bits == 32 &&
                  ret_info->has_fixed_reg &&
                  ret_info->fixed_phys_reg == 15,
                  "S/370 integer return value should be fixed to R15");

            CHECK(anvil_mainframe_regalloc_mir(mir, ANVIL_MAINFRAME_VARIANT_S370),
                  "S/370 MachineIR regalloc should succeed");
            const anvil_regalloc_assignment_t *ret_assignment =
                anvil_mir_get_assignment(mir, ret_copy.def);
            CHECK(ret_assignment && !ret_assignment->spilled &&
                  ret_assignment->phys_reg == 15,
                  "S/370 return assignment should stay in R15");

            anvil_mir_func_destroy(mir);
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_zarch_lowers_and_emits_64bit_hlasm(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_ZARCH);
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = build_iadd_func(ctx, &mod, "zarch_mir_iadd",
                                       "z_iadd64", anvil_type_i64(ctx));
    if (fn) {
        anvil_mir_func_t *mir =
            anvil_mainframe_lower_func_to_mir(fn, ANVIL_MAINFRAME_VARIANT_ZARCH);
        CHECK(mir != NULL, "z/Architecture integer add should lower to MachineIR");
        if (mir) {
            anvil_mir_instr_info_t arg0;
            anvil_mir_instr_info_t arg1;
            anvil_mir_instr_info_t add;
            CHECK(anvil_mir_get_instr_info(mir, 0, &arg0),
                  "first z/Architecture parameter load should be inspectable");
            CHECK(anvil_mir_get_instr_info(mir, 1, &arg1),
                  "second z/Architecture parameter load should be inspectable");
            CHECK(anvil_mir_get_instr_info(mir, 2, &add),
                  "z/Architecture add should be inspectable");
            CHECK(arg0.op == ANVIL_MIR_OP_INCOMING_STACK_ARG &&
                  arg0.has_imm && arg0.imm == 0,
                  "first z/Architecture argument should use parameter slot 0");
            CHECK(arg1.op == ANVIL_MIR_OP_INCOMING_STACK_ARG &&
                  arg1.has_imm && arg1.imm == 8,
                  "second z/Architecture argument should use parameter slot 8");
            CHECK(add.op == ANVIL_MIR_OP_ADD,
                  "z/Architecture integer add should use MachineIR add");

            CHECK(anvil_mainframe_regalloc_mir(mir, ANVIL_MAINFRAME_VARIANT_ZARCH),
                  "z/Architecture MachineIR regalloc should succeed");

            char *asm_text = NULL;
            size_t asm_len = 0;
            CHECK(anvil_mainframe_emit_mir(mir, ANVIL_MAINFRAME_VARIANT_ZARCH,
                                           &asm_text, &asm_len),
                  "z/Architecture MachineIR emitter should produce HLASM");
            CHECK(asm_text && asm_len > 0,
                  "z/Architecture HLASM output should be non-empty");
            if (asm_text) {
                check_contains(asm_text, "STMG  R14,R12,24(R13)",
                               "z/Architecture prologue should use 64-bit save area");
                check_contains(asm_text, "LGR   R12,R15",
                               "z/Architecture prologue should establish 64-bit base register");
                check_contains(asm_text, "LG    R",
                               "z/Architecture params should be loaded through R1 parameter list");
                check_contains(asm_text, "AGR   R",
                               "z/Architecture i64 add should use AGR");
                check_contains(asm_text, "BR    R14",
                               "z/Architecture epilogue should branch to R14");
                free(asm_text);
            }

            anvil_mir_func_destroy(mir);
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_s370_constant_gep_index_does_not_create_64bit_gpr(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_S370);
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "s370_const_gep");
    CHECK(mod != NULL, "S/370 const GEP module should be created");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *arr_type = anvil_type_array(ctx, i32, 4);
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, NULL, 0, false);
        anvil_func_t *fn = anvil_func_create(mod, "s370_const_gep", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "S/370 const GEP function should be created");
        if (fn && arr_type) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *arr = anvil_build_alloca(ctx, arr_type, "arr");
            anvil_value_t *idx = anvil_const_i64(ctx, 2);
            anvil_value_t *slot =
                anvil_build_gep(ctx, i32, arr, &idx, 1, "slot");
            anvil_build_store(ctx, anvil_const_i32(ctx, 99), slot);
            anvil_value_t *loaded = anvil_build_load(ctx, i32, slot, "loaded");
            anvil_build_ret(ctx, loaded);

            char *asm_text = NULL;
            size_t asm_len = 0;
            CHECK(anvil_module_codegen(mod, &asm_text, &asm_len) == ANVIL_OK,
                  "S/370 const GEP should codegen without materializing an i64 GPR");
            CHECK(asm_text && asm_len > 0,
                  "S/370 const GEP output should be non-empty");
            free(asm_text);
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_s370_global_array_address_is_pointer_sized(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_S370);
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "s370_global_array");
    CHECK(mod != NULL, "S/370 global array module should be created");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *arr_type = anvil_type_array(ctx, i32, 10);
        anvil_value_t *global =
            anvil_module_add_global(mod, "arr", arr_type, ANVIL_LINK_EXTERNAL);
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, NULL, 0, false);
        anvil_func_t *fn = anvil_func_create(mod, "s370_global_array",
                                             fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(global != NULL, "S/370 global array should be created");
        CHECK(fn != NULL, "S/370 global array function should be created");
        if (global && fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *idx = anvil_const_i64(ctx, 0);
            anvil_value_t *slot =
                anvil_build_gep(ctx, i32, global, &idx, 1, "slot");
            anvil_build_store(ctx, anvil_const_i32(ctx, 7), slot);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 0));

            char *asm_text = NULL;
            size_t asm_len = 0;
            CHECK(anvil_module_codegen(mod, &asm_text, &asm_len) == ANVIL_OK,
                  "S/370 global array address should lower as a pointer");
            CHECK(asm_text && asm_len > 0,
                  "S/370 global array output should be non-empty");
            if (asm_text) {
                check_contains(asm_text, "ARR",
                               "global array symbol should be emitted uppercase");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_s370_i64_constant_store_uses_big_endian_word_halves(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_S370);
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "s370_i64_store");
    CHECK(mod != NULL, "S/370 i64 store module should be created");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, NULL, 0, false);
        anvil_func_t *fn = anvil_func_create(mod, "s370_i64_store", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "S/370 i64 store function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *slot = anvil_build_alloca(ctx, i64, "slot");
            anvil_build_store(ctx, anvil_const_i64(ctx, 1234605616436508552LL),
                              slot);
            anvil_value_t *neg = anvil_build_neg(ctx, anvil_const_i64(ctx, 5),
                                                 "neg");
            anvil_build_store(ctx, neg, slot);
            anvil_build_ret(ctx, anvil_const_i32(ctx, 0));

            char *asm_text = NULL;
            size_t asm_len = 0;
            CHECK(anvil_module_codegen(mod, &asm_text, &asm_len) == ANVIL_OK,
                  "S/370 i64 constant stores should codegen without a 64-bit GPR");
            CHECK(asm_text && asm_len > 0,
                  "S/370 i64 constant store output should be non-empty");
            if (asm_text) {
                check_contains(asm_text, "=F'287454020'",
                               "S/370 i64 constant high word should be emitted");
                check_contains(asm_text, "=F'1432778632'",
                               "S/370 i64 constant low word should be emitted");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_hlasm_control_text_is_uppercase(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_ZARCH);
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = build_iadd_func(ctx, &mod, "uppercase_hlasm",
                                       "mixed_case_entry", anvil_type_i64(ctx));
    if (fn) {
        char *asm_text = NULL;
        size_t asm_len = 0;
        CHECK(anvil_module_codegen(mod, &asm_text, &asm_len) == ANVIL_OK,
              "uppercase HLASM module should codegen");
        CHECK(asm_text && asm_len > 0,
              "uppercase HLASM output should be non-empty");
        if (asm_text) {
            CHECK(!hlasm_has_lowercase_outside_literals(asm_text),
                  "HLASM control text should be uppercase outside quoted literals");
            check_contains(asm_text, "MIXED_CASE_ENTRY",
                           "function labels should be uppercased in HLASM");
            free(asm_text);
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_fp_format_selection_is_target_honest(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_ZARCH);
    if (!ctx) return;

    CHECK(anvil_ctx_set_fp_format(ctx, ANVIL_FP_IEEE754) == ANVIL_OK,
          "z/Architecture should allow IEEE floating point");
    anvil_module_t *mod = anvil_module_create(ctx, "zarch_fp");
    CHECK(mod != NULL, "FP module should be created");
    if (mod) {
        anvil_type_t *f64 = anvil_type_f64(ctx);
        anvil_type_t *params[] = { f64, f64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, f64, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "z_fadd", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "FP function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *sum = anvil_build_fadd(ctx, a, b, "sum");
            anvil_build_ret(ctx, sum);

            char *asm_text = NULL;
            size_t asm_len = 0;
            CHECK(anvil_module_codegen(mod, &asm_text, &asm_len) == ANVIL_OK,
                  "z/Architecture IEEE FP module should codegen");
            CHECK(asm_text && asm_len > 0, "FP module output should be non-empty");
            if (asm_text) {
                check_contains(asm_text, "ADBR",
                               "z/Architecture IEEE f64 add should use ADBR");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);

    ctx = new_mainframe_ctx(ANVIL_ARCH_S370);
    if (!ctx) return;
    CHECK(anvil_ctx_set_fp_format(ctx, ANVIL_FP_IEEE754) == ANVIL_ERR_INVALID_ARG,
          "S/370 must reject IEEE floating point selection");
    anvil_ctx_destroy(ctx);
}

static void test_decimal_types_and_hlasm_constants_are_first_class(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_ZARCH);
    if (!ctx) return;

    anvil_type_t *packed = anvil_type_decimal_packed(ctx, 7, 2);
    anvil_type_t *zoned = anvil_type_decimal_zoned(ctx, 5, 0);
    CHECK(packed != NULL, "packed decimal type should be created");
    CHECK(zoned != NULL, "zoned decimal type should be created");
    if (packed && zoned) {
        CHECK(anvil_type_decimal_encoding(packed) == ANVIL_DECIMAL_PACKED &&
              anvil_type_decimal_precision(packed) == 7 &&
              anvil_type_decimal_scale(packed) == 2 &&
              anvil_type_size(packed) == 4 &&
              anvil_type_align(packed) == 1,
              "packed decimal(7,2) should occupy 4 bytes");
        CHECK(anvil_type_decimal_encoding(zoned) == ANVIL_DECIMAL_ZONED &&
              anvil_type_decimal_precision(zoned) == 5 &&
              anvil_type_decimal_scale(zoned) == 0 &&
              anvil_type_size(zoned) == 5 &&
              anvil_type_align(zoned) == 1,
              "zoned decimal(5,0) should occupy 5 bytes");
    }

    anvil_module_t *mod = anvil_module_create(ctx, "decimal_globals");
    CHECK(mod != NULL, "decimal module should be created");
    if (mod && packed && zoned) {
        anvil_value_t *packed_global =
            anvil_module_add_global(mod, "packed_amt", packed, ANVIL_LINK_EXTERNAL);
        anvil_value_t *zoned_global =
            anvil_module_add_global(mod, "zoned_code", zoned, ANVIL_LINK_EXTERNAL);
        anvil_global_set_initializer(packed_global,
            anvil_const_decimal(ctx, packed, "-12345"));
        anvil_global_set_initializer(zoned_global,
            anvil_const_decimal(ctx, zoned, "67890"));

        char *asm_text = NULL;
        size_t asm_len = 0;
        CHECK(anvil_module_codegen(mod, &asm_text, &asm_len) == ANVIL_OK,
              "decimal globals should codegen for z/Architecture");
        CHECK(asm_text && asm_len > 0,
              "decimal global HLASM output should be non-empty");
        if (asm_text) {
            check_contains(asm_text, "PACKED_AMT",
                           "packed decimal global should be emitted with uppercase symbol");
            check_contains(asm_text, "DC    PL4'-12345'",
                           "packed decimal global should use HLASM packed decimal constant");
            check_contains(asm_text, "ZONED_CODE",
                           "zoned decimal global should be emitted with uppercase symbol");
            check_contains(asm_text, "DC    ZL5'67890'",
                           "zoned decimal global should use HLASM zoned decimal constant");
            free(asm_text);
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    test_mainframe_descriptors_model_real_variants();
    test_s370_lowers_i32_add_through_mvs_parameter_list();
    test_zarch_lowers_and_emits_64bit_hlasm();
    test_s370_constant_gep_index_does_not_create_64bit_gpr();
    test_s370_global_array_address_is_pointer_sized();
    test_s370_i64_constant_store_uses_big_endian_word_halves();
    test_hlasm_control_text_is_uppercase();
    test_fp_format_selection_is_target_honest();
    test_decimal_types_and_hlasm_constants_are_first_class();

    if (failures != 0) {
        fprintf(stderr, "%d mainframe MachineIR regression checks failed\n", failures);
        return 1;
    }

    printf("mainframe MachineIR regression checks passed\n");
    return 0;
}
