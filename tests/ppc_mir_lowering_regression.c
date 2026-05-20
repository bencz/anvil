/*
 * Regression tests for the shared PowerPC -> MachineIR backend path.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_machine.h>
#include <anvil/anvil_ppc_mir.h>

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

static void check_not_contains(const char *text, const char *needle, const char *msg)
{
    CHECK(text && needle && strstr(text, needle) == NULL, msg);
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
                anvil_value_t *call =
                    anvil_build_call(ctx, callee_type,
                                     anvil_func_get_value(callee),
                                     args, 10, "call");
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
            anvil_value_t *called =
                anvil_build_call(ctx, callee_type, loaded, args, 2, "called");
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
        anvil_type_t *i8 = anvil_type_i8(ctx);
        anvil_func_t *fn =
            anvil_func_create(mod, "ppc32_i64_pair_cmp",
                              anvil_type_func(ctx, i8, NULL, 0, false),
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

int main(void)
{
    test_ppc_descriptors_capture_real_variant_differences();
    test_ppc32_lowers_i32_add_with_fixed_abi_regs();
    test_ppc64_variants_lower_and_emit_distinct_abis();
    test_ppc_stack_call_args_use_variant_word_size();
    test_ppc64le_emits_indirect_call_through_ctr();
    test_ppc32_splits_i64_locals_into_word_pairs();
    test_ppc_module_codegen_routes_through_shared_mir_path();

    if (failures) {
        fprintf(stderr, "%d PowerPC MachineIR regression test(s) failed\n",
                failures);
        return 1;
    }

    printf("PowerPC MachineIR lowering regression tests passed\n");
    return 0;
}
