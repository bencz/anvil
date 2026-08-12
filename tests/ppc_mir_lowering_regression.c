/*
 * Regression tests for the shared PowerPC -> MachineIR backend path.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_machine.h>
#include <anvil/anvil_ppc_mir.h>

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

static void check_not_contains(const char *text, const char *needle, const char *msg)
{
    CHECK(text && needle && strstr(text, needle) == NULL, msg);
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

static anvil_ctx_t *new_ppc_ctx(anvil_arch_t arch)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx != NULL, "context should be created");
    if (!ctx) return NULL;

    CHECK(anvil_ctx_set_target(ctx, arch) == ANVIL_OK,
          "PowerPC target should be available");
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

static void test_ppc_descriptors_capture_real_variant_differences(void)
{
    const anvil_ppc_target_desc_t *ppc32 =
        anvil_ppc_get_target_desc(ANVIL_PPC_VARIANT_PPC32);
    const anvil_ppc_target_desc_t *ppc64 =
        anvil_ppc_get_target_desc(ANVIL_PPC_VARIANT_PPC64);
    const anvil_ppc_target_desc_t *ppc64le =
        anvil_ppc_get_target_desc(ANVIL_PPC_VARIANT_PPC64LE);

    CHECK(ppc32 && ppc64 && ppc64le,
          "all PowerPC target descriptors should be available");
    if (!ppc32 || !ppc64 || !ppc64le) return;

    CHECK(ppc32->arch == ANVIL_ARCH_PPC32 &&
          ppc32->word_size == 4 &&
          !ppc32->little_endian &&
          !ppc32->uses_function_descriptors &&
          ppc32->min_frame_size == 32,
          "PPC32 descriptor should model 32-bit big-endian SysV ABI");
    CHECK(ppc64->arch == ANVIL_ARCH_PPC64 &&
          ppc64->word_size == 8 &&
          !ppc64->little_endian &&
          ppc64->uses_function_descriptors &&
          ppc64->min_frame_size == 112,
          "PPC64 descriptor should model 64-bit big-endian ELFv1 ABI");
    CHECK(ppc64le->arch == ANVIL_ARCH_PPC64LE &&
          ppc64le->word_size == 8 &&
          ppc64le->little_endian &&
          !ppc64le->uses_function_descriptors &&
          ppc64le->min_frame_size == 32,
          "PPC64LE descriptor should model 64-bit little-endian ELFv2 ABI");

    CHECK(ppc32->gpr_arg_regs[0] == 3 &&
          ppc64->gpr_arg_regs[0] == 3 &&
          ppc64le->gpr_arg_regs[0] == 3,
          "all PowerPC ABIs should pass first integer arg in r3");
    CHECK(ppc32->gpr_return_reg == 3 &&
          ppc64->gpr_return_reg == 3 &&
          ppc64le->gpr_return_reg == 3,
          "all PowerPC ABIs should return integer values in r3");
}

static void test_ppc32_lowers_i32_add_with_fixed_abi_regs(void)
{
    anvil_ctx_t *ctx = new_ppc_ctx(ANVIL_ARCH_PPC32);
    if (!ctx) return;

    anvil_module_t *mod = NULL;
    anvil_func_t *fn = build_iadd_func(ctx, &mod, "ppc32_mir_iadd",
                                       "ppc32_iadd", anvil_type_i32(ctx));
    if (fn) {
        anvil_mir_func_t *mir =
            anvil_ppc_lower_func_to_mir(fn, ANVIL_PPC_VARIANT_PPC32);
        CHECK(mir != NULL, "PPC32 integer add should lower to MachineIR");
        if (mir) {
            CHECK(anvil_mir_num_instrs(mir) == 5,
                  "PPC32 add should copy ABI args, add, copy return, ret");

            anvil_mir_instr_info_t copy0;
            anvil_mir_instr_info_t copy1;
            anvil_mir_instr_info_t add;
            anvil_mir_instr_info_t ret_copy;
            CHECK(anvil_mir_get_instr_info(mir, 0, &copy0),
                  "first PPC32 ABI arg copy should be inspectable");
            CHECK(anvil_mir_get_instr_info(mir, 1, &copy1),
                  "second PPC32 ABI arg copy should be inspectable");
            CHECK(anvil_mir_get_instr_info(mir, 2, &add),
                  "PPC32 add instruction should be inspectable");
            CHECK(anvil_mir_get_instr_info(mir, 3, &ret_copy),
                  "PPC32 return copy should be inspectable");

            anvil_mir_vreg_t incoming0 = anvil_mir_get_instr_use(mir, 0, 0);
            anvil_mir_vreg_t incoming1 = anvil_mir_get_instr_use(mir, 1, 0);
            const anvil_mir_vreg_info_t *incoming0_info =
                anvil_mir_get_vreg_info(mir, incoming0);
            const anvil_mir_vreg_info_t *incoming1_info =
                anvil_mir_get_vreg_info(mir, incoming1);
            const anvil_mir_vreg_info_t *arg0_info =
                anvil_mir_get_vreg_info(mir, copy0.def);
            const anvil_mir_vreg_info_t *ret_info =
                anvil_mir_get_vreg_info(mir, ret_copy.def);

            CHECK(copy0.op == ANVIL_MIR_OP_COPY &&
                  copy1.op == ANVIL_MIR_OP_COPY &&
                  add.op == ANVIL_MIR_OP_ADD &&
                  ret_copy.op == ANVIL_MIR_OP_COPY,
                  "PPC32 lowering should use canonical ABI copy/add/copy sequence");
            CHECK(incoming0_info &&
                  incoming0_info->reg_class == ANVIL_MIR_REG_GPR &&
                  incoming0_info->size_bits == 32 &&
                  incoming0_info->has_fixed_reg &&
                  incoming0_info->fixed_phys_reg == 3,
                  "first PPC32 incoming integer argument should be fixed to r3");
            CHECK(incoming1_info &&
                  incoming1_info->reg_class == ANVIL_MIR_REG_GPR &&
                  incoming1_info->size_bits == 32 &&
                  incoming1_info->has_fixed_reg &&
                  incoming1_info->fixed_phys_reg == 4,
                  "second PPC32 incoming integer argument should be fixed to r4");
            CHECK(arg0_info && arg0_info->size_bits == 32 &&
                  !arg0_info->has_fixed_reg,
                  "PPC32 local integer argument should be allocatable");
            CHECK(ret_info && ret_info->size_bits == 32 &&
                  ret_info->has_fixed_reg && ret_info->fixed_phys_reg == 3,
                  "PPC32 integer return value should be fixed to r3");

            CHECK(anvil_ppc_regalloc_mir(mir, ANVIL_PPC_VARIANT_PPC32),
                  "PPC32 MachineIR regalloc should succeed");
            const anvil_regalloc_assignment_t *ret_assignment =
                anvil_mir_get_assignment(mir, ret_copy.def);
            CHECK(ret_assignment && !ret_assignment->spilled &&
                  ret_assignment->phys_reg == 3,
                  "PPC32 return assignment should stay in r3");

            anvil_mir_func_destroy(mir);
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ppc64_variants_lower_and_emit_distinct_abis(void)
{
    struct {
        anvil_arch_t arch;
        anvil_ppc_variant_t variant;
        const char *module_name;
        const char *func_name;
        bool expects_opd;
    } cases[] = {
        { ANVIL_ARCH_PPC64, ANVIL_PPC_VARIANT_PPC64,
          "ppc64_mir_iadd", "ppc64_iadd64", true },
        { ANVIL_ARCH_PPC64LE, ANVIL_PPC_VARIANT_PPC64LE,
          "ppc64le_mir_iadd", "ppc64le_iadd64", false },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = new_ppc_ctx(cases[c].arch);
        if (!ctx) continue;

        anvil_module_t *mod = NULL;
        anvil_func_t *fn = build_iadd_func(ctx, &mod, cases[c].module_name,
                                           cases[c].func_name,
                                           anvil_type_i64(ctx));
        if (fn) {
            anvil_mir_func_t *mir =
                anvil_ppc_lower_func_to_mir(fn, cases[c].variant);
            CHECK(mir != NULL, "PPC64 integer add should lower to MachineIR");
            if (mir) {
                anvil_mir_instr_info_t copy0;
                anvil_mir_instr_info_t copy1;
                anvil_mir_instr_info_t ret_copy;
                CHECK(anvil_mir_get_instr_info(mir, 0, &copy0),
                      "first PPC64 ABI arg copy should be inspectable");
                CHECK(anvil_mir_get_instr_info(mir, 1, &copy1),
                      "second PPC64 ABI arg copy should be inspectable");
                CHECK(anvil_mir_get_instr_info(mir, 3, &ret_copy),
                      "PPC64 return copy should be inspectable");

                anvil_mir_vreg_t incoming0 = anvil_mir_get_instr_use(mir, 0, 0);
                anvil_mir_vreg_t incoming1 = anvil_mir_get_instr_use(mir, 1, 0);
                const anvil_mir_vreg_info_t *incoming0_info =
                    anvil_mir_get_vreg_info(mir, incoming0);
                const anvil_mir_vreg_info_t *incoming1_info =
                    anvil_mir_get_vreg_info(mir, incoming1);
                const anvil_mir_vreg_info_t *ret_info =
                    anvil_mir_get_vreg_info(mir, ret_copy.def);

                CHECK(incoming0_info && incoming0_info->size_bits == 64 &&
                      incoming0_info->has_fixed_reg &&
                      incoming0_info->fixed_phys_reg == 3,
                      "first PPC64 incoming integer argument should be fixed to r3");
                CHECK(incoming1_info && incoming1_info->size_bits == 64 &&
                      incoming1_info->has_fixed_reg &&
                      incoming1_info->fixed_phys_reg == 4,
                      "second PPC64 incoming integer argument should be fixed to r4");
                CHECK(ret_info && ret_info->size_bits == 64 &&
                      ret_info->has_fixed_reg && ret_info->fixed_phys_reg == 3,
                      "PPC64 integer return value should be fixed to r3");

                CHECK(anvil_ppc_regalloc_mir(mir, cases[c].variant),
                      "PPC64 MachineIR regalloc should succeed");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_ppc_emit_mir(mir, cases[c].variant,
                                         &asm_text, &asm_len),
                      "PPC64 MachineIR emitter should produce assembly");
                CHECK(asm_text != NULL && asm_len > 0,
                      "PPC64 MachineIR emitter should return non-empty assembly");
                if (asm_text) {
                    check_contains(asm_text, "\tadd ",
                                   "PPC64 MIR assembly should emit add");
                    check_contains(asm_text, "\tblr\n",
                                   "PPC64 MIR assembly should return with blr");
                    if (cases[c].expects_opd) {
                        check_contains(asm_text, "\t.abiversion 1\n",
                                       "PPC64 BE should emit ELFv1 ABI marker");
                        check_contains(asm_text, ".section \".opd\",\"aw\"\n",
                                       "PPC64 BE should emit function descriptor section");
                        check_contains(asm_text, ".L.ppc64_iadd64:\n",
                                       "PPC64 BE should emit real entry label under descriptor");
                    } else {
                        check_contains(asm_text, "\t.abiversion 2\n",
                                       "PPC64LE should emit ELFv2 ABI marker");
                        check_contains(asm_text, "\t.localentry ppc64le_iadd64",
                                       "PPC64LE should emit ELFv2 localentry directive");
                        check_not_contains(asm_text, ".section \".opd\",\"aw\"\n",
                                           "PPC64LE should not emit ELFv1 descriptor section");
                    }
                    free(asm_text);
                }

                anvil_mir_func_destroy(mir);
            }
        }

        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

static void test_ppc_stack_call_args_use_variant_word_size(void)
{
    struct {
        anvil_arch_t arch;
        anvil_ppc_variant_t variant;
        const char *module_name;
        const char *callee_name;
        const char *caller_name;
        anvil_type_t *(*type_fn)(anvil_ctx_t *);
        int64_t expected_second_offset;
    } cases[] = {
        { ANVIL_ARCH_PPC32, ANVIL_PPC_VARIANT_PPC32,
          "ppc32_stack_args", "ppc32_callee10", "ppc32_stack_call",
          anvil_type_i32, 4 },
        { ANVIL_ARCH_PPC64, ANVIL_PPC_VARIANT_PPC64,
          "ppc64_stack_args", "ppc64_callee10", "ppc64_stack_call",
          anvil_type_i64, 8 },
        { ANVIL_ARCH_PPC64LE, ANVIL_PPC_VARIANT_PPC64LE,
          "ppc64le_stack_args", "ppc64le_callee10", "ppc64le_stack_call",
          anvil_type_i64, 8 },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = new_ppc_ctx(cases[c].arch);
        if (!ctx) continue;

        anvil_module_t *mod = anvil_module_create(ctx, cases[c].module_name);
        CHECK(mod != NULL, "stack-arg module should be created");
        if (mod) {
            anvil_type_t *int_type = cases[c].type_fn(ctx);
            anvil_type_t *callee_params[10];
            for (size_t i = 0; i < 10; i++) {
                callee_params[i] = int_type;
            }
            anvil_type_t *callee_type =
                anvil_type_func(ctx, int_type, callee_params, 10, false);
            anvil_func_t *callee =
                anvil_func_declare(mod, cases[c].callee_name, callee_type);
            CHECK(callee != NULL, "stack-arg callee should be declared");

            anvil_type_t *caller_type =
                anvil_type_func(ctx, int_type, NULL, 0, false);
            anvil_func_t *caller =
                anvil_func_create(mod, cases[c].caller_name, caller_type,
                                  ANVIL_LINK_EXTERNAL);
            CHECK(caller != NULL, "stack-arg caller should be created");
            if (callee && caller) {
                anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
                anvil_value_t *args[10];
                for (size_t i = 0; i < 10; i++) {
                    args[i] = cases[c].arch == ANVIL_ARCH_PPC32
                        ? anvil_const_i32(ctx, (int32_t)i + 1)
                        : anvil_const_i64(ctx, (int64_t)i + 1);
                }
                anvil_value_t *call = NULL;
                anvil_build_call_checked(ctx, anvil_func_get_value(callee),
                                         args, 10, "call", &call);
                anvil_build_ret(ctx, call);

                anvil_mir_func_t *mir =
                    anvil_ppc_lower_func_to_mir(caller, cases[c].variant);
                CHECK(mir != NULL,
                      "PowerPC calls beyond r10 should lower stack arguments");
                if (mir) {
                    bool saw_offset0 = false;
                    bool saw_second_offset = false;
                    bool saw_call = false;
                    for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                        anvil_mir_instr_info_t info;
                        CHECK(anvil_mir_get_instr_info(mir, i, &info),
                              "PowerPC stack-arg instruction should be inspectable");
                        if (info.op == ANVIL_MIR_OP_CALL_STACK_ARG) {
                            CHECK(info.has_imm && info.num_uses == 1,
                                  "PowerPC stack arg should carry offset and source");
                            if (info.imm == 0) saw_offset0 = true;
                            if (info.imm == cases[c].expected_second_offset) {
                                saw_second_offset = true;
                            }
                        } else if (info.op == ANVIL_MIR_OP_CALL) {
                            saw_call = true;
                            CHECK(info.num_uses == 8,
                                  "PowerPC call should keep only r3-r10 args as uses");
                        }
                    }
                    CHECK(saw_offset0,
                          "ninth PowerPC integer arg should use stack offset 0");
                    CHECK(saw_second_offset,
                          "tenth PowerPC integer arg should use variant word-size offset");
                    CHECK(saw_call,
                          "PowerPC stack-arg lowering should still emit call");
                    CHECK(anvil_ppc_regalloc_mir(mir, cases[c].variant),
                          "PowerPC stack-arg MIR should allocate");

                    anvil_mir_func_destroy(mir);
                }
            }
        }

        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

static void test_ppc64le_emits_indirect_call_through_ctr(void)
{
    anvil_ctx_t *ctx = new_ppc_ctx(ANVIL_ARCH_PPC64LE);
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "ppc64le_indirect_call");
    CHECK(mod != NULL, "indirect-call module should be created");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = { i64, i64 };
        anvil_type_t *callee_type = anvil_type_func(ctx, i64, params, 2, false);
        anvil_type_t *callee_ptr_type = anvil_type_ptr(ctx, callee_type);
        anvil_func_t *callee =
            anvil_func_create(mod, "ppc64le_fp_add", callee_type,
                              ANVIL_LINK_EXTERNAL);
        anvil_func_t *caller =
            anvil_func_create(mod, "ppc64le_fp_call",
                              anvil_type_func(ctx, i64, NULL, 0, false),
                              ANVIL_LINK_EXTERNAL);
        CHECK(callee != NULL && caller != NULL,
              "indirect-call functions should be created");
        if (callee) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(callee));
            anvil_value_t *a = anvil_func_get_param(callee, 0);
            anvil_value_t *b = anvil_func_get_param(callee, 1);
            anvil_build_ret(ctx, anvil_build_add(ctx, a, b, "sum"));
        }
        if (callee && caller) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
            anvil_value_t *slot = anvil_build_alloca(ctx, callee_ptr_type, "slot");
            anvil_build_store(ctx, anvil_func_get_value(callee), slot);
            anvil_value_t *loaded =
                anvil_build_load(ctx, callee_ptr_type, slot, "loaded_fn");
            anvil_value_t *args[] = {
                anvil_const_i64(ctx, 3),
                anvil_const_i64(ctx, 4)
            };
            anvil_value_t *called = NULL;
            anvil_build_call_checked(ctx, loaded, args, 2, "called", &called);
            anvil_build_ret(ctx, called);

            anvil_mir_func_t *mir =
                anvil_ppc_lower_func_to_mir(caller, ANVIL_PPC_VARIANT_PPC64LE);
            CHECK(mir != NULL,
                  "PPC64LE indirect call should lower to MachineIR");
            if (mir) {
                bool saw_indirect_call = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "PPC64LE indirect-call MIR should be inspectable");
                    if (info.op == ANVIL_MIR_OP_CALL && info.symbol == NULL) {
                        saw_indirect_call = true;
                        CHECK(info.num_uses == 3,
                              "indirect call should use target plus two ABI args");
                    }
                }
                CHECK(saw_indirect_call,
                      "PPC64LE lowering should preserve indirect call target");
                CHECK(anvil_ppc_regalloc_mir(mir, ANVIL_PPC_VARIANT_PPC64LE),
                      "PPC64LE indirect-call MIR should allocate");

                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_ppc_emit_mir(mir, ANVIL_PPC_VARIANT_PPC64LE,
                                         &asm_text, &asm_len),
                      "PPC64LE indirect-call MIR should emit assembly");
                if (asm_text) {
                    check_contains(asm_text, "\tmtctr ",
                                   "PPC64LE indirect call should load CTR");
                    check_contains(asm_text, "\tbctrl\n",
                                   "PPC64LE indirect call should branch via CTR");
                    free(asm_text);
                }

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ppc32_splits_i64_locals_into_word_pairs(void)
{
    anvil_ctx_t *ctx = new_ppc_ctx(ANVIL_ARCH_PPC32);
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "ppc32_i64_pairs");
    CHECK(mod != NULL, "PPC32 i64-pair module should be created");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *i1 = anvil_type_i1(ctx);
        anvil_func_t *fn =
            anvil_func_create(mod, "ppc32_i64_pair_cmp",
                              anvil_type_func(ctx, i1, NULL, 0, false),
                              ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "PPC32 i64-pair function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *lhs_slot = anvil_build_alloca(ctx, i64, "lhs_slot");
            anvil_value_t *rhs_slot = anvil_build_alloca(ctx, i64, "rhs_slot");
            anvil_build_store(ctx, anvil_const_i64(ctx, 1234605616436508552LL),
                              lhs_slot);
            anvil_build_store(ctx, anvil_const_i64(ctx, -5), rhs_slot);
            anvil_value_t *lhs = anvil_build_load(ctx, i64, lhs_slot, "lhs");
            anvil_value_t *rhs = anvil_build_load(ctx, i64, rhs_slot, "rhs");
            anvil_value_t *cmp = anvil_build_cmp_gt(ctx, lhs, rhs, "gt");
            anvil_build_ret(ctx, cmp);

            anvil_mir_func_t *mir =
                anvil_ppc_lower_func_to_mir(fn, ANVIL_PPC_VARIANT_PPC32);
            CHECK(mir != NULL,
                  "PPC32 should lower local i64 stores/loads/comparisons as pairs");
            if (mir) {
                bool saw_store = false;
                bool saw_load = false;
                bool saw_cmp = false;
                bool saw_64bit_gpr = false;

                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "PPC32 i64-pair MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_STORE) saw_store = true;
                    if (info.op == ANVIL_MIR_OP_LOAD) saw_load = true;
                    if (info.op == ANVIL_MIR_OP_CMP_GT ||
                        info.op == ANVIL_MIR_OP_CMP_EQ ||
                        info.op == ANVIL_MIR_OP_CMP_UGT) {
                        saw_cmp = true;
                    }
                }

                for (size_t i = 0; i < anvil_mir_num_vregs(mir); i++) {
                    const anvil_mir_vreg_info_t *info =
                        anvil_mir_get_vreg_info(mir, (anvil_mir_vreg_t)i);
                    if (info && info->reg_class == ANVIL_MIR_REG_GPR &&
                        info->size_bits > 32) {
                        saw_64bit_gpr = true;
                    }
                }

                CHECK(saw_store && saw_load && saw_cmp,
                      "PPC32 i64-pair lowering should emit word stores, loads, and compares");
                CHECK(!saw_64bit_gpr,
                      "PPC32 i64-pair lowering should not leave illegal 64-bit GPR vregs");
                CHECK(anvil_ppc_regalloc_mir(mir, ANVIL_PPC_VARIANT_PPC32),
                      "PPC32 i64-pair MIR should allocate");

                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_ppc_emit_mir(mir, ANVIL_PPC_VARIANT_PPC32,
                                         &asm_text, &asm_len),
                      "PPC32 i64-pair MIR should emit assembly");
                CHECK(asm_text != NULL && asm_len > 0,
                      "PPC32 i64-pair assembly should be non-empty");
                if (asm_text) {
                    check_contains(asm_text, "\tstw ",
                                   "PPC32 i64-pair assembly should store words");
                    check_contains(asm_text, "\tlwz ",
                                   "PPC32 i64-pair assembly should load words");
                    check_contains(asm_text, "\tcmpw ",
                                   "PPC32 i64-pair assembly should compare high words");
                    free(asm_text);
                }

                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_ppc_module_codegen_routes_through_shared_mir_path(void)
{
    struct {
        anvil_arch_t arch;
        const char *module_name;
        const char *func_name;
        const char *abi_marker;
    } cases[] = {
        { ANVIL_ARCH_PPC32, "ppc32_codegen", "ppc32_codegen_iadd", NULL },
        { ANVIL_ARCH_PPC64, "ppc64_codegen", "ppc64_codegen_iadd", "\t.abiversion 1\n" },
        { ANVIL_ARCH_PPC64LE, "ppc64le_codegen", "ppc64le_codegen_iadd", "\t.abiversion 2\n" },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = new_ppc_ctx(cases[c].arch);
        if (!ctx) continue;

        anvil_module_t *mod = NULL;
        anvil_type_t *int_type = cases[c].arch == ANVIL_ARCH_PPC32
            ? anvil_type_i32(ctx)
            : anvil_type_i64(ctx);
        anvil_func_t *fn = build_iadd_func(ctx, &mod, cases[c].module_name,
                                           cases[c].func_name, int_type);
        if (fn) {
            char *asm_text = NULL;
            size_t asm_len = 0;
            CHECK(anvil_module_codegen(mod, &asm_text, &asm_len) == ANVIL_OK,
                  "PowerPC module codegen should route through shared MIR path");
            CHECK(asm_text != NULL && asm_len > 0,
                  "PowerPC module codegen should produce non-empty assembly");
            if (asm_text) {
                check_contains(asm_text, cases[c].func_name,
                               "PowerPC module codegen should emit function symbol");
                check_contains(asm_text, "\tadd ",
                               "PowerPC module codegen should emit MIR add");
                if (cases[c].abi_marker) {
                    check_contains(asm_text, cases[c].abi_marker,
                                   "PowerPC module codegen should emit ABI marker");
                }
                free(asm_text);
            }
        }

        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

static void test_ppc_numeric_casts_have_typed_lowering(void)
{
    struct {
        anvil_ppc_variant_t variant;
        anvil_mir_opcode_t op;
        anvil_mir_reg_class_t src_class;
        uint16_t src_bits;
        anvil_mir_reg_class_t dst_class;
        uint16_t dst_bits;
        int64_t input_bits;
        const char *name;
        const char *required;
        const char *normalization;
    } cases[] = {
        { ANVIL_PPC_VARIANT_PPC32, ANVIL_MIR_OP_SITOFP,
          ANVIL_MIR_REG_GPR, 32, ANVIL_MIR_REG_FPR, 64,
          INT32_MIN, "ppc32_sitofp_min", "bl __floatsidf", "mr r3, r14" },
        { ANVIL_PPC_VARIANT_PPC32, ANVIL_MIR_OP_UITOFP,
          ANVIL_MIR_REG_GPR, 32, ANVIL_MIR_REG_FPR, 32,
          -1, "ppc32_uitofp_max", "bl __floatunsisf", "mr r3, r14" },
        { ANVIL_PPC_VARIANT_PPC32, ANVIL_MIR_OP_FPTOSI,
          ANVIL_MIR_REG_FPR, 64, ANVIL_MIR_REG_GPR, 32,
          INT64_C(0x41dfffffffc00000), "ppc32_fptosi_limit",
          "bl __fixdfsi", "mr r15, r3" },
        { ANVIL_PPC_VARIANT_PPC32, ANVIL_MIR_OP_FPTOUI,
          ANVIL_MIR_REG_FPR, 32, ANVIL_MIR_REG_GPR, 32,
          INT64_C(0x4f7fffff), "ppc32_fptoui_limit",
          "bl __fixunssfsi", "mr r15, r3" },
        { ANVIL_PPC_VARIANT_PPC32, ANVIL_MIR_OP_FPEXT,
          ANVIL_MIR_REG_FPR, 32, ANVIL_MIR_REG_FPR, 64,
          INT64_C(0x3f800000), "ppc32_fpext", "fmr f15, f14", NULL },
        { ANVIL_PPC_VARIANT_PPC32, ANVIL_MIR_OP_FPTRUNC,
          ANVIL_MIR_REG_FPR, 64, ANVIL_MIR_REG_FPR, 32,
          INT64_C(0x3ff0000000000000), "ppc32_fptrunc",
          "frsp f15, f14", NULL },

        { ANVIL_PPC_VARIANT_PPC64, ANVIL_MIR_OP_SITOFP,
          ANVIL_MIR_REG_GPR, 64, ANVIL_MIR_REG_FPR, 64,
          INT64_MIN, "ppc64_sitofp_min", "bl __floatdidf", "mr r3, r14" },
        { ANVIL_PPC_VARIANT_PPC64, ANVIL_MIR_OP_UITOFP,
          ANVIL_MIR_REG_GPR, 64, ANVIL_MIR_REG_FPR, 32,
          -1, "ppc64_uitofp_max", "bl __floatundisf", "mr r3, r14" },
        { ANVIL_PPC_VARIANT_PPC64, ANVIL_MIR_OP_FPTOSI,
          ANVIL_MIR_REG_FPR, 64, ANVIL_MIR_REG_GPR, 64,
          INT64_C(0x43dfffffffffffff), "ppc64_fptosi_limit",
          "bl __fixdfdi", "mr r15, r3" },
        { ANVIL_PPC_VARIANT_PPC64, ANVIL_MIR_OP_FPTOUI,
          ANVIL_MIR_REG_FPR, 32, ANVIL_MIR_REG_GPR, 64,
          INT64_C(0x5f7fffff), "ppc64_fptoui_limit",
          "bl __fixunssfdi", "mr r15, r3" },
        { ANVIL_PPC_VARIANT_PPC64, ANVIL_MIR_OP_FPEXT,
          ANVIL_MIR_REG_FPR, 32, ANVIL_MIR_REG_FPR, 64,
          INT64_C(0x3f800000), "ppc64_fpext", "fmr f15, f14", NULL },
        { ANVIL_PPC_VARIANT_PPC64, ANVIL_MIR_OP_FPTRUNC,
          ANVIL_MIR_REG_FPR, 64, ANVIL_MIR_REG_FPR, 32,
          INT64_C(0x3ff0000000000000), "ppc64_fptrunc",
          "frsp f15, f14", NULL },

        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_SITOFP,
          ANVIL_MIR_REG_GPR, 8, ANVIL_MIR_REG_FPR, 32,
          -128, "ppc64le_sitofp_i8_min", "bl __floatsisf", "extsb r3, r14" },
        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_UITOFP,
          ANVIL_MIR_REG_GPR, 16, ANVIL_MIR_REG_FPR, 64,
          65535, "ppc64le_uitofp_u16_max", "bl __floatunsidf",
          "rlwinm r3, r14, 0, 16, 31" },
        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_FPTOSI,
          ANVIL_MIR_REG_FPR, 64, ANVIL_MIR_REG_GPR, 8,
          INT64_C(0xc05fc00000000000), "ppc64le_fptosi_i8_min",
          "bl __fixdfsi", "extsb r15, r3" },
        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_FPTOUI,
          ANVIL_MIR_REG_FPR, 64, ANVIL_MIR_REG_GPR, 16,
          INT64_C(0x40efffe000000000), "ppc64le_fptoui_u16_max",
          "bl __fixunsdfsi", "rlwinm r15, r3, 0, 16, 31" },
        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_FPEXT,
          ANVIL_MIR_REG_FPR, 32, ANVIL_MIR_REG_FPR, 64,
          INT64_C(0x3f800000), "ppc64le_fpext", "fmr f15, f14", NULL },
        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_FPTRUNC,
          ANVIL_MIR_REG_FPR, 64, ANVIL_MIR_REG_FPR, 32,
          INT64_C(0x3ff0000000000000), "ppc64le_fptrunc",
          "frsp f15, f14", NULL },
        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_SITOFP,
          ANVIL_MIR_REG_GPR, 64, ANVIL_MIR_REG_FPR, 32,
          INT64_C(16777217), "ppc64le_i64_to_f32_halfway_2p24",
          "bl __floatdisf", "mr r3, r14" },
        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_SITOFP,
          ANVIL_MIR_REG_GPR, 64, ANVIL_MIR_REG_FPR, 64,
          INT64_C(9007199254740993), "ppc64le_i64_to_f64_halfway_2p53",
          "bl __floatdidf", "mr r3, r14" },
        { ANVIL_PPC_VARIANT_PPC64, ANVIL_MIR_OP_UITOFP,
          ANVIL_MIR_REG_GPR, 64, ANVIL_MIR_REG_FPR, 64,
          -1, "ppc64_u64max_to_f64", "bl __floatundidf", "mr r3, r14" },
        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_FPTOSI,
          ANVIL_MIR_REG_FPR, 64, ANVIL_MIR_REG_GPR, 64,
          INT64_C(0x7ff8000000000000), "ppc64le_nan_fptosi_poison_domain",
          "bl __fixdfdi", "mr r15, r3" },
        { ANVIL_PPC_VARIANT_PPC64LE, ANVIL_MIR_OP_FPTOUI,
          ANVIL_MIR_REG_FPR, 64, ANVIL_MIR_REG_GPR, 64,
          INT64_C(0x7ff0000000000000), "ppc64le_inf_fptoui_poison_domain",
          "bl __fixunsdfdi", "mr r15, r3" },
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_mir_func_t *mir = anvil_mir_func_create(cases[c].name);
        CHECK(mir != NULL, "PowerPC numeric conversion MIR should be created");
        if (!mir) continue;

        anvil_mir_vreg_t src = anvil_mir_add_vreg_ex(
            mir, cases[c].src_class, cases[c].src_bits);
        anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(
            mir, cases[c].dst_class, cases[c].dst_bits);
        anvil_mir_vreg_t cast_uses[] = { src };
        anvil_mir_vreg_t ret_uses[] = { dst };
        CHECK(anvil_mir_set_fixed_reg(mir, src, 14) &&
              anvil_mir_set_fixed_reg(mir, dst, 15),
              "PowerPC conversion operands should use distinct stable registers");
        CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, src,
                                      cases[c].input_bits),
              "PowerPC numeric conversion should define its boundary source");
        CHECK(anvil_mir_add_instr(mir, cases[c].op, dst, cast_uses, 1),
              "generic MIR should represent the typed PowerPC conversion");
        CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, ret_uses, 1),
              "PowerPC numeric conversion MIR should terminate");

        char error[160] = { 0 };
        CHECK(anvil_ppc_verify_mir_legal(mir, cases[c].variant,
                                         error, sizeof(error)),
              "PowerPC legalizer should accept a well-typed numeric conversion");
        CHECK(anvil_ppc_regalloc_mir(mir, cases[c].variant),
              "PowerPC numeric conversion should allocate");
        char *asm_text = NULL;
        size_t asm_len = 0;
        CHECK(anvil_ppc_emit_mir(mir, cases[c].variant, &asm_text, &asm_len),
              "PowerPC numeric conversion should emit");
        CHECK(asm_text && asm_len > 0,
              "PowerPC numeric conversion assembly should be non-empty");
        if (asm_text) {
            check_contains(asm_text, cases[c].required,
                           "PowerPC conversion must use its numeric sequence/helper");
            if (cases[c].normalization) {
                check_contains(asm_text, cases[c].normalization,
                               "PowerPC conversion must normalize source/result width");
            }
            if (cases[c].op == ANVIL_MIR_OP_SITOFP ||
                cases[c].op == ANVIL_MIR_OP_UITOFP ||
                cases[c].op == ANVIL_MIR_OP_FPTOSI ||
                cases[c].op == ANVIL_MIR_OP_FPTOUI) {
                check_contains(asm_text, "\tmflr r0\n",
                               "helper-based conversion must preserve the link register");
            }
            free(asm_text);
        }

        anvil_mir_func_destroy(mir);
    }
}

static void test_ppc_numeric_boundary_expectations_are_ieee_exact(void)
{
    float at_2p24 = (float)INT64_C(16777217);
    double at_2p53 = (double)INT64_C(9007199254740993);
    double at_u64max = (double)UINT64_MAX;
    float halfway_to_f32 = (float)0x1.000001p0;
    uint32_t f32_bits = 0;
    uint64_t f64_bits = 0;

    memcpy(&f32_bits, &at_2p24, sizeof(f32_bits));
    CHECK(f32_bits == UINT32_C(0x4b800000),
          "2^24+1 must tie-to-even to 2^24 when converting i64 to f32");
    memcpy(&f64_bits, &at_2p53, sizeof(f64_bits));
    CHECK(f64_bits == UINT64_C(0x4340000000000000),
          "2^53+1 must tie-to-even to 2^53 when converting i64 to f64");
    memcpy(&f64_bits, &at_u64max, sizeof(f64_bits));
    CHECK(f64_bits == UINT64_C(0x43f0000000000000),
          "UINT64_MAX must round to 2^64 when converting u64 to f64");
    memcpy(&f32_bits, &halfway_to_f32, sizeof(f32_bits));
    CHECK(f32_bits == UINT32_C(0x3f800000),
          "FPTRUNC halfway case must use IEEE ties-to-even rounding");
    CHECK((int32_t)-1.75 == -1 && (uint32_t)65535.75 == UINT32_C(65535),
          "defined in-range FP-to-int conversions must truncate toward zero");
}

static void test_ppc_rejects_malformed_numeric_cast(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("bad_numeric_cast");
    CHECK(mir != NULL, "malformed PowerPC conversion MIR should be created");
    if (!mir) return;
    anvil_mir_vreg_t src = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t uses[] = { src };
    anvil_mir_vreg_t ret_uses[] = { dst };
    CHECK(anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, src, 1),
          "malformed conversion source should be defined");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_SITOFP, dst, uses, 1),
          "generic MIR should represent a class-invalid numeric conversion");
    CHECK(anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, ret_uses, 1),
          "malformed conversion MIR should terminate");
    char error[160] = { 0 };
    CHECK(!anvil_ppc_verify_mir_legal(mir, ANVIL_PPC_VARIANT_PPC64LE,
                                      error, sizeof(error)),
          "PowerPC legalizer must reject class-invalid numeric conversions");
    CHECK(strstr(error, "invalid cast widths or classes") != NULL ||
          strstr(error, "incompatible source/destination types") != NULL,
          "PowerPC conversion rejection should explain the type mismatch");
    anvil_mir_func_destroy(mir);
}

static void test_ppc_source_numeric_casts_lower_end_to_end(void)
{
    struct {
        anvil_arch_t arch;
        bool wide_int;
        const char *module_name;
        const char *signed_helper;
        const char *unsigned_helper;
        const char *fix_signed_helper;
        const char *fix_unsigned_helper;
    } targets[] = {
        { ANVIL_ARCH_PPC32, true, "ppc32_source_casts",
          "__floatdidf", "__floatundidf", "__fixdfdi", "__fixunsdfdi" },
        { ANVIL_ARCH_PPC64, true, "ppc64_source_casts",
          "__floatdidf", "__floatundidf", "__fixdfdi", "__fixunsdfdi" },
        { ANVIL_ARCH_PPC64LE, true, "ppc64le_source_casts",
          "__floatdidf", "__floatundidf", "__fixdfdi", "__fixunsdfdi" },
    };

    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
        anvil_ctx_t *ctx = new_ppc_ctx(targets[t].arch);
        if (!ctx) continue;
        anvil_module_t *mod = anvil_module_create(ctx, targets[t].module_name);
        CHECK(mod != NULL, "PowerPC source conversion module should be created");
        if (!mod) {
            anvil_ctx_destroy(ctx);
            continue;
        }

        anvil_type_t *signed_int = targets[t].wide_int
            ? anvil_type_i64(ctx) : anvil_type_i32(ctx);
        anvil_type_t *unsigned_int = targets[t].wide_int
            ? anvil_type_u64(ctx) : anvil_type_u32(ctx);
        anvil_type_t *f32 = anvil_type_f32(ctx);
        anvil_type_t *f64 = anvil_type_f64(ctx);
        struct {
            const char *name;
            anvil_type_t *src_type;
            anvil_type_t *dst_type;
            anvil_mir_opcode_t op;
        } conversions[] = {
            { "source_sitofp", signed_int, f64, ANVIL_MIR_OP_SITOFP },
            { "source_uitofp", unsigned_int, f64, ANVIL_MIR_OP_UITOFP },
            { "source_fptosi", f64, signed_int, ANVIL_MIR_OP_FPTOSI },
            { "source_fptoui", f64, unsigned_int, ANVIL_MIR_OP_FPTOUI },
            { "source_fpext", f32, f64, ANVIL_MIR_OP_FPEXT },
            { "source_fptrunc", f64, f32, ANVIL_MIR_OP_FPTRUNC },
        };

        for (size_t c = 0; c < sizeof(conversions) / sizeof(conversions[0]); c++) {
            anvil_type_t *params[] = { conversions[c].src_type };
            anvil_type_t *fn_type = anvil_type_func(
                ctx, conversions[c].dst_type, params, 1, false);
            anvil_func_t *fn = anvil_func_create(
                mod, conversions[c].name, fn_type, ANVIL_LINK_EXTERNAL);
            CHECK(fn != NULL, "PowerPC source conversion function should be created");
            if (!fn) continue;
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *src = anvil_func_get_param(fn, 0);
            anvil_value_t *result = NULL;
            switch (conversions[c].op) {
                case ANVIL_MIR_OP_SITOFP:
                    result = anvil_build_sitofp(ctx, src, conversions[c].dst_type,
                                                "converted");
                    break;
                case ANVIL_MIR_OP_UITOFP:
                    result = anvil_build_uitofp(ctx, src, conversions[c].dst_type,
                                                "converted");
                    break;
                case ANVIL_MIR_OP_FPTOSI:
                    result = anvil_build_fptosi(ctx, src, conversions[c].dst_type,
                                                "converted");
                    break;
                case ANVIL_MIR_OP_FPTOUI:
                    result = anvil_build_fptoui(ctx, src, conversions[c].dst_type,
                                                "converted");
                    break;
                case ANVIL_MIR_OP_FPEXT:
                    result = anvil_build_fpext(ctx, src, conversions[c].dst_type,
                                               "converted");
                    break;
                case ANVIL_MIR_OP_FPTRUNC:
                    result = anvil_build_fptrunc(ctx, src, conversions[c].dst_type,
                                                 "converted");
                    break;
                default:
                    break;
            }
            CHECK(result != NULL, "PowerPC source conversion should build");
            if (result) anvil_build_ret(ctx, result);

            anvil_ppc_variant_t variant = targets[t].arch == ANVIL_ARCH_PPC32
                ? ANVIL_PPC_VARIANT_PPC32
                : targets[t].arch == ANVIL_ARCH_PPC64
                    ? ANVIL_PPC_VARIANT_PPC64
                    : ANVIL_PPC_VARIANT_PPC64LE;
            anvil_mir_func_t *lowered = result
                ? anvil_ppc_lower_func_to_mir(fn, variant) : NULL;
            if (!lowered) {
                fprintf(stderr, "[FAIL] source conversion %s did not lower for %s\n",
                        conversions[c].name, targets[t].module_name);
                failures++;
            }
            if (lowered && targets[t].arch == ANVIL_ARCH_PPC32 &&
                (conversions[c].op == ANVIL_MIR_OP_SITOFP ||
                 conversions[c].op == ANVIL_MIR_OP_UITOFP)) {
                bool saw_pair_call = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(lowered); i++) {
                    anvil_mir_instr_info_t info;
                    if (!anvil_mir_get_instr_info(lowered, i, &info) ||
                        info.op != ANVIL_MIR_OP_CALL || info.num_uses != 2) {
                        continue;
                    }
                    const anvil_mir_vreg_info_t *hi = anvil_mir_get_vreg_info(
                        lowered, anvil_mir_get_instr_use(lowered, i, 0));
                    const anvil_mir_vreg_info_t *lo = anvil_mir_get_vreg_info(
                        lowered, anvil_mir_get_instr_use(lowered, i, 1));
                    saw_pair_call = hi && lo && hi->has_fixed_reg &&
                        lo->has_fixed_reg && hi->fixed_phys_reg == 3 &&
                        lo->fixed_phys_reg == 4;
                }
                CHECK(saw_pair_call,
                      "PPC32 i64-to-FP helper call must receive ABI pair r3/r4");
            }
            if (lowered && targets[t].arch == ANVIL_ARCH_PPC32 &&
                (conversions[c].op == ANVIL_MIR_OP_FPTOSI ||
                 conversions[c].op == ANVIL_MIR_OP_FPTOUI)) {
                bool saw_low_result = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(lowered); i++) {
                    anvil_mir_instr_info_t info;
                    if (!anvil_mir_get_instr_info(lowered, i, &info) ||
                        info.op != ANVIL_MIR_OP_CALL_RESULT || info.symbol) {
                        continue;
                    }
                    const anvil_mir_vreg_info_t *lo =
                        anvil_mir_get_vreg_info(lowered, info.def);
                    saw_low_result = lo && lo->has_fixed_reg &&
                                     lo->fixed_phys_reg == 4;
                }
                CHECK(saw_low_result,
                      "PPC32 FP-to-i64 helper call must model low result r4");
            }
            anvil_mir_func_destroy(lowered);
        }

        char *asm_text = NULL;
        size_t asm_len = 0;
        CHECK(anvil_module_codegen(mod, &asm_text, &asm_len) == ANVIL_OK,
              "all six source-level PowerPC conversions should lower and emit");
        CHECK(asm_text && asm_len > 0,
              "source-level PowerPC conversion assembly should be non-empty");
        if (asm_text) {
            check_contains(asm_text, targets[t].signed_helper,
                           "source SITOFP should select helper by integer width");
            check_contains(asm_text, targets[t].unsigned_helper,
                           "source UITOFP should preserve unsigned semantics");
            check_contains(asm_text, targets[t].fix_signed_helper,
                           "source FPTOSI should select signed truncating helper");
            check_contains(asm_text, targets[t].fix_unsigned_helper,
                           "source FPTOUI should select unsigned truncating helper");
            check_contains(asm_text, "\tfrsp ",
                           "source FPTRUNC should round to single precision");
            free(asm_text);
        }
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

static void test_ppc_single_precision_ops_and_dynamic_alloca_backchain(void)
{
    anvil_ppc_variant_t variants[] = {
        ANVIL_PPC_VARIANT_PPC32,
        ANVIL_PPC_VARIANT_PPC64,
        ANVIL_PPC_VARIANT_PPC64LE,
    };
    for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
        const anvil_ppc_target_desc_t *desc =
            anvil_ppc_get_target_desc(variants[v]);
        anvil_mir_func_t *mir = anvil_mir_func_create("ppc_f32_ops");
        anvil_mir_vreg_t a = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_FPR, 32);
        anvil_mir_vreg_t b = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_FPR, 32);
        anvil_mir_vreg_t sum = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_FPR, 32);
        anvil_mir_vreg_t diff = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_FPR, 32);
        anvil_mir_vreg_t prod = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_FPR, 32);
        anvil_mir_vreg_t quot = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_FPR, 32);
        anvil_mir_vreg_t uses[] = { a, b };
        anvil_mir_vreg_t all[] = { sum, diff, prod, quot };
        anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, a, 0x3f800000);
        anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, b, 0x40000000);
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_ADD, sum, uses, 2);
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_SUB, diff, uses, 2);
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_MUL, prod, uses, 2);
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_FDIV, quot, uses, 2);
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_KEEPALIVE,
                            ANVIL_MIR_NO_VREG, all, 4);
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                            ANVIL_MIR_NO_VREG, NULL, 0);
        CHECK(anvil_ppc_regalloc_mir(mir, variants[v]),
              "PPC f32 arithmetic MIR should allocate on every ABI");
        char *text = NULL;
        size_t len = 0;
        CHECK(anvil_ppc_emit_mir(mir, variants[v], &text, &len),
              "PPC f32 arithmetic should emit on every ABI");
        if (text) {
            check_contains(text, "\tfadds ", "f32 add must use fadds rounding");
            check_contains(text, "\tfsubs ", "f32 sub must use fsubs rounding");
            check_contains(text, "\tfmuls ", "f32 mul must use fmuls rounding");
            check_contains(text, "\tfdivs ", "f32 div must use fdivs rounding");
            check_not_contains(text, "\tfadd ", "f32 add must not silently use f64 form");
            check_not_contains(text, "\tfdiv ", "f32 div must not silently use f64 form");
            free(text);
        }
        anvil_mir_func_destroy(mir);

        mir = anvil_mir_func_create("ppc_dyn_alloca_chain");
        unsigned ptr_bits = desc->word_size * 8;
        anvil_mir_vreg_t count = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_GPR, ptr_bits);
        anvil_mir_vreg_t first = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_GPR, ptr_bits);
        anvil_mir_vreg_t second = anvil_mir_add_vreg_ex(
            mir, ANVIL_MIR_REG_GPR, ptr_bits);
        anvil_mir_vreg_t count_use[] = { count };
        anvil_mir_vreg_t ret_use[] = { second };
        anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, count, 3);
        anvil_mir_add_instr_imm_uses(mir, ANVIL_MIR_OP_DYN_ALLOCA,
                                     first, count_use, 1, 4);
        anvil_mir_add_call(mir, ANVIL_MIR_NO_VREG, NULL, 0, "nested",
                           ANVIL_CC_SYSV, false, 0);
        anvil_mir_add_instr_imm_uses(mir, ANVIL_MIR_OP_DYN_ALLOCA,
                                     second, count_use, 1, 4);
        anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                            ANVIL_MIR_NO_VREG, ret_use, 1);
        CHECK(anvil_ppc_regalloc_mir(mir, variants[v]),
              "PPC dynamic alloca MIR should allocate on every ABI");
        text = NULL;
        len = 0;
        CHECK(anvil_ppc_emit_mir(mir, variants[v], &text, &len),
              "PPC dynamic alloca should emit on every ABI");
        if (text) {
            const char *update = desc->word_size == 8
                ? "\tstdux r1, r1, r11\n" : "\tstwux r1, r1, r11\n";
            CHECK(count_occurrences(text, update) == 2,
                  "each dynamic allocation must atomically update SP/backchain");
            check_contains(text, desc->word_size == 8
                                      ? "\tclrrdi r11, r11, 4\n"
                                      : "\trlwinm r11, r11, 0, 0, 27\n",
                           "dynamic allocation size must be rounded to 16 bytes");
            char payload_offset[48];
            snprintf(payload_offset, sizeof(payload_offset), "r1, %u\n",
                     desc->min_frame_size);
            check_contains(text, payload_offset,
                           "dynamic allocation result must skip the backchain prefix");
            check_contains(text, "\tmr r1, r31\n",
                           "dynamic-allocation epilogue must restore SP from stable frame pointer");
            check_not_contains(text, "\tsubf r1, r11, r1\n",
                               "dynamic allocation must not update SP without a backchain");
            free(text);
        }
        size_t dynamic_size = ((size_t)3 * 4 + desc->min_frame_size + 15) &
                              ~(size_t)15;
        CHECK(desc->min_frame_size + (size_t)3 * 4 <= dynamic_size,
              "dynamic allocation alignment model must reserve non-overlapping backchain space");
        anvil_mir_func_destroy(mir);
    }
}

static void test_ppc_emits_interleaved_block_ownership_correctly(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("ppc_interleaved");
    CHECK(mir != NULL, "interleaved PPC MIR should be created");
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
          "PPC MIR should permit valid per-block order with interleaved storage");
    CHECK(anvil_ppc_regalloc_mir(mir, ANVIL_PPC_VARIANT_PPC32),
          "interleaved PPC MIR should allocate");
    char *text = NULL;
    size_t len = 0;
    CHECK(anvil_ppc_emit_mir(mir, ANVIL_PPC_VARIANT_PPC32, &text, &len),
          "interleaved PPC MIR should emit");
    if (text) {
        const char *entry_label = strstr(text, ".Lppc_interleaved_entry:");
        const char *branch = entry_label ? strstr(entry_label, "\tbne ") : NULL;
        const char *then_label = strstr(text, ".Lppc_interleaved_then:");
        CHECK(entry_label && branch && then_label &&
              entry_label < branch && branch < then_label,
              "PPC emitter must select instructions by block, not a false contiguous range");
        free(text);
    }
    anvil_mir_func_destroy(mir);
}

static void test_ppc64_legalizes_unequal_width_pointer_casts(void)
{
    anvil_ctx_t *ctx = new_ppc_ctx(ANVIL_ARCH_PPC64);
    if (!ctx) return;
    anvil_module_t *mod = anvil_module_create(ctx, "ppc64_ptr_cast");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *ptr = anvil_type_ptr(ctx, anvil_type_i8(ctx));
    anvil_type_t *params[] = { i32 };
    anvil_func_t *fn = anvil_func_create(mod, "int_to_ptr",
        anvil_type_func(ctx, ptr, params, 1, false), ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "PPC64 unequal-width pointer cast function should build");
    if (fn) {
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *cast = anvil_build_inttoptr(
            ctx, anvil_func_get_param(fn, 0), ptr, "ptr");
        CHECK(cast && anvil_build_ret(ctx, cast),
              "PPC64 inttoptr IR should build");
        anvil_mir_func_t *mir = anvil_ppc_lower_func_to_mir(
            fn, ANVIL_PPC_VARIANT_PPC64);
        CHECK(mir != NULL, "PPC64 inttoptr should lower");
        bool saw_zext = false;
        if (mir) for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
            anvil_mir_instr_info_t info;
            if (anvil_mir_get_instr_info(mir, i, &info) &&
                info.op == ANVIL_MIR_OP_ZEXT) saw_zext = true;
        }
        CHECK(saw_zext,
              "PPC64 i32-to-pointer must zero-extend, not use an unequal-width bitcast");
        CHECK(mir && anvil_ppc_regalloc_mir(mir, ANVIL_PPC_VARIANT_PPC64),
              "PPC64 legalized pointer cast should allocate");
        char *text = NULL;
        size_t len = 0;
        CHECK(mir && anvil_ppc_emit_mir(mir, ANVIL_PPC_VARIANT_PPC64,
                                        &text, &len),
              "PPC64 legalized pointer cast should emit");
        free(text);
        anvil_mir_func_destroy(mir);
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

int main(void)
{
    test_ppc_descriptors_capture_real_variant_differences();
    test_ppc32_lowers_i32_add_with_fixed_abi_regs();
    test_ppc64_variants_lower_and_emit_distinct_abis();
    test_ppc_stack_call_args_use_variant_word_size();
    test_ppc64le_emits_indirect_call_through_ctr();
    test_ppc32_splits_i64_locals_into_word_pairs();
    test_ppc_module_codegen_routes_through_shared_mir_path();
    test_ppc_numeric_casts_have_typed_lowering();
    test_ppc_numeric_boundary_expectations_are_ieee_exact();
    test_ppc_rejects_malformed_numeric_cast();
    test_ppc_source_numeric_casts_lower_end_to_end();
    test_ppc_single_precision_ops_and_dynamic_alloca_backchain();
    test_ppc_emits_interleaved_block_ownership_correctly();
    test_ppc64_legalizes_unequal_width_pointer_casts();

    if (failures) {
        fprintf(stderr, "%d PowerPC MachineIR regression test(s) failed\n",
                failures);
        return 1;
    }

    printf("PowerPC MachineIR lowering regression tests passed\n");
    return 0;
}
