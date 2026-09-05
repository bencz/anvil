/*
 * Regression tests for the shared IBM mainframe -> MachineIR backend path.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_machine.h>
#include <anvil/anvil_mainframe_mir.h>

#include <stdio.h>
#include <stdint.h>
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

static size_t count_occurrences(const char *text, const char *needle)
{
    size_t count = 0;
    size_t needle_len = needle ? strlen(needle) : 0;
    if (!text || needle_len == 0) return 0;
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += needle_len;
    }
    return count;
}

static int hlasm_dynamic_frame_size(const char *text)
{
    const char *equ = text ? strstr(text, "_DYN EQU   ") : NULL;
    return equ ? atoi(equ + strlen("_DYN EQU   ")) : -1;
}

static size_t hlasm_store_offsets(const char *text, const char *opcode,
                                  int *offsets, size_t capacity)
{
    size_t count = 0;
    size_t opcode_len = strlen(opcode);
    const char *line = text;
    while (line && *line) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *found = strstr(line, opcode);
        if (found && found < end) {
            const char *comma = strchr(found + opcode_len, ',');
            if (comma && comma < end && count < capacity) {
                offsets[count++] = (int)strtol(comma + 1, NULL, 10);
            }
        }
        line = *end ? end + 1 : NULL;
    }
    return count;
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
            anvil_value_t *idx[] = { anvil_const_i64(ctx, 0),
                                     anvil_const_i64(ctx, 2) };
            anvil_value_t *slot =
                anvil_build_gep(ctx, arr_type, arr, idx, 2, "slot");
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
            anvil_value_t *idx[] = { anvil_const_i64(ctx, 0),
                                     anvil_const_i64(ctx, 0) };
            anvil_value_t *slot =
                anvil_build_gep(ctx, arr_type, global, idx, 2, "slot");
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

static void test_mainframe_frame_allocation_separates_nested_calls(void)
{
    struct {
        anvil_mainframe_variant_t variant;
        uint16_t ptr_bits;
        uint16_t slot_bits;
        int local_offset;
        const char *name;
    } cases[] = {
        { ANVIL_MAINFRAME_VARIANT_S390, 32, 512, 88, "frame32" },
        { ANVIL_MAINFRAME_VARIANT_ZARCH, 64, 1024, 160, "frame64" },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_mir_func_t *mir = anvil_mir_func_create(cases[c].name);
        CHECK(mir != NULL, "mainframe frame-layout MIR should be created");
        if (!mir) continue;

        anvil_mir_vreg_t addr = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_GPR, cases[c].ptr_bits);
        anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_GPR, 32);
        int slot = anvil_mir_add_frame_slot(mir, cases[c].slot_bits, 8);
        anvil_mir_vreg_t store_uses[] = { value, addr };
        CHECK(slot >= 0 && anvil_mir_add_frame_addr(mir, addr, slot),
              "mainframe local frame address should be represented");
        CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, value, 7),
              "mainframe local frame test should define its value");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_STORE,
                                  ANVIL_MIR_NO_VREG, store_uses, 2),
              "mainframe local frame test should store into its frame");
        CHECK(anvil_mir_add_call(mir, ANVIL_MIR_NO_VREG, NULL, 0,
                                 "nested_callee", ANVIL_CC_MVS, false, 0),
              "mainframe frame test should contain a nested call");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "mainframe frame test should terminate");
        CHECK(anvil_mainframe_regalloc_mir(mir, cases[c].variant),
              "mainframe frame-layout MIR should allocate");

        char *asm_text = NULL;
        size_t asm_len = 0;
        CHECK(anvil_mainframe_emit_mir(mir, cases[c].variant,
                                       &asm_text, &asm_len),
              "mainframe frame-layout MIR should emit");
        if (asm_text) {
            int frame_size = hlasm_dynamic_frame_size(asm_text);
            int slot_bytes = (cases[c].slot_bits + 7) / 8;
            char prologue[64];
            char local[64];
            snprintf(prologue, sizeof(prologue),
                     "LA    R2,%d(,R13)", frame_size);
            snprintf(local, sizeof(local), ",%d(,R11)", cases[c].local_offset);
            CHECK(frame_size >= cases[c].local_offset + slot_bytes,
                  "allocated mainframe frame must contain the complete local slot");
            CHECK(frame_size > cases[c].local_offset,
                  "next callee save area must begin after current-frame locals");
            check_contains(asm_text, prologue,
                           "prologue must advance R13 by the complete dynamic frame size");
            check_contains(asm_text, local,
                           "locals must use the stable current-frame base R11");
            CHECK(strstr(asm_text, "LGR   R11,R1\n") == NULL &&
                  strstr(asm_text, "LR    R11,R1\n") == NULL,
                  "prologue must not overwrite the frame base with the parameter list");
            free(asm_text);
        }
        anvil_mir_func_destroy(mir);
    }
}

static void test_mainframe_materializes_call_values_before_bundle(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_S370);
    if (!ctx) return;
    anvil_module_t *mod = anvil_module_create(ctx, "call_bundle_constants");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *callee_params[] = { i32, i32 };
    anvil_func_t *callee = anvil_func_declare(mod, "callee",
        anvil_type_func(ctx, i32, callee_params, 2, false));
    anvil_func_t *caller = anvil_func_create(mod, "caller",
        anvil_type_func(ctx, i32, NULL, 0, false), ANVIL_LINK_EXTERNAL);
    CHECK(callee && caller, "mainframe call-bundle source functions should build");
    if (callee && caller) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
        anvil_value_t *args[] = {
            anvil_const_i32(ctx, 11), anvil_const_i32(ctx, 22)
        };
        anvil_value_t *result = NULL;
        anvil_build_call_checked(ctx, anvil_func_get_value(callee), args, 2,
                                 "result", &result);
        CHECK(result && anvil_build_ret(ctx, result),
              "mainframe constant-argument call should build");
        anvil_mir_func_t *mir = anvil_mainframe_lower_func_to_mir(
            caller, ANVIL_MAINFRAME_VARIANT_S370);
        CHECK(mir != NULL, "mainframe constant-argument call should lower");
        bool contiguous = false;
        if (mir) for (size_t i = 2; i < anvil_mir_num_instrs(mir); i++) {
            anvil_mir_instr_info_t a, b, call;
            if (anvil_mir_get_instr_info(mir, i - 2, &a) &&
                anvil_mir_get_instr_info(mir, i - 1, &b) &&
                anvil_mir_get_instr_info(mir, i, &call) &&
                a.op == ANVIL_MIR_OP_CALL_STACK_ARG && a.imm == 0 &&
                b.op == ANVIL_MIR_OP_CALL_STACK_ARG && b.imm == 1 &&
                call.op == ANVIL_MIR_OP_CALL) contiguous = true;
        }
        CHECK(contiguous,
              "all argument values must materialize before the contiguous call bundle");
        CHECK(mir && anvil_mainframe_regalloc_mir(
                         mir, ANVIL_MAINFRAME_VARIANT_S370),
              "constant-argument S370 call bundle should legalize and allocate");
        anvil_mir_func_destroy(mir);
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_mainframe_fp_parameters_use_gpr_address_scratch(void)
{
    anvil_mainframe_variant_t variants[] = {
        ANVIL_MAINFRAME_VARIANT_S390,
        ANVIL_MAINFRAME_VARIANT_ZARCH,
    };
    for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
        anvil_mir_func_t *mir = anvil_mir_func_create("fp_param_address");
        CHECK(mir != NULL, "mainframe FP parameter MIR should be created");
        if (!mir) continue;
        anvil_mir_vreg_t fp = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_FPR, 64);
        anvil_mir_vreg_t ret_uses[] = { fp };
        CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_INCOMING_STACK_ARG,
                                      fp, 0),
              "mainframe FP parameter should use an incoming stack pseudo");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, ret_uses, 1),
              "mainframe FP parameter MIR should terminate");
        CHECK(anvil_mainframe_regalloc_mir(mir, variants[v]),
              "mainframe FP parameter MIR should allocate");

        char *asm_text = NULL;
        size_t asm_len = 0;
        CHECK(anvil_mainframe_emit_mir(mir, variants[v], &asm_text, &asm_len),
              "mainframe FP parameter MIR should emit");
        if (asm_text) {
            check_contains(asm_text,
                variants[v] == ANVIL_MAINFRAME_VARIANT_ZARCH
                    ? "LG    R0,0(,R1)" : "L     R0,0(,R1)",
                "FP parameter pointer must be loaded into GPR scratch R0");
            check_contains(asm_text, "LD    F",
                           "f64 parameter value should be loaded into its FPR");
            CHECK(strstr(asm_text, "(,F") == NULL,
                  "generated HLASM must never use an FPR as an address register");
            free(asm_text);
        }
        anvil_mir_func_destroy(mir);
    }
}

static void test_s390_call_bundles_layout_f64_and_mark_each_call(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("call_bundles_32");
    CHECK(mir != NULL, "S/390 call-bundle MIR should be created");
    if (!mir) return;

    anvil_mir_vreg_t f0 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t f1 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t f2 = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t arg0[] = { f0 };
    anvil_mir_vreg_t arg1[] = { f1 };
    anvil_mir_vreg_t arg2[] = { f2 };
    CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, f0, 0),
          "first f64 call value should be defined");
    CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, f1, 0),
          "second f64 call value should be defined");
    CHECK(anvil_mir_add_instr_imm_uses(mir, ANVIL_MIR_OP_CALL_STACK_ARG,
                                       ANVIL_MIR_NO_VREG, arg0, 1, 0),
          "first call should contain argument zero");
    CHECK(anvil_mir_add_instr_imm_uses(mir, ANVIL_MIR_OP_CALL_STACK_ARG,
                                       ANVIL_MIR_NO_VREG, arg1, 1, 1),
          "first call should contain argument one");
    CHECK(anvil_mir_add_call(mir, ANVIL_MIR_NO_VREG, NULL, 0, "callee_two",
                             ANVIL_CC_MVS, false, 0),
          "first call should terminate its argument bundle");
    CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, f2, 0),
          "second call f64 value should be defined after the first call");
    CHECK(anvil_mir_add_instr_imm_uses(mir, ANVIL_MIR_OP_CALL_STACK_ARG,
                                       ANVIL_MIR_NO_VREG, arg2, 1, 0),
          "second call should contain one argument");
    CHECK(anvil_mir_add_call(mir, ANVIL_MIR_NO_VREG, NULL, 0, "callee_one",
                             ANVIL_CC_MVS, false, 0),
          "second call should terminate its argument bundle");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, NULL, 0),
          "S/390 call-bundle MIR should terminate");
    CHECK(anvil_mainframe_regalloc_mir(mir, ANVIL_MAINFRAME_VARIANT_S390),
          "S/390 call-bundle MIR should allocate");

    char *asm_text = NULL;
    size_t asm_len = 0;
    CHECK(anvil_mainframe_emit_mir(mir, ANVIL_MAINFRAME_VARIANT_S390,
                                   &asm_text, &asm_len),
          "S/390 call-bundle MIR should emit");
    if (asm_text) {
        int offsets[3] = { -1, -1, -1 };
        size_t stores = hlasm_store_offsets(asm_text, "STD", offsets, 3);
        CHECK(stores == 3, "both S/390 call bundles should store all f64 values");
        CHECK(stores == 3 && offsets[1] >= offsets[0] + 8,
              "two f64 values in one 32-bit call bundle must not overlap");
        CHECK(stores == 3 && offsets[2] == offsets[0],
              "a later call may safely reuse the outgoing value area");
        CHECK(count_occurrences(asm_text, "O     R0,=X'80000000'") == 2,
              "the last-parameter marker must be emitted once per nonempty call");
        for (size_t i = 0; i < stores; i++) {
            char stable_store[48];
            snprintf(stable_store, sizeof(stable_store),
                     ",%d(,R11)", offsets[i]);
            check_contains(asm_text, stable_store,
                           "outgoing call data must use stable frame base R11");
        }
        free(asm_text);
    }
    anvil_mir_func_destroy(mir);
}

static void test_zarch_spilled_call_arguments_preserve_bundle(void)
{
    enum { ARG_COUNT = 12 };
    anvil_mir_func_t *mir = anvil_mir_func_create("zarch_spilled_call_bundle");
    CHECK(mir != NULL, "z/Architecture spilled-call MIR should be created");
    if (!mir) return;

    anvil_mir_vreg_t args[ARG_COUNT];
    bool built = true;
    for (size_t i = 0; i < ARG_COUNT; i++) {
        args[i] = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 64);
        built = built && args[i] != ANVIL_MIR_NO_VREG;
        if (i % 2 == 0)
        {
            built = built && anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, args[i], (int64_t)(i + 1));
        }
        else
        {
            anvil_mir_vreg_t uses[] = { args[i - 1], args[i - 1] };
            built = built && anvil_mir_add_instr(mir, ANVIL_MIR_OP_ADD, args[i], uses, 2);
        }
    }
    for (size_t i = 0; i < ARG_COUNT; i++) {
        built = built && anvil_mir_add_instr_imm_uses(
            mir, ANVIL_MIR_OP_CALL_STACK_ARG, ANVIL_MIR_NO_VREG,
            &args[i], 1, (int64_t)i);
    }
    built = built && anvil_mir_add_call(
        mir, ANVIL_MIR_NO_VREG, NULL, 0, "spill_target",
        ANVIL_CC_MVS, false, 0) &&
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                            ANVIL_MIR_NO_VREG, NULL, 0);
    CHECK(built, "z/Architecture high-pressure call bundle should build");

    CHECK(anvil_mainframe_regalloc_mir(mir, ANVIL_MAINFRAME_VARIANT_ZARCH),
          "spill reloads interleaved with call arguments must remain legal");
    CHECK(anvil_mir_num_spills(mir) >= ARG_COUNT - 9,
          "twelve simultaneously-live arguments must exercise real spilling");

    bool in_bundle = false;
    bool saw_interleaved_reload = false;
    bool saw_interleaved_constant = false;
    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
        anvil_mir_instr_info_t info;
        CHECK(anvil_mir_get_instr_info(mir, i, &info),
              "materialized z/Architecture MIR should remain inspectable");
        if (!anvil_mir_get_instr_info(mir, i, &info)) break;
        if (info.op == ANVIL_MIR_OP_CALL_STACK_ARG) in_bundle = true;
        else if (in_bundle && info.op == ANVIL_MIR_OP_SPILL_LOAD)
            saw_interleaved_reload = true;
        else if (in_bundle && info.op == ANVIL_MIR_OP_MOV && info.has_imm)
            saw_interleaved_constant = true;
        else if (in_bundle && info.op == ANVIL_MIR_OP_CALL)
            in_bundle = false;
    }
    CHECK(saw_interleaved_reload,
          "regression must contain a spill reload inside the logical call bundle");
    CHECK(saw_interleaved_constant,
          "regression must contain a rematerialized constant inside the logical call bundle");

    char legal_error[192] = { 0 };
    CHECK(anvil_mainframe_verify_mir_legal(
              mir, ANVIL_MAINFRAME_VARIANT_ZARCH,
              legal_error, sizeof(legal_error)),
          "post-allocation spilled call bundle should pass mainframe legality");
    char *asm_text = NULL;
    size_t asm_len = 0;
    CHECK(anvil_mainframe_emit_mir(mir, ANVIL_MAINFRAME_VARIANT_ZARCH,
                                   &asm_text, &asm_len),
          "post-allocation spilled call bundle should emit HLASM");
    CHECK(asm_text &&
          count_occurrences(asm_text, "         LA    R0,") == ARG_COUNT,
          "every spilled or resident argument must reach the outgoing value area");
    CHECK(asm_text &&
          count_occurrences(asm_text, "         OIHH  R0,X'8000'") == 1,
          "the logical bundle must retain exactly one final-argument marker");
    free(asm_text);
    anvil_mir_func_destroy(mir);
}

static void test_mainframe_rejects_malformed_call_bundle(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("bad_call_bundle");
    CHECK(mir != NULL, "malformed mainframe call-bundle MIR should be created");
    if (!mir) return;
    anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(
        mir, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t uses[] = { value };
    CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, value, 1),
          "malformed bundle test value should be defined");
    CHECK(anvil_mir_add_instr_imm_uses(mir, ANVIL_MIR_OP_CALL_STACK_ARG,
                                       ANVIL_MIR_NO_VREG, uses, 1, 1),
          "generic MIR should represent a malformed first argument index");
    CHECK(anvil_mir_add_call(mir, ANVIL_MIR_NO_VREG, NULL, 0, "callee",
                             ANVIL_CC_MVS, false, 0),
          "malformed bundle should still have a call");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, NULL, 0),
          "malformed bundle MIR should terminate");
    char error[160] = { 0 };
    CHECK(!anvil_mainframe_verify_mir_legal(mir, ANVIL_MAINFRAME_VARIANT_S390,
                                            error, sizeof(error)),
          "mainframe legalizer must reject a malformed call bundle");
    CHECK(strstr(error, "malformed call argument bundle") != NULL,
          "malformed mainframe call bundle should produce a precise diagnostic");
    CHECK(!anvil_mainframe_regalloc_mir(mir, ANVIL_MAINFRAME_VARIANT_S390),
          "mainframe allocation pipeline must not admit a malformed call bundle");
    char *asm_text = NULL;
    size_t asm_len = 0;
    CHECK(!anvil_mainframe_emit_mir(mir, ANVIL_MAINFRAME_VARIANT_S390,
                                    &asm_text, &asm_len),
          "mainframe emitter must reject a structurally malformed call bundle");
    free(asm_text);
    anvil_mir_func_destroy(mir);
}

static void test_mainframe_dynamic_alloca_updates_stack_chain(void)
{
    struct {
        anvil_mainframe_variant_t variant;
        uint16_t bits;
        const char *multiply;
        const char *align;
        const char *backchain;
        const char *advance;
    } cases[] = {
        { ANVIL_MAINFRAME_VARIANT_S370, 32, "M     R0,=F'24'",
          "N     R", "ST    R11,4(,R0)", "LR    R13,R0" },
        { ANVIL_MAINFRAME_VARIANT_S370_XA, 32, "M     R0,=F'24'",
          "N     R", "ST    R11,4(,R0)", "LR    R13,R0" },
        { ANVIL_MAINFRAME_VARIANT_S390, 32, "M     R0,=F'24'",
          "N     R", "ST    R11,4(,R0)", "LR    R13,R0" },
        { ANVIL_MAINFRAME_VARIANT_ZARCH, 64, "MSGFI R0,24",
          "NILL  R", "STG   R11,8(,R0)", "LGR   R13,R0" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_mir_func_t *mir = anvil_mir_func_create("dynamic_alloca");
        anvil_mir_vreg_t count = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_GPR, cases[c].bits);
        anvil_mir_vreg_t address = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_GPR, cases[c].bits);
        anvil_mir_vreg_t uses[] = { count };
        CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, count, 7),
              "dynamic alloca count should be defined");
        CHECK(anvil_mir_add_instr_imm_uses(mir, ANVIL_MIR_OP_DYN_ALLOCA,
                                           address, uses, 1, 24),
              "dynamic alloca should retain its element size");
        CHECK(anvil_mir_add_call(mir, ANVIL_MIR_NO_VREG, NULL, 0, "nested",
                                 ANVIL_CC_MVS, false, 0),
              "dynamic alloca regression should include a nested call");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "dynamic alloca MIR should terminate");
        CHECK(anvil_mainframe_regalloc_mir(mir, cases[c].variant),
              "dynamic alloca MIR should allocate on every mainframe variant");
        char *text = NULL;
        size_t len = 0;
        CHECK(anvil_mainframe_emit_mir(mir, cases[c].variant, &text, &len),
              "dynamic alloca should emit on every mainframe variant");
        if (text) {
            check_contains(text, cases[c].multiply,
                           "dynamic alloca must multiply count by element size");
            check_contains(text, cases[c].align,
                           "dynamic alloca result/top must be aligned to 16 bytes");
            check_contains(text, cases[c].backchain,
                           "dynamic alloca must install a valid stack backchain");
            check_contains(text, cases[c].advance,
                           "dynamic alloca must advance the live R13 stack top");
            CHECK(strstr(text, "LA    R2,88(,R11)") == NULL,
                  "dynamic alloca must not collapse to the old fixed-address stub");
            free(text);
        }
        anvil_mir_func_destroy(mir);
    }
}

static void test_mainframe_narrow_loads_extend_exactly(void)
{
    anvil_mainframe_variant_t variants[] = {
        ANVIL_MAINFRAME_VARIANT_S370, ANVIL_MAINFRAME_VARIANT_S370_XA,
        ANVIL_MAINFRAME_VARIANT_S390, ANVIL_MAINFRAME_VARIANT_ZARCH,
    };
    for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
        bool wide = variants[v] == ANVIL_MAINFRAME_VARIANT_ZARCH;
        anvil_mir_func_t *mir = anvil_mir_func_create("narrow_loads");
        anvil_mir_vreg_t addr = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_GPR, wide ? 64 : 32);
        anvil_mir_vreg_t i8 = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, 8, true);
        anvil_mir_vreg_t u8 = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, 8, false);
        anvil_mir_vreg_t i16 = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, 16, true);
        anvil_mir_vreg_t u16 = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, 16, false);
        anvil_mir_vreg_t address_use[] = { addr };
        CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, addr, 1024),
              "narrow-load address should be defined");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_LOAD, i8, address_use, 1) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_LOAD, u8, address_use, 1) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_LOAD, i16, address_use, 1) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_LOAD, u16, address_use, 1),
              "all signed/unsigned narrow loads should be represented");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "narrow-load MIR should terminate");
        CHECK(anvil_mainframe_regalloc_mir(mir, variants[v]),
              "narrow-load MIR should allocate");
        char *text = NULL;
        size_t len = 0;
        CHECK(anvil_mainframe_emit_mir(mir, variants[v], &text, &len),
              "narrow loads should emit");
        if (text) {
            CHECK(count_occurrences(text, "IC    R") == 2,
                  "i8/u8 loads should use exactly two byte loads");
            check_contains(text, "SLL   R", "signed i8 load must shift for sign extension");
            check_contains(text, "SRA   R", "signed i8 load must arithmetically extend");
            CHECK(count_occurrences(text, "LH    R") == 2,
                  "i16/u16 loads should use exactly two halfword loads");
            check_contains(text, "=X'0000FFFF'",
                           "unsigned i16 load must clear sign-extended bits");
            if (wide) {
                check_contains(text, "LGFR  R",
                               "signed narrow z/Arch load must normalize all 64 bits");
                check_contains(text, "LLGFR R",
                               "unsigned narrow z/Arch load must normalize all 64 bits");
            }
            free(text);
        }
        anvil_mir_func_destroy(mir);
    }
}

static uint64_t restoring_udiv_model(uint64_t dividend, uint64_t divisor,
                                     unsigned bits, uint64_t *remainder)
{
    uint64_t mask = bits == 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
    uint64_t quotient = dividend & mask;
    uint64_t rem = 0;
    for (unsigned i = 0; i < bits; i++) {
        bool carry = ((rem >> (bits - 1)) & 1u) != 0;
        uint64_t input = (quotient >> (bits - 1)) & 1u;
        rem = ((rem << 1) | input) & mask;
        quotient = (quotient << 1) & mask;
        if (carry || rem >= divisor) {
            rem = (rem - divisor) & mask;
            quotient |= 1;
        }
    }
    if (remainder) *remainder = rem;
    return quotient;
}

static void check_restoring_division_oracle(unsigned bits)
{
    uint64_t mask = bits == 64 ? UINT64_MAX : UINT32_MAX;
    uint64_t divisors[] = {
        (UINT64_C(1) << (bits - 1)) + 1,
        mask,
        (UINT64_C(1) << (bits - 2)) + 3,
        3,
    };
    uint64_t dividends[] = { 0, 1, mask, mask - 1,
                             (UINT64_C(1) << (bits - 1)),
                             (UINT64_C(1) << (bits - 1)) + 1 };
    for (size_t d = 0; d < sizeof(divisors) / sizeof(divisors[0]); d++) {
        for (size_t n = 0; n < sizeof(dividends) / sizeof(dividends[0]); n++) {
            uint64_t rem = 0;
            uint64_t q = restoring_udiv_model(
                dividends[n], divisors[d], bits, &rem);
            CHECK(q == dividends[n] / divisors[d] &&
                  rem == dividends[n] % divisors[d],
                  "restoring unsigned division must match host / and % at boundaries");
        }
    }
    uint64_t state = UINT64_C(0x9e3779b97f4a7c15);
    for (unsigned i = 0; i < 1000; i++) {
        state = state * UINT64_C(6364136223846793005) + 1;
        uint64_t dividend = state & mask;
        state = state * UINT64_C(6364136223846793005) + 1;
        uint64_t divisor = (state & mask) | 1;
        uint64_t rem = 0;
        uint64_t q = restoring_udiv_model(dividend, divisor, bits, &rem);
        CHECK(q == dividend / divisor && rem == dividend % divisor,
              "restoring unsigned division must pass deterministic differential cases");
    }
}

static void test_mainframe_unsigned_divmod_and_not64_are_real(void)
{
    anvil_mainframe_variant_t variants[] = {
        ANVIL_MAINFRAME_VARIANT_S370, ANVIL_MAINFRAME_VARIANT_S370_XA,
        ANVIL_MAINFRAME_VARIANT_S390, ANVIL_MAINFRAME_VARIANT_ZARCH,
    };
    CHECK(UINT32_MAX / 2u == 2147483647u && UINT32_MAX % 2u == 1u,
          "unsigned 32-bit boundary reference must be exact");
    CHECK(UINT64_MAX / UINT64_C(3) == UINT64_C(6148914691236517205) &&
          UINT64_MAX % UINT64_C(3) == 0,
          "unsigned 64-bit boundary reference must be exact");
    check_restoring_division_oracle(32);
    check_restoring_division_oracle(64);
    for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
        bool wide = variants[v] == ANVIL_MAINFRAME_VARIANT_ZARCH;
        uint16_t bits = wide ? 64 : 32;
        anvil_mir_func_t *mir = anvil_mir_func_create("unsigned_divmod");
        anvil_mir_vreg_t lhs = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, bits, false);
        anvil_mir_vreg_t rhs = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, bits, false);
        anvil_mir_vreg_t quot = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, bits, false);
        anvil_mir_vreg_t rem = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, bits, false);
        anvil_mir_vreg_t inv = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, bits, false);
        anvil_mir_vreg_t operands[] = { lhs, rhs };
        anvil_mir_vreg_t unary[] = { lhs };
        CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, lhs, -1) &&
              anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, rhs, wide ? 3 : 2),
              "unsigned division boundary operands should be defined");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_UDIV, quot, operands, 2) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_UMOD, rem, operands, 2) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_NOT, inv, unary, 1),
              "unsigned div/mod and NOT should be represented");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "unsigned arithmetic MIR should terminate");
        CHECK(anvil_mainframe_regalloc_mir(mir, variants[v]),
              "unsigned arithmetic MIR should allocate");
        char *text = NULL;
        size_t len = 0;
        CHECK(anvil_mainframe_emit_mir(mir, variants[v], &text, &len),
              "unsigned arithmetic should emit");
        if (text) {
            check_contains(text, wide ? "CLGR  R0," : "CLR   R0,",
                           "unsigned division must compare without a sign bit");
            CHECK(strstr(text, wide ? "DSGR  R0," : "DR    R0,") == NULL,
                  "unsigned division/modulo must never use signed divide");
            check_contains(text, wide ? "=X'FFFFFFFFFFFFFFFF'" : "=X'FFFFFFFF'",
                           "NOT must invert the full target integer width");
            free(text);
        }
        anvil_mir_func_destroy(mir);
    }
}

static uint64_t legacy_hfp_from_u32(uint32_t magnitude, bool negative)
{
    return (negative ? UINT64_C(0x8000000000000000) : 0) |
           UINT64_C(0x4e00000000000000) | magnitude;
}

static int32_t legacy_hfp_trunc_i32(uint64_t bits)
{
    unsigned exponent = (unsigned)((bits >> 56) & 0x7f);
    uint64_t fraction = bits & UINT64_C(0x00ffffffffffffff);
    int shift = 312 - (int)(4 * exponent);
    uint32_t magnitude = shift >= 56 ? 0
        : shift >= 0 ? (uint32_t)(fraction >> shift)
                     : (uint32_t)(fraction << -shift);
    return (bits >> 63) ? (int32_t)(0u - magnitude) : (int32_t)magnitude;
}

static void test_mainframe_numeric_conversions_use_target_isa(void)
{
    struct {
        anvil_mainframe_variant_t variant;
        uint16_t int_bits;
        const char *sitofp;
        const char *fptosi;
        const char *fpext;
        const char *fptrunc;
    } cases[] = {
        { ANVIL_MAINFRAME_VARIANT_S370, 32, "=X'4E000000'", "=F'312'", "LDER", "LEDR" },
        { ANVIL_MAINFRAME_VARIANT_S370_XA, 32, "=X'4E000000'", "=F'312'", "LDER", "LEDR" },
        { ANVIL_MAINFRAME_VARIANT_S390, 32, "CEFR", "CFDR", "LDER", "LEDR" },
        { ANVIL_MAINFRAME_VARIANT_ZARCH, 64, "CEGBR", "CGDBR", "LDEBR", "LEDBR" },
    };
    /* These values exercise both integer precision cliffs and the FP poison
       domain routed to the helpers (NaN/Inf/out-of-range are never folded). */
    CHECK((float)UINT64_C(16777217) == (float)UINT64_C(16777216),
          "f32 boundary reference should expose round-to-nearest at 2^24+1");
    CHECK((double)UINT64_C(9007199254740993) ==
          (double)UINT64_C(9007199254740992),
          "f64 boundary reference should expose round-to-nearest at 2^53+1");
    CHECK(legacy_hfp_trunc_i32(legacy_hfp_from_u32(INT32_MAX, false)) ==
              INT32_MAX &&
          legacy_hfp_trunc_i32(legacy_hfp_from_u32(UINT32_C(0x80000000), true)) ==
              INT32_MIN &&
          (uint32_t)legacy_hfp_trunc_i32(
              legacy_hfp_from_u32(UINT32_MAX, false)) == UINT32_MAX,
          "legacy HFP software conversion must preserve signed/unsigned i32 limits");
    CHECK(legacy_hfp_trunc_i32(UINT64_C(0x4118000000000000)) == 1 &&
          legacy_hfp_trunc_i32(UINT64_C(0xc118000000000000)) == -1,
          "legacy HFP-to-integer model must truncate both signs toward zero");

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_mir_func_t *mir = anvil_mir_func_create("numeric_casts");
        anvil_mir_vreg_t si = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, cases[c].int_bits, true);
        anvil_mir_vreg_t ui = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, cases[c].int_bits, false);
        anvil_mir_vreg_t f32 = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_FPR, 32, true);
        anvil_mir_vreg_t f64 = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_FPR, 64, true);
        anvil_mir_vreg_t to_f32 = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_FPR, 32, true);
        anvil_mir_vreg_t to_f64 = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_FPR, 64, true);
        anvil_mir_vreg_t to_si = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, cases[c].int_bits, true);
        anvil_mir_vreg_t to_ui = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_GPR, cases[c].int_bits, false);
        anvil_mir_vreg_t ext = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_FPR, 64, true);
        anvil_mir_vreg_t trunc = anvil_mir_add_vreg_typed(
            mir, ANVIL_MIR_REG_FPR, 32, true);
        anvil_mir_vreg_t use_si[] = { si }, use_ui[] = { ui };
        anvil_mir_vreg_t use_f32[] = { f32 }, use_f64[] = { f64 };
        CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, si,
                                      cases[c].int_bits == 64
                                          ? INT64_MIN : INT32_MIN) &&
              anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, ui, -1) &&
              anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, f32, 0x7f800000) &&
              anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, f64,
                                      INT64_C(0x7ff8000000000001)),
              "numeric conversion boundary sources should be defined");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_SITOFP, to_f32, use_si, 1) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_UITOFP, to_f64, use_ui, 1) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_FPTOSI, to_si, use_f64, 1) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_FPTOUI, to_ui, use_f32, 1) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_FPEXT, ext, use_f32, 1) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_FPTRUNC, trunc, use_f64, 1),
              "all numeric conversion classes should be represented");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "numeric conversion MIR should terminate");
        char error[192] = { 0 };
        CHECK(anvil_mainframe_verify_mir_legal(mir, cases[c].variant,
                                                error, sizeof(error)),
              "well-typed numeric conversions should pass the legalizer");
        CHECK(anvil_mainframe_regalloc_mir(mir, cases[c].variant),
              "numeric conversion MIR should allocate");
        char *text = NULL;
        size_t len = 0;
        CHECK(anvil_mainframe_emit_mir(mir, cases[c].variant, &text, &len),
              "numeric conversion MIR should emit real ISA sequences");
        if (text) {
            check_contains(text, cases[c].sitofp,
                           "SITOFP must use the target's real HFP/BFP instruction");
            check_contains(text, cases[c].fptosi,
                           "FPTOSI must use a real truncating target instruction");
            check_contains(text, cases[c].fpext,
                           "FPEXT must use lengthen conversion, not a bit copy");
            check_contains(text, cases[c].fptrunc,
                           "FPTRUNC must use rounded shortening, not a bit copy");
            check_contains(text, cases[c].int_bits == 64
                                    ? "=X'8000000000000000'"
                                    : "=X'80000000'",
                           "unsigned FP-to-int must split around the signed boundary");
            CHECK(count_occurrences(text, "BALR  R14,R15") == 0,
                  "numeric conversions must not depend on unresolved helper symbols");
            CHECK(strstr(text, "requires target-specific runtime support") == NULL,
                  "numeric conversion placeholder comments must be gone");
            free(text);
        }
        anvil_mir_func_destroy(mir);
    }

    anvil_mir_func_t *bad = anvil_mir_func_create("bad_numeric_cast");
    anvil_mir_vreg_t g0 = anvil_mir_add_vreg_ex(bad, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t g1 = anvil_mir_add_vreg_ex(bad, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t bad_use[] = { g0 };
    CHECK(anvil_mir_add_instr_imm(bad, ANVIL_MIR_OP_MOV, g0, 1) &&
          anvil_mir_add_instr(bad, ANVIL_MIR_OP_SITOFP, g1, bad_use, 1) &&
          anvil_mir_add_instr(bad, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, NULL, 0),
          "malformed numeric cast should be representable by generic MIR");
    char bad_error[192] = { 0 };
    CHECK(!anvil_mainframe_verify_mir_legal(
              bad, ANVIL_MAINFRAME_VARIANT_S390, bad_error, sizeof(bad_error)) &&
          (strstr(bad_error, "invalid numeric conversion") != NULL ||
           strstr(bad_error, "invalid cast widths or classes") != NULL),
          "mainframe legalizer must reject invalid numeric cast classes");
    anvil_mir_func_destroy(bad);
}

static void test_mainframe_phi_swap_uses_parallel_copy(void)
{
    anvil_arch_t arches[] = {
        ANVIL_ARCH_S370, ANVIL_ARCH_S370_XA, ANVIL_ARCH_S390, ANVIL_ARCH_ZARCH,
    };
    anvil_mainframe_variant_t variants[] = {
        ANVIL_MAINFRAME_VARIANT_S370, ANVIL_MAINFRAME_VARIANT_S370_XA,
        ANVIL_MAINFRAME_VARIANT_S390, ANVIL_MAINFRAME_VARIANT_ZARCH,
    };
    for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
        anvil_ctx_t *ctx = new_mainframe_ctx(arches[v]);
        if (!ctx) continue;
        anvil_module_t *mod = anvil_module_create(ctx, "mainframe_phi_swap");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32 };
        anvil_func_t *fn = anvil_func_create(
            mod, "phi_swap", anvil_type_func(ctx, i32, params, 1, false),
            ANVIL_LINK_EXTERNAL);
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *header = anvil_block_create(fn, "header");
        anvil_block_t *body = anvil_block_create(fn, "body");
        anvil_block_t *exit_block = anvil_block_create(fn, "exit");
        anvil_set_insert_point(ctx, entry);
        anvil_build_br(ctx, header);
        anvil_set_insert_point(ctx, header);
        anvil_value_t *a = anvil_build_phi(ctx, i32, "a");
        anvil_value_t *b = anvil_build_phi(ctx, i32, "b");
        anvil_phi_add_incoming(a, anvil_const_i32(ctx, 1), entry);
        anvil_phi_add_incoming(a, b, body);
        anvil_phi_add_incoming(b, anvil_const_i32(ctx, 2), entry);
        anvil_phi_add_incoming(b, a, body);
        anvil_value_t *again = anvil_build_cmp_ne(
            ctx, anvil_func_get_param(fn, 0), anvil_const_i32(ctx, 0), "again");
        anvil_build_br_cond(ctx, again, body, exit_block);
        anvil_set_insert_point(ctx, body);
        anvil_build_br(ctx, header);
        anvil_set_insert_point(ctx, exit_block);
        anvil_build_ret(ctx, anvil_build_add(ctx, a, b, "sum"));

        char source_error[256] = { 0 };
        CHECK(anvil_module_verify(mod, source_error, sizeof(source_error)),
              source_error[0] ? source_error : "PHI swap source IR should verify");
        anvil_mir_func_t *mir = anvil_mainframe_lower_func_to_mir(fn, variants[v]);
        CHECK(mir != NULL, "mainframe PHI cycle should lower on every variant");
        if (mir) {
            anvil_mir_block_t mir_body = ANVIL_MIR_NO_BLOCK;
            for (size_t bidx = 0; bidx < anvil_mir_num_blocks(mir); bidx++) {
                anvil_mir_block_info_t info;
                anvil_mir_get_block_info(mir, (anvil_mir_block_t)bidx, &info);
                if (info.name && strncmp(info.name, "body_to_header_phi_", strlen("body_to_header_phi_")) == 0) {
                    mir_body = (anvil_mir_block_t)bidx;
                }
            }
            size_t body_copies = 0;
            for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                anvil_mir_instr_info_t info;
                anvil_mir_get_instr_info(mir, i, &info);
                if (info.block == mir_body && info.op == ANVIL_MIR_OP_COPY) {
                    body_copies++;
                }
            }
            CHECK(body_copies == 3,
                  "two-node PHI cycle must lower to save-temp plus two copies");
            CHECK(anvil_mainframe_regalloc_mir(mir, variants[v]),
                  "parallel-copy PHI cycle should survive register allocation");
            anvil_mir_func_destroy(mir);
        }
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

static void test_zarch_shared_phi_literal_is_entry_dominated(void)
{
    anvil_ctx_t *ctx = new_mainframe_ctx(ANVIL_ARCH_ZARCH);
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "zarch_phi_literal_dom");
    anvil_type_t *i1 = anvil_type_i1(ctx);
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *params[] = { i1 };
    anvil_func_t *fn = anvil_func_create(
        mod, "shared_phi_literal",
        anvil_type_func(ctx, i64, params, 1u, false),
        ANVIL_LINK_EXTERNAL);
    CHECK(mod && fn, "shared-PHI-literal source function should be created");
    if (mod && fn) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *left = anvil_block_create(fn, "left");
        anvil_block_t *right = anvil_block_create(fn, "right");
        anvil_block_t *join = anvil_block_create(fn, "join");
        anvil_value_t *shared = anvil_const_i64(ctx, 0);

        anvil_set_insert_point(ctx, entry);
        CHECK(anvil_build_br_cond(ctx, anvil_func_get_param(fn, 0),
                                  left, right),
              "shared-PHI-literal entry should branch");
        anvil_set_insert_point(ctx, left);
        CHECK(anvil_build_br(ctx, join),
              "shared-PHI-literal left edge should branch to join");
        anvil_set_insert_point(ctx, right);
        CHECK(anvil_build_br(ctx, join),
              "shared-PHI-literal right edge should branch to join");
        anvil_set_insert_point(ctx, join);
        anvil_value_t *phi = anvil_build_phi(ctx, i64, "shared");
        CHECK(phi && shared && anvil_phi_add_incoming(phi, shared, left) &&
              anvil_phi_add_incoming(phi, shared, right) &&
              anvil_build_ret(ctx, phi),
              "both PHI edges should consume the same IR literal object");

        char source_error[256] = { 0 };
        CHECK(anvil_module_verify(mod, source_error, sizeof(source_error)),
              source_error[0] ? source_error
                              : "shared-PHI-literal source IR should verify");
        anvil_mir_func_t *mir = anvil_mainframe_lower_func_to_mir(
            fn, ANVIL_MAINFRAME_VARIANT_ZARCH);
        CHECK(mir != NULL,
              "shared literal used by disjoint PHI edges must dominate both edges");
        if (mir) {
            bool saw_entry_i64_zero = false;
            for (size_t index = 0u; index < anvil_mir_num_instrs(mir);
                 index++) {
                anvil_mir_instr_info_t info;
                if (!anvil_mir_get_instr_info(mir, index, &info)) continue;
                const anvil_mir_vreg_info_t *vreg =
                    info.def == ANVIL_MIR_NO_VREG ? NULL
                    : anvil_mir_get_vreg_info(mir, info.def);
                if (info.op == ANVIL_MIR_OP_MOV && info.block == 0u &&
                    info.has_imm && info.imm == 0 && vreg &&
                    vreg->reg_class == ANVIL_MIR_REG_GPR &&
                    vreg->size_bits == 64u) {
                    saw_entry_i64_zero = true;
                }
            }
            CHECK(saw_entry_i64_zero,
                  "shared PHI literal must be materialized in the entry block");
            CHECK(anvil_mainframe_regalloc_mir(
                      mir, ANVIL_MAINFRAME_VARIANT_ZARCH),
                  "entry-dominated PHI literal should survive register allocation");
            char *text = NULL;
            size_t length = 0u;
            CHECK(anvil_mainframe_emit_mir(
                      mir, ANVIL_MAINFRAME_VARIANT_ZARCH, &text, &length),
                  "entry-dominated PHI literal should emit complete HLASM");
            CHECK(text != NULL && length != 0u,
                  "entry-dominated PHI literal should produce nonempty HLASM");
            free(text);
            anvil_mir_func_destroy(mir);
        }
    }

    anvil_module_destroy(mod);
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

static void test_mainframe_emits_interleaved_block_ownership_correctly(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("mf_interleaved");
    CHECK(mir != NULL, "interleaved mainframe MIR should be created");
    if (!mir) return;
    anvil_mir_block_t entry = anvil_mir_current_block(mir);
    anvil_mir_block_t then_block = anvil_mir_add_block(mir, "then");
    anvil_mir_block_t else_block = anvil_mir_add_block(mir, "else");
    anvil_mir_vreg_t cond = anvil_mir_add_vreg_ex(
        mir, ANVIL_MIR_REG_GPR, 8);
    CHECK(anvil_mir_set_current_block(mir, entry) &&
          anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, cond, 1) &&
          anvil_mir_set_current_block(mir, then_block) &&
          anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, NULL, 0) &&
          anvil_mir_set_current_block(mir, entry) &&
          anvil_mir_add_cond_branch(mir, cond, then_block, else_block) &&
          anvil_mir_set_current_block(mir, else_block) &&
          anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, NULL, 0),
          "mainframe MIR should permit valid per-block order with interleaved storage");
    CHECK(anvil_mainframe_regalloc_mir(mir, ANVIL_MAINFRAME_VARIANT_S390),
          "interleaved mainframe MIR should allocate");
    char *text = NULL;
    size_t len = 0;
    CHECK(anvil_mainframe_emit_mir(mir, ANVIL_MAINFRAME_VARIANT_S390,
                                   &text, &len),
          "interleaved mainframe MIR should emit");
    if (text) {
        const char *branch = strstr(text, "         BNE   MF_INTERLEAVED_THEN");
        const char *then_label = branch
            ? strstr(branch + strlen("         BNE   MF_INTERLEAVED_THEN"),
                     "MF_INTERLEAVED_THEN") : NULL;
        CHECK(branch && then_label && branch < then_label,
              "mainframe emitter must select instructions by block, not a false contiguous range");
        free(text);
    }
    anvil_mir_func_destroy(mir);
}

static void test_mainframe_internal_labels_do_not_truncate(void)
{
    char name[96];
    memset(name, 'a', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    anvil_mir_func_t *mir = anvil_mir_func_create(name);
    CHECK(mir != NULL, "maximum supported mainframe name should create MIR");
    if (mir) {
        anvil_mir_vreg_t a = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t b = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t result = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 8);
        anvil_mir_vreg_t uses[] = { a, b };
        CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, a, 1) &&
              anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, b, 2) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_CMP_EQ, result, uses, 2) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "long-name mainframe MIR stream should build");
        CHECK(anvil_mainframe_regalloc_mir(mir, ANVIL_MAINFRAME_VARIANT_S390),
              "long-name mainframe MIR should allocate");
        char *text = NULL;
        size_t len = 0;
        CHECK(anvil_mainframe_emit_mir(mir, ANVIL_MAINFRAME_VARIANT_S390,
                                       &text, &len),
              "95-character mainframe name should emit without truncating internals");
        CHECK(text && strstr(text, "_CMP_T_0"),
              "long internal compare label suffix must remain intact");
        free(text);
        anvil_mir_func_destroy(mir);
    }

    char too_long[97];
    memset(too_long, 'b', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    mir = anvil_mir_func_create(too_long);
    if (mir) {
        anvil_mir_vreg_t live = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_GPR, 32);
        CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, live, 0) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_KEEPALIVE,
                                  ANVIL_MIR_NO_VREG, &live, 1) &&
              anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "overlong-name MIR should build structurally");
        CHECK(anvil_mainframe_regalloc_mir(mir, ANVIL_MAINFRAME_VARIANT_S390),
              "overlong-name MIR should allocate before emitter name validation");
        char *text = NULL;
        size_t len = 0;
        CHECK(!anvil_mainframe_emit_mir(mir, ANVIL_MAINFRAME_VARIANT_S390,
                                        &text, &len) && !text && len == 0,
              "mainframe emitter must reject rather than truncate an overlong symbol");
        anvil_mir_func_destroy(mir);
    }
}

static void test_mainframe_string_literals_preserve_ir_bytes(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("string_bytes");
    const char *label = NULL;
    CHECK(mir && anvil_mir_add_string_literal(mir, "A\n\200", &label) >= 0 &&
          label && anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                       ANVIL_MIR_NO_VREG, NULL, 0),
          "mainframe byte-exact string MIR should construct");
    if (!mir) return;
    CHECK(anvil_mainframe_regalloc_mir(mir, ANVIL_MAINFRAME_VARIANT_S390),
          "mainframe byte-exact string MIR should allocate");
    char *text = NULL;
    size_t len = 0;
    CHECK(anvil_mainframe_emit_mir(mir, ANVIL_MAINFRAME_VARIANT_S390,
                                   &text, &len),
          "mainframe byte-exact string MIR should emit");
    CHECK(text && strstr(text, "DC    X'410A8000'"),
          "mainframe string literals must retain control/high bytes and NUL");
    free(text);
    anvil_mir_func_destroy(mir);
}

int main(void)
{
    test_mainframe_frame_allocation_separates_nested_calls();
    test_mainframe_materializes_call_values_before_bundle();
    test_mainframe_dynamic_alloca_updates_stack_chain();
    test_mainframe_narrow_loads_extend_exactly();
    test_mainframe_unsigned_divmod_and_not64_are_real();
    test_mainframe_numeric_conversions_use_target_isa();
    test_mainframe_phi_swap_uses_parallel_copy();
    test_zarch_shared_phi_literal_is_entry_dominated();
    test_mainframe_fp_parameters_use_gpr_address_scratch();
    test_s390_call_bundles_layout_f64_and_mark_each_call();
    test_zarch_spilled_call_arguments_preserve_bundle();
    test_mainframe_rejects_malformed_call_bundle();
    test_mainframe_descriptors_model_real_variants();
    test_s370_lowers_i32_add_through_mvs_parameter_list();
    test_zarch_lowers_and_emits_64bit_hlasm();
    test_s370_constant_gep_index_does_not_create_64bit_gpr();
    test_s370_global_array_address_is_pointer_sized();
    test_s370_i64_constant_store_uses_big_endian_word_halves();
    test_hlasm_control_text_is_uppercase();
    test_fp_format_selection_is_target_honest();
    test_decimal_types_and_hlasm_constants_are_first_class();
    test_mainframe_emits_interleaved_block_ownership_correctly();
    test_mainframe_internal_labels_do_not_truncate();
    test_mainframe_string_literals_preserve_ir_bytes();

    if (failures != 0) {
        fprintf(stderr, "%d mainframe MachineIR regression checks failed\n", failures);
        return 1;
    }

    printf("mainframe MachineIR regression checks passed\n");
    return 0;
}
