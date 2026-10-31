/*
 * Regression tests for MachineIR and register allocation infrastructure.
 */

#include <anvil/anvil_machine.h>
#include <anvil/anvil_internal.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s\n", msg); \
        failures++; \
    } \
} while (0)

static void test_machine_ir_tracks_vregs_and_instructions(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_basic");
    CHECK(fn != NULL, "MachineIR function should be created");
    if (!fn) return;

    anvil_mir_vreg_t v0 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t v1 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t uses[] = { v0 };
    const anvil_mir_vreg_info_t *v0_info = anvil_mir_get_vreg_info(fn, v0);

    CHECK(v0 == 0, "first virtual register should be v0");
    CHECK(v1 == 1, "second virtual register should be v1");
    CHECK(v0_info != NULL, "default vreg should expose metadata");
    CHECK(v0_info && v0_info->reg_class == ANVIL_MIR_REG_GPR,
          "default vreg should use GPR class");
    CHECK(v0_info && v0_info->size_bits == 64,
          "default vreg should use 64-bit size");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, v0, NULL, 0),
          "MachineIR should accept def-only instruction");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, v1, uses, 1),
          "MachineIR should accept instruction uses");
    CHECK(anvil_mir_num_vregs(fn) == 2, "MachineIR should track vreg count");
    CHECK(anvil_mir_num_instrs(fn) == 2, "MachineIR should track instruction count");

    anvil_mir_func_destroy(fn);
}

static void test_strbuf_failure_is_sticky_and_detach_is_atomic(void)
{
    anvil_strbuf_t buffer;
    anvil_strbuf_init(&buffer);
    CHECK(buffer.data != NULL, "string buffer should initialize for failure test");
    if (!buffer.data) return;
    anvil_strbuf_append(&buffer, "prefix");
    buffer.failed = true; /* Deterministic fault injection after partial output. */
    anvil_strbuf_append(&buffer, "must-not-escape");
    size_t len = 123;
    char *detached = anvil_strbuf_detach(&buffer, &len);
    CHECK(detached == NULL && len == 0,
          "failed string buffer must not detach partial assembly");
    CHECK(buffer.data == NULL && buffer.len == 0 && buffer.cap == 0,
          "failed detach must release and reset the partial buffer");
}

static void test_machine_ir_tracks_blocks_and_branch_targets(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_blocks");
    CHECK(fn != NULL, "MachineIR function should be created for block test");
    if (!fn) return;

    CHECK(anvil_mir_num_blocks(fn) == 1,
          "MachineIR functions should start with an entry block");
    anvil_mir_block_t entry = anvil_mir_current_block(fn);
    anvil_mir_block_t then_block = anvil_mir_add_block(fn, "then");
    anvil_mir_block_t else_block = anvil_mir_add_block(fn, "else");
    CHECK(entry != ANVIL_MIR_NO_BLOCK &&
          then_block != ANVIL_MIR_NO_BLOCK &&
          else_block != ANVIL_MIR_NO_BLOCK,
          "MachineIR blocks should be created");

    anvil_mir_vreg_t cond = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 8);
    CHECK(anvil_mir_set_current_block(fn, entry),
          "entry block should be selectable");
    CHECK(anvil_mir_add_cond_branch(fn, cond, then_block, else_block),
          "MachineIR should add conditional branch with targets");

    CHECK(anvil_mir_set_current_block(fn, then_block),
          "then block should be selectable");
    CHECK(anvil_mir_add_branch(fn, else_block),
          "MachineIR should add unconditional branch with target");

    anvil_mir_block_info_t entry_info;
    anvil_mir_block_info_t then_info;
    CHECK(anvil_mir_get_block_info(fn, entry, &entry_info),
          "entry block info should be inspectable");
    CHECK(anvil_mir_get_block_info(fn, then_block, &then_info),
          "then block info should be inspectable");
    CHECK(entry_info.first_instr == 0 && entry_info.num_instrs == 1,
          "entry block should own its branch instruction");
    CHECK(then_info.first_instr == 1 && then_info.num_instrs == 1,
          "then block should own its branch instruction");

    anvil_mir_instr_info_t cond_br;
    anvil_mir_instr_info_t br;
    CHECK(anvil_mir_get_instr_info(fn, 0, &cond_br),
          "conditional branch should be inspectable");
    CHECK(anvil_mir_get_instr_info(fn, 1, &br),
          "unconditional branch should be inspectable");
    CHECK(cond_br.block == entry && cond_br.true_block == then_block &&
          cond_br.false_block == else_block && cond_br.num_uses == 1,
          "conditional branch should preserve block, targets, and condition");
    CHECK(br.block == then_block && br.true_block == else_block &&
          br.false_block == ANVIL_MIR_NO_BLOCK,
          "unconditional branch should preserve block and target");

    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_verifier_accepts_valid_function(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_verify_valid");
    CHECK(fn != NULL, "MachineIR function should be created for verifier test");
    if (!fn) return;

    anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t ret_uses[] = { value };
    CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 7),
          "valid verifier function should define a value");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "valid verifier function should terminate");

    char error[128] = { 0 };
    CHECK(anvil_mir_verify(fn, error, sizeof(error)),
          "MachineIR verifier should accept a valid function");
    CHECK(error[0] == '\0',
          "MachineIR verifier should not leave an error for valid MIR");

    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_verifier_rejects_unterminated_blocks(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_verify_unterminated");
    CHECK(fn != NULL, "MachineIR function should be created for unterminated verifier test");
    if (!fn) return;

    anvil_mir_vreg_t value = anvil_mir_add_vreg(fn);
    CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 1),
          "unterminated verifier test should define a value");

    char error[128] = { 0 };
    CHECK(!anvil_mir_verify(fn, error, sizeof(error)),
          "MachineIR verifier should reject a non-empty block without terminator");
    CHECK(strstr(error, "terminator") != NULL,
          "MachineIR verifier should explain missing terminator");

    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_verifier_rejects_register_class_mismatch(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_verify_class_mismatch");
    CHECK(fn != NULL, "MachineIR function should be created for class mismatch verifier test");
    if (!fn) return;

    anvil_mir_vreg_t g0 = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t f0 = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t uses[] = { g0, f0 };
    anvil_mir_vreg_t ret_uses[] = { dst };

    CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, g0, 1),
          "class mismatch verifier test should define GPR input");
    CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, f0, 2),
          "class mismatch verifier test should define FPR input");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, dst, uses, 2),
          "class mismatch verifier test should add mismatched operands");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "class mismatch verifier test should terminate");

    char error[128] = { 0 };
    CHECK(!anvil_mir_verify(fn, error, sizeof(error)),
          "MachineIR verifier should reject mismatched register classes");
    CHECK(strstr(error, "class") != NULL,
          "MachineIR verifier should explain register class mismatches");

    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_verifier_rejects_use_before_def(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_use_before_def");
    anvil_mir_vreg_t value = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t uses[] = { value };
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, uses, 1),
          "use-before-def test should build malformed MIR");
    char error[160] = { 0 };
    CHECK(!anvil_mir_verify(fn, error, sizeof(error)) &&
          strstr(error, "before definite assignment") != NULL,
          "verifier must diagnose same-block use before definition");
    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_verifier_tracks_definitions_across_diamond(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_def_diamond");
    anvil_mir_block_t entry = anvil_mir_current_block(fn);
    anvil_mir_block_t left = anvil_mir_add_block(fn, "left");
    anvil_mir_block_t right = anvil_mir_add_block(fn, "right");
    anvil_mir_block_t merge = anvil_mir_add_block(fn, "merge");
    anvil_mir_vreg_t cond = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    CHECK(anvil_mir_set_current_block(fn, entry) &&
          anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, cond, 1) &&
          anvil_mir_add_cond_branch(fn, cond, left, right),
          "diamond entry should be valid");
    CHECK(anvil_mir_set_current_block(fn, left) &&
          anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 7) &&
          anvil_mir_add_branch(fn, merge),
          "diamond left should define the merged value");
    CHECK(anvil_mir_set_current_block(fn, right) &&
          anvil_mir_add_branch(fn, merge),
          "diamond right should omit the merged definition");
    anvil_mir_vreg_t uses[] = { value };
    CHECK(anvil_mir_set_current_block(fn, merge) &&
          anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, uses, 1),
          "diamond merge should use the conditional definition");
    char error[160] = { 0 };
    CHECK(!anvil_mir_verify(fn, error, sizeof(error)) &&
          strstr(error, "before definite assignment") != NULL,
          "definition on only one diamond arm must not be definite at merge");

    CHECK(anvil_mir_set_current_block(fn, right),
          "diamond right should be selectable for repair");
    /* Insertion after its terminator is deliberately impossible to repair in
       place; build a second graph to prove multiple edge definitions. */
    anvil_mir_func_destroy(fn);

    fn = anvil_mir_func_create("mir_parallel_defs_diamond");
    entry = anvil_mir_current_block(fn);
    left = anvil_mir_add_block(fn, "left");
    right = anvil_mir_add_block(fn, "right");
    merge = anvil_mir_add_block(fn, "merge");
    cond = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 8);
    value = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_set_current_block(fn, entry);
    anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, cond, 1);
    anvil_mir_add_cond_branch(fn, cond, left, right);
    anvil_mir_set_current_block(fn, left);
    anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 7);
    anvil_mir_add_branch(fn, merge);
    anvil_mir_set_current_block(fn, right);
    anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 9);
    anvil_mir_add_branch(fn, merge);
    uses[0] = value;
    anvil_mir_set_current_block(fn, merge);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, uses, 1);
    CHECK(anvil_mir_verify(fn, error, sizeof(error)),
          "multiple PHI-edge-style definitions on all paths must be accepted");
    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_verifier_rejects_unreachable_and_width_mismatch(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_unreachable");
    anvil_mir_block_t dead = anvil_mir_add_block(fn, "dead");
    anvil_mir_set_current_block(fn, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);
    anvil_mir_set_current_block(fn, dead);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);
    char error[160] = { 0 };
    CHECK(!anvil_mir_verify(fn, error, sizeof(error)) &&
          strstr(error, "unreachable") != NULL,
          "verifier must reject unreachable MIR blocks");
    anvil_mir_func_destroy(fn);

    fn = anvil_mir_func_create("mir_width_mismatch");
    anvil_mir_vreg_t i32 = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t i64 = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t copy_use[] = { i32 };
    anvil_mir_vreg_t ret_use[] = { i64 };
    anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, i32, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_COPY, i64, copy_use, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_use, 1);
    CHECK(!anvil_mir_verify(fn, error, sizeof(error)) &&
          strstr(error, "mismatch") != NULL,
          "verifier must reject same-class copies with unequal widths");
    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_verifier_handles_loops_and_rejects_other(void)
{
    char error[160] = { 0 };
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_invalid_loop_def");
    anvil_mir_block_t header = anvil_mir_add_block(fn, "header");
    anvil_mir_block_t body = anvil_mir_add_block(fn, "body");
    anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(
        fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_set_current_block(fn, 0);
    anvil_mir_add_branch(fn, header);
    anvil_mir_set_current_block(fn, header);
    anvil_mir_vreg_t value_use[] = { value };
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE,
                        ANVIL_MIR_NO_VREG, value_use, 1);
    anvil_mir_add_branch(fn, body);
    anvil_mir_set_current_block(fn, body);
    anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 1);
    anvil_mir_add_branch(fn, header);
    CHECK(!anvil_mir_verify(fn, error, sizeof(error)) &&
          strstr(error, "before definite assignment") != NULL,
          "a backedge definition must not satisfy the loop entry path");
    anvil_mir_func_destroy(fn);

    fn = anvil_mir_func_create("mir_valid_loop_phi_cycle");
    header = anvil_mir_add_block(fn, "header");
    body = anvil_mir_add_block(fn, "body");
    anvil_mir_vreg_t a = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t b = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t temp = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_set_current_block(fn, 0);
    anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, a, 1);
    anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, b, 2);
    anvil_mir_add_branch(fn, header);
    anvil_mir_set_current_block(fn, header);
    anvil_mir_vreg_t live[] = { a, b };
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE,
                        ANVIL_MIR_NO_VREG, live, 2);
    anvil_mir_add_branch(fn, body);
    anvil_mir_set_current_block(fn, body);
    anvil_mir_vreg_t use_a[] = { a };
    anvil_mir_vreg_t use_b[] = { b };
    anvil_mir_vreg_t use_temp[] = { temp };
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_COPY, temp, use_a, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_COPY, a, use_b, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_COPY, b, use_temp, 1);
    anvil_mir_add_branch(fn, header);
    CHECK(anvil_mir_verify(fn, error, sizeof(error)),
          "loop-carried parallel-copy cycles with entry definitions are valid");
    anvil_mir_func_destroy(fn);

    fn = anvil_mir_func_create("mir_reject_invalid_opcode");
    value = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 1);
    value_use[0] = value;
    anvil_mir_add_instr(fn, (anvil_mir_opcode_t)UINT32_MAX,
                        ANVIL_MIR_NO_VREG, value_use, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET,
                        ANVIL_MIR_NO_VREG, NULL, 0);
    CHECK(!anvil_mir_verify(fn, error, sizeof(error)) &&
          strstr(error, "invalid opcode") != NULL,
          "generic verifier must reject opcodes outside the formal enum");
    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_verifier_models_liveins_and_abi_bundles(void)
{
    char error[192] = { 0 };
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_fixed_not_livein");
    anvil_mir_vreg_t result = anvil_mir_add_vreg_ex(
        fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_set_fixed_reg(fn, result, 0);
    anvil_mir_vreg_t result_use[] = { result };
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE,
                        ANVIL_MIR_NO_VREG, result_use, 1);
    anvil_mir_add_call(fn, result, NULL, 0, "callee", ANVIL_CC_CDECL,
                       false, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET,
                        ANVIL_MIR_NO_VREG, result_use, 1);
    CHECK(!anvil_mir_verify(fn, error, sizeof(error)) &&
          strstr(error, "before definite assignment") != NULL,
          "a fixed call result must not be treated as an ABI live-in");
    anvil_mir_func_destroy(fn);

    fn = anvil_mir_func_create("mir_explicit_livein");
    result = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_set_fixed_reg(fn, result, 0);
    anvil_mir_set_live_in(fn, result, true);
    result_use[0] = result;
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET,
                        ANVIL_MIR_NO_VREG, result_use, 1);
    CHECK(anvil_mir_verify(fn, error, sizeof(error)),
          "an explicitly marked ABI input must be definite at entry");
    anvil_mir_func_destroy(fn);

    fn = anvil_mir_func_create("mir_multi_result_bundle");
    anvil_mir_vreg_t lo = anvil_mir_add_vreg_ex(
        fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t hi = anvil_mir_add_vreg_ex(
        fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_set_fixed_reg(fn, lo, 0);
    anvil_mir_set_fixed_reg(fn, hi, 1);
    anvil_mir_add_call(fn, ANVIL_MIR_NO_VREG, NULL, 0, "pair_result",
                       ANVIL_CC_CDECL, false, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_CALL_RESULT, lo, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_CALL_RESULT, hi, NULL, 0);
    anvil_mir_vreg_t pair_uses[] = { lo, hi };
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE,
                        ANVIL_MIR_NO_VREG, pair_uses, 2);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET_VALUE_PART,
                        ANVIL_MIR_NO_VREG, result_use, 1);
    anvil_mir_vreg_t hi_use[] = { hi };
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET_VALUE_PART,
                        ANVIL_MIR_NO_VREG, hi_use, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET,
                        ANVIL_MIR_NO_VREG, NULL, 0);
    CHECK(anvil_mir_verify(fn, error, sizeof(error)),
          "contiguous multi-part call-result and return bundles must verify");
    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_coalesces_redundant_nonfixed_copies(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_copy_coalesce");
    CHECK(fn != NULL, "MachineIR function should be created for copy coalescing");
    if (!fn) return;

    anvil_mir_vreg_t src = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t sum = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t copy_uses[] = { src };
    anvil_mir_vreg_t add_uses[] = { dst, src };
    anvil_mir_vreg_t ret_uses[] = { sum };

    CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, src, 3),
          "copy coalescing should define source");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_COPY, dst, copy_uses, 1),
          "copy coalescing should add redundant copy");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, sum, add_uses, 2),
          "copy coalescing should consume copied value");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "copy coalescing test should terminate");

    CHECK(anvil_mir_coalesce_copies(fn),
          "MachineIR copy coalescing pass should run");

    CHECK(anvil_mir_num_instrs(fn) == 3,
          "redundant copy should be physically removed, not changed to OTHER");
    CHECK(anvil_mir_get_instr_use(fn, 1, 0) == src,
          "coalescing should rewrite dst uses to source vreg");
    CHECK(anvil_mir_get_instr_use(fn, 1, 1) == src,
          "coalescing should preserve existing source uses");

    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_coalescing_preserves_fixed_register_copies(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_copy_fixed");
    CHECK(fn != NULL, "MachineIR function should be created for fixed copy coalescing");
    if (!fn) return;

    anvil_mir_vreg_t fixed = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t local = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t copy_uses[] = { fixed };
    anvil_mir_vreg_t ret_uses[] = { local };

    CHECK(anvil_mir_set_fixed_reg(fn, fixed, 0),
          "fixed copy source should use ABI register");
    CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, fixed, 11),
          "fixed copy test should define source");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_COPY, local, copy_uses, 1),
          "fixed copy test should add ABI copy");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "fixed copy test should terminate");

    CHECK(anvil_mir_coalesce_copies(fn),
          "MachineIR copy coalescing pass should inspect fixed copies");

    anvil_mir_instr_info_t copy_info;
    CHECK(anvil_mir_get_instr_info(fn, 1, &copy_info),
          "fixed copy instruction should remain inspectable");
    CHECK(copy_info.op == ANVIL_MIR_OP_COPY &&
          copy_info.def == local &&
          copy_info.num_uses == 1 &&
          anvil_mir_get_instr_use(fn, 1, 0) == fixed,
          "coalescing should preserve copies from fixed ABI registers");

    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_coalescing_does_not_cross_nondominating_blocks(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_copy_diamond");
    CHECK(fn != NULL, "MachineIR function should be created for diamond coalescing");
    if (!fn) return;

    anvil_mir_block_t entry = anvil_mir_current_block(fn);
    anvil_mir_block_t left = anvil_mir_add_block(fn, "left");
    anvil_mir_block_t right = anvil_mir_add_block(fn, "right");
    anvil_mir_block_t exit = anvil_mir_add_block(fn, "exit");
    anvil_mir_vreg_t cond = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t src = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t sum = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);

    CHECK(anvil_mir_set_current_block(fn, entry),
          "diamond entry block should be selectable");
    CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, cond, 1),
          "diamond should define its condition");
    CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, src, 7),
          "diamond should define the copy source");
    CHECK(anvil_mir_add_cond_branch(fn, cond, left, right),
          "diamond entry should branch to both arms");

    CHECK(anvil_mir_set_current_block(fn, left),
          "diamond left block should be selectable");
    anvil_mir_vreg_t copy_uses[] = { src };
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_COPY, dst, copy_uses, 1),
          "diamond left arm should define the copy destination");
    CHECK(anvil_mir_add_branch(fn, exit),
          "diamond left arm should reach exit");

    CHECK(anvil_mir_set_current_block(fn, right),
          "diamond right block should be selectable");
    anvil_mir_vreg_t add_uses[] = { dst, src };
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, sum, add_uses, 2),
          "diamond right arm should contain the nondominated destination use");
    CHECK(anvil_mir_add_branch(fn, exit),
          "diamond right arm should reach exit");

    CHECK(anvil_mir_set_current_block(fn, exit),
          "diamond exit block should be selectable");
    anvil_mir_vreg_t ret_uses[] = { src };
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "diamond exit should terminate");

    CHECK(anvil_mir_coalesce_copies(fn),
          "MachineIR coalescing should inspect a diamond CFG");

    anvil_mir_instr_info_t copy_info;
    CHECK(anvil_mir_get_instr_info(fn, 3, &copy_info),
          "diamond copy should remain inspectable");
    CHECK(copy_info.op == ANVIL_MIR_OP_COPY,
          "coalescing must retain a copy with a use outside its block");
    CHECK(anvil_mir_get_instr_use(fn, 5, 0) == dst,
          "coalescing must not rewrite a use the copy does not dominate");

    anvil_mir_func_destroy(fn);
}

static void test_linear_scan_allocates_register_classes_independently(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_classes");
    CHECK(fn != NULL, "MachineIR function should be created for class test");
    if (!fn) return;

    anvil_mir_vreg_t g0 = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t f0 = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_FPR, 64);
    anvil_mir_vreg_t both[] = { g0, f0 };

    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, g0, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, f0, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, both, 2);

    anvil_regalloc_class_config_t configs[] = {
        { ANVIL_MIR_REG_GPR, 1, NULL },
        { ANVIL_MIR_REG_FPR, 1, NULL },
    };
    CHECK(anvil_regalloc_linear_scan_classes(fn, configs, 2),
          "class-aware linear scan should allocate mixed register banks");
    CHECK(anvil_mir_num_spills(fn) == 0,
          "one GPR and one FPR should not compete for the same physical register");

    const anvil_regalloc_assignment_t *g_assignment =
        anvil_mir_get_assignment(fn, g0);
    const anvil_regalloc_assignment_t *f_assignment =
        anvil_mir_get_assignment(fn, f0);
    CHECK(g_assignment != NULL && f_assignment != NULL,
          "class-aware allocation should produce assignments");
    CHECK(g_assignment && g_assignment->reg_class == ANVIL_MIR_REG_GPR,
          "GPR vreg assignment should preserve class");
    CHECK(f_assignment && f_assignment->reg_class == ANVIL_MIR_REG_FPR,
          "FPR vreg assignment should preserve class");
    CHECK(g_assignment && !g_assignment->spilled && g_assignment->phys_reg == 0,
          "GPR vreg should use GPR physical register 0");
    CHECK(f_assignment && !f_assignment->spilled && f_assignment->phys_reg == 0,
          "FPR vreg should use FPR physical register 0");

    anvil_mir_func_destroy(fn);
}

static bool any_scale_mode(void *user, unsigned scale, int64_t disp, uint16_t access_bits)
{
    (void)user; (void)disp; (void)access_bits;
    return scale == 1 || scale == 2 || scale == 4 || scale == 8;
}

static void test_address_arithmetic_folds_into_accesses(void)
{
    for (int redefine = 0; redefine < 2; redefine++) {
        anvil_mir_func_t *fn = anvil_mir_func_create("mir_fold_address");
        anvil_mir_vreg_t base = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
        anvil_mir_vreg_t index = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
        anvil_mir_vreg_t eight = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
        anvil_mir_vreg_t scaled = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
        anvil_mir_vreg_t address = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
        anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
        anvil_mir_vreg_t mul_uses[] = { index, eight }, add_uses[] = { base, scaled }, load_uses[] = { address };
        anvil_mir_vreg_t store_uses[] = { value, address }, ret_uses[] = { value };
        CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, base, 4096) && anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, index, 3) &&
              anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, eight, 8) && anvil_mir_add_instr(fn, ANVIL_MIR_OP_MUL, scaled, mul_uses, 2) &&
              anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, address, add_uses, 2), "address arithmetic should build");
        /* A second definition of the index between the arithmetic and the
         * access must keep the arithmetic. */
        if (redefine)
            CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, index, 9), "index redefinition should build");
        CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_LOAD, value, load_uses, 1) &&
              anvil_mir_add_instr(fn, ANVIL_MIR_OP_STORE, ANVIL_MIR_NO_VREG, store_uses, 2) &&
              anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_uses, 1), "accesses should build");
        size_t before = anvil_mir_num_instrs(fn);
        CHECK(anvil_mir_fold_address_arithmetic(fn, any_scale_mode, NULL), "address folding should run");
        char error[192] = { 0 };
        CHECK(anvil_mir_verify(fn, error, sizeof(error)), "folded MachineIR should verify");
        size_t loads = 0, folded = 0, plain = 0;
        for (size_t i = 0; i < anvil_mir_num_instrs(fn); i++) {
            anvil_mir_instr_info_t info;
            if (!anvil_mir_get_instr_info(fn, i, &info) || (info.op != ANVIL_MIR_OP_LOAD && info.op != ANVIL_MIR_OP_STORE))
                continue;
            loads++;
            folded += info.index_scale == 8;
            plain += info.index_scale == 1;
        }
        if (redefine) {
            /* The scaled product was computed before the redefinition, so
             * base + product (scale 1) is still exact. */
            CHECK(folded == 0 && plain == 2 && anvil_mir_num_instrs(fn) == before - 1,
                  "a redefined index is not read later; the scaled product is used instead");
        } else {
            CHECK(loads == 2 && folded == 2, "both accesses through the address take base + index * 8");
            CHECK(anvil_mir_num_instrs(fn) == before - 3, "the ADD, the MUL and its constant are deleted");
        }
        anvil_mir_func_destroy(fn);
    }
}

static void test_local_cse_reuses_pure_values_until_redefinition(void)
{
    for (int redefine = 0; redefine < 2; redefine++) {
        anvil_mir_func_t *fn = anvil_mir_func_create("mir_local_cse");
        anvil_mir_vreg_t x = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t a = anvil_mir_add_vreg_typed(fn, ANVIL_MIR_REG_GPR, 64, true);
        anvil_mir_vreg_t b = anvil_mir_add_vreg_typed(fn, ANVIL_MIR_REG_GPR, 64, true);
        anvil_mir_vreg_t x_use[] = { x }, both[] = { a, b };
        CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, x, 5) && anvil_mir_add_instr(fn, ANVIL_MIR_OP_SEXT, a, x_use, 1),
              "first extension should build");
        if (redefine)
            CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, x, 6), "redefinition should build");
        CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_SEXT, b, x_use, 1) &&
              anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, both, 2) &&
              anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0), "second extension should build");
        CHECK(anvil_mir_local_cse(fn), "local CSE should run");
        anvil_mir_instr_info_t second;
        CHECK(anvil_mir_get_instr_info(fn, redefine ? 3 : 2, &second), "second extension should be inspectable");
        if (redefine)
            CHECK(second.op == ANVIL_MIR_OP_SEXT, "a redefined operand forbids reuse");
        else
            CHECK(second.op == ANVIL_MIR_OP_COPY && anvil_mir_get_instr_use(fn, 2, 0) == a, "the repeated extension copies the first");
        anvil_mir_func_destroy(fn);
    }
}

static void test_loop_tests_rotate_and_phi_copies_retarget(void)
{
    /* entry: i = 0; br cond.  cond: c = i < n; br c body, done.
     * body: t = i + 1; i = COPY t; br cond.  done: ret i */
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_rotate");
    anvil_mir_block_t entry = anvil_mir_current_block(fn);
    anvil_mir_block_t cond = anvil_mir_add_block(fn, "cond");
    anvil_mir_block_t body = anvil_mir_add_block(fn, "body");
    anvil_mir_block_t done = anvil_mir_add_block(fn, "done");
    anvil_mir_vreg_t i = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t n = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t one = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t c = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t t = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 32);
    anvil_mir_vreg_t cmp_uses[] = { i, n }, add_uses[] = { i, one }, copy_use[] = { t }, ret_use[] = { i };
    CHECK(anvil_mir_set_current_block(fn, entry) &&
          anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, i, 0) && anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, n, 10) &&
          anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, one, 1) && anvil_mir_add_branch(fn, cond) &&
          anvil_mir_set_current_block(fn, cond) && anvil_mir_add_instr(fn, ANVIL_MIR_OP_CMP_LT, c, cmp_uses, 2) &&
          anvil_mir_add_cond_branch(fn, c, body, done) && anvil_mir_set_current_block(fn, body) &&
          anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, t, add_uses, 2) && anvil_mir_add_instr(fn, ANVIL_MIR_OP_COPY, i, copy_use, 1) &&
          anvil_mir_add_branch(fn, cond) && anvil_mir_set_current_block(fn, done) &&
          anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_use, 1), "loop MachineIR should build");
    char error[192] = { 0 };
    CHECK(anvil_mir_verify(fn, error, sizeof(error)), "loop MachineIR should verify before rotation");
    CHECK(anvil_mir_duplicate_branch_targets(fn, 4) && anvil_mir_verify(fn, error, sizeof(error)), "rotation should keep valid MachineIR");
    CHECK(anvil_mir_coalesce_copies(fn) && anvil_mir_verify(fn, error, sizeof(error)), "coalescing should keep valid MachineIR");

    size_t body_branches = 0, entry_jumps = 0, copies = 0;
    bool add_writes_i = false;
    for (size_t k = 0; k < anvil_mir_num_instrs(fn); k++) {
        anvil_mir_instr_info_t info;
        if (!anvil_mir_get_instr_info(fn, k, &info))
            continue;
        if (info.block == body && info.op == ANVIL_MIR_OP_BR_COND && info.true_block == body && info.false_block == done)
            body_branches++;
        if (info.block == entry && info.op == ANVIL_MIR_OP_BR && info.true_block == cond)
            entry_jumps++;
        copies += info.op == ANVIL_MIR_OP_COPY;
        add_writes_i |= info.op == ANVIL_MIR_OP_ADD && info.def == i;
    }
    CHECK(body_branches == 1, "the latch tests the condition and branches back by itself");
    CHECK(entry_jumps == 1, "the loop entry still reaches the original test");
    CHECK(copies == 0 && add_writes_i, "the PHI copy disappears: the increment writes i directly");
    anvil_mir_func_destroy(fn);
}

static void test_linear_scan_respects_fixed_physical_registers(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_fixed");
    CHECK(fn != NULL, "MachineIR function should be created for fixed-reg test");
    if (!fn) return;

    anvil_mir_vreg_t fixed = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t normal = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t both[] = { fixed, normal };

    CHECK(anvil_mir_set_fixed_reg(fn, fixed, 0),
          "vreg should accept fixed physical register");

    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, fixed, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, normal, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, both, 2);

    anvil_regalloc_class_config_t configs[] = {
        { ANVIL_MIR_REG_GPR, 2, NULL },
    };
    CHECK(anvil_regalloc_linear_scan_classes(fn, configs, 1),
          "class-aware linear scan should allocate fixed-register vregs");

    const anvil_regalloc_assignment_t *fixed_assignment =
        anvil_mir_get_assignment(fn, fixed);
    const anvil_regalloc_assignment_t *normal_assignment =
        anvil_mir_get_assignment(fn, normal);
    CHECK(fixed_assignment && !fixed_assignment->spilled &&
          fixed_assignment->phys_reg == 0,
          "fixed vreg should keep its requested physical register");
    CHECK(normal_assignment && !normal_assignment->spilled &&
          normal_assignment->phys_reg != 0,
          "overlapping normal vreg should avoid fixed physical register");

    anvil_mir_func_destroy(fn);
}

static void test_linear_scan_moves_values_displaced_by_fixed_registers(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_fixed_displace");
    CHECK(fn != NULL, "MachineIR function should be created for displacement test");
    if (!fn) return;

    anvil_mir_vreg_t normal = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t fixed = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t both[] = { fixed, normal };
    CHECK(anvil_mir_set_fixed_reg(fn, fixed, 0), "vreg should accept fixed physical register");

    /* normal starts first and takes r0; the fixed interval then needs r0
     * while r1 is still free, so normal moves to r1 rather than spilling. */
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, normal, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, fixed, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, both, 2);

    anvil_regalloc_class_config_t configs[] = { { ANVIL_MIR_REG_GPR, 2, NULL } };
    CHECK(anvil_regalloc_linear_scan_classes(fn, configs, 1), "linear scan should allocate displaced values");
    const anvil_regalloc_assignment_t *fixed_assignment = anvil_mir_get_assignment(fn, fixed);
    const anvil_regalloc_assignment_t *normal_assignment = anvil_mir_get_assignment(fn, normal);
    CHECK(fixed_assignment && !fixed_assignment->spilled && fixed_assignment->phys_reg == 0,
          "fixed vreg should keep its requested physical register");
    CHECK(normal_assignment && !normal_assignment->spilled && normal_assignment->phys_reg == 1,
          "a displaced value should move to a free register instead of spilling");
    CHECK(anvil_mir_num_spills(fn) == 0, "displacement with a free register should not spill");
    anvil_mir_func_destroy(fn);
}

static void test_linear_scan_uses_explicit_allocatable_register_list(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_allocatable_regs");
    CHECK(fn != NULL, "MachineIR function should be created for allocatable-reg test");
    if (!fn) return;

    anvil_mir_vreg_t v0 = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t uses[] = { v0 };
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, v0, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, uses, 1);

    int gpr_regs[] = { 9 };
    anvil_regalloc_class_config_t configs[] = {
        { ANVIL_MIR_REG_GPR, 1, gpr_regs },
    };

    CHECK(anvil_regalloc_linear_scan_classes(fn, configs, 1),
          "linear scan should allocate from explicit register list");

    const anvil_regalloc_assignment_t *assignment =
        anvil_mir_get_assignment(fn, v0);
    CHECK(assignment && !assignment->spilled && assignment->phys_reg == 9,
          "allocation should use explicit physical register id");

    anvil_mir_func_destroy(fn);
}

static void test_linear_scan_reuses_expired_registers_without_spills(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_reuse");
    CHECK(fn != NULL, "MachineIR function should be created for reuse test");
    if (!fn) return;

    anvil_mir_vreg_t v0 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t v1 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t v2 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t v3 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t add_uses[] = { v0, v1 };
    anvil_mir_vreg_t add2_uses[] = { v2, v1 };
    anvil_mir_vreg_t ret_uses[] = { v3 };

    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, v0, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, v1, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, v2, add_uses, 2);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, v3, add2_uses, 2);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_uses, 1);

    CHECK(anvil_regalloc_linear_scan(fn, 3), "linear scan should allocate with 3 registers");
    CHECK(anvil_mir_num_spills(fn) == 0, "3 registers should avoid spills in reuse test");

    for (size_t i = 0; i < anvil_mir_num_vregs(fn); i++) {
        const anvil_regalloc_assignment_t *assignment =
            anvil_mir_get_assignment(fn, (anvil_mir_vreg_t)i);
        CHECK(assignment != NULL, "allocated vreg should have assignment");
        CHECK(!assignment->spilled, "reuse test vreg should not be spilled");
        CHECK(assignment->phys_reg >= 0 && assignment->phys_reg < 3,
              "physical register should be in requested range");
    }

    anvil_mir_func_destroy(fn);
}

static void test_linear_scan_marks_spills_when_pressure_exceeds_registers(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_spill");
    CHECK(fn != NULL, "MachineIR function should be created for spill test");
    if (!fn) return;

    anvil_mir_vreg_t v0 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t v1 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t v2 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t add_uses[] = { v0, v1 };
    anvil_mir_vreg_t ret_uses[] = { v2 };

    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, v0, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, v1, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, v2, add_uses, 2);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret_uses, 1);

    CHECK(anvil_regalloc_linear_scan(fn, 1), "linear scan should allocate with spills");
    CHECK(anvil_mir_num_spills(fn) > 0, "register pressure should force a spill");

    size_t valid_assignments = 0;
    for (size_t i = 0; i < anvil_mir_num_vregs(fn); i++) {
        const anvil_regalloc_assignment_t *assignment =
            anvil_mir_get_assignment(fn, (anvil_mir_vreg_t)i);
        if (!assignment) continue;
        if (assignment->spilled) {
            CHECK(assignment->spill_slot >= 0, "spilled vreg should have stack slot");
        } else {
            CHECK(assignment->phys_reg == 0, "single-register allocation should use r0");
        }
        valid_assignments++;
    }
    CHECK(valid_assignments == anvil_mir_num_vregs(fn),
          "each vreg should have an allocation result");

    anvil_mir_func_destroy(fn);
}

static void test_linear_scan_extends_loop_carried_values_to_backedge_exit(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_loop_liveness");
    CHECK(fn != NULL, "MachineIR function should be created for loop liveness test");
    if (!fn) return;

    anvil_mir_block_t entry = anvil_mir_current_block(fn);
    anvil_mir_block_t header = anvil_mir_add_block(fn, "header");
    anvil_mir_block_t body = anvil_mir_add_block(fn, "body");
    CHECK(entry != ANVIL_MIR_NO_BLOCK &&
          header != ANVIL_MIR_NO_BLOCK &&
          body != ANVIL_MIR_NO_BLOCK,
          "loop liveness blocks should be created");

    anvil_mir_vreg_t carried = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t local = anvil_mir_add_vreg(fn);

    CHECK(anvil_mir_set_current_block(fn, entry),
          "entry should be selectable for loop liveness test");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, carried, NULL, 0),
          "entry should define initial loop-carried value");
    CHECK(anvil_mir_add_branch(fn, header),
          "entry should branch to loop header");

    anvil_mir_vreg_t carried_uses[] = { carried };
    CHECK(anvil_mir_set_current_block(fn, header),
          "header should be selectable for loop liveness test");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG,
                              carried_uses, 1),
          "header should consume loop-carried value");
    CHECK(anvil_mir_add_branch(fn, body),
          "header should branch to loop body");

    anvil_mir_vreg_t local_uses[] = { local };
    CHECK(anvil_mir_set_current_block(fn, body),
          "body should be selectable for loop liveness test");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, carried, NULL, 0),
          "body should update loop-carried value for the backedge");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, local, NULL, 0),
          "body should define a local value after the backedge update");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG,
                              local_uses, 1),
          "body should consume local value while carried value is live-out");
    CHECK(anvil_mir_add_branch(fn, header),
          "body should branch back to loop header");

    CHECK(anvil_regalloc_linear_scan(fn, 1),
          "linear scan should allocate loop liveness test");
    CHECK(anvil_mir_num_spills(fn) > 0,
          "loop-carried value should remain live to the backedge block exit");

    anvil_mir_func_destroy(fn);
}

static void test_split_keeps_return_bundles_contiguous(void)
{
    char error[192] = { 0 };
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_split_ret_bundle");
    CHECK(fn != NULL, "MachineIR function should be created for split bundle test");
    if (!fn) return;

    anvil_mir_vreg_t x = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t a = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t b = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t x_uses[] = { x, x };
    anvil_mir_vreg_t xa_uses[] = { x, a };
    anvil_mir_vreg_t a_use[] = { a };
    anvil_mir_vreg_t b_use[] = { b };

    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, x, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, a, x_uses, 2);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, b, xa_uses, 2);
    anvil_mir_add_call(fn, ANVIL_MIR_NO_VREG, NULL, 0, "clobbering", ANVIL_CC_CDECL, false, 0);
    CHECK(anvil_mir_set_instr_clobbers(fn, anvil_mir_num_instrs(fn) - 1, ANVIL_MIR_REG_GPR, 1),
          "the call should clobber the only register");
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET_VALUE_PART, ANVIL_MIR_NO_VREG, a_use, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET_VALUE_PART, ANVIL_MIR_NO_VREG, b_use, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);

    CHECK(anvil_regalloc_linear_scan(fn, 1), "split allocation should succeed");
    for (size_t i = 0; i < anvil_mir_num_instrs(fn); i++) {
        anvil_mir_instr_info_t info, after;
        if (!anvil_mir_get_instr_info(fn, i, &info) || info.op != ANVIL_MIR_OP_RET_VALUE_PART)
            continue;
        anvil_mir_opcode_t next = anvil_mir_get_instr_info(fn, i + 1, &after) ? after.op : ANVIL_MIR_OP_INVALID;
        CHECK(next == ANVIL_MIR_OP_RET_VALUE_PART || next == ANVIL_MIR_OP_RET,
              "split reloads must not separate a return bundle");
    }
    CHECK(anvil_mir_verify(fn, error, sizeof(error)), "split return bundle should still verify");
    anvil_mir_func_destroy(fn);
}

/* A value redefined after a call on one path is not live across that call:
 * it keeps a register the call clobbers instead of spilling. */
static void test_linear_scan_keeps_registers_across_clobbers_in_holes(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_clobber_hole");
    CHECK(fn != NULL, "MachineIR function should be created for hole test");
    if (!fn) return;

    anvil_mir_block_t entry = anvil_mir_current_block(fn);
    anvil_mir_block_t rare = anvil_mir_add_block(fn, "rare");
    anvil_mir_block_t merge = anvil_mir_add_block(fn, "merge");
    anvil_mir_vreg_t cond = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t value_use[] = { value };
    CHECK(anvil_mir_set_current_block(fn, entry) && anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 1) &&
          anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, cond, 1) && anvil_mir_add_cond_branch(fn, cond, rare, merge),
          "hole entry should be valid");
    CHECK(anvil_mir_set_current_block(fn, rare) &&
          anvil_mir_add_call(fn, ANVIL_MIR_NO_VREG, NULL, 0, "clobbering", ANVIL_CC_CDECL, false, 0) &&
          anvil_mir_set_instr_clobbers(fn, anvil_mir_num_instrs(fn) - 1, ANVIL_MIR_REG_GPR, 3) &&
          anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 2) && anvil_mir_add_branch(fn, merge),
          "hole path should redefine the value after its call");
    CHECK(anvil_mir_set_current_block(fn, merge) && anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, value_use, 1),
          "hole merge should use the value");

    anvil_regalloc_class_config_t configs[] = { { ANVIL_MIR_REG_GPR, 2, NULL } };
    const anvil_regalloc_assignment_t *assignment = NULL;
    CHECK(anvil_regalloc_linear_scan_classes(fn, configs, 1), "hole allocation should succeed");
    assignment = anvil_mir_get_assignment(fn, value);
    CHECK(assignment && !assignment->spilled && anvil_mir_num_spills(fn) == 0,
          "a value not live across a clobber should keep its register");

    /* Live across the call (used after it, not redefined): spilled. */
    anvil_mir_func_t *live = anvil_mir_func_create("mir_clobber_live");
    anvil_mir_vreg_t kept = anvil_mir_add_vreg_ex(live, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t kept_use[] = { kept };
    CHECK(live && anvil_mir_add_instr_imm(live, ANVIL_MIR_OP_MOV, kept, 1) &&
          anvil_mir_add_call(live, ANVIL_MIR_NO_VREG, NULL, 0, "clobbering", ANVIL_CC_CDECL, false, 0) &&
          anvil_mir_set_instr_clobbers(live, anvil_mir_num_instrs(live) - 1, ANVIL_MIR_REG_GPR, 3) &&
          anvil_mir_add_instr(live, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, kept_use, 1) &&
          anvil_regalloc_linear_scan_classes(live, configs, 1),
          "live-across allocation should succeed");
    CHECK(anvil_mir_num_spills(live) == 1, "a value live across a clobber must not keep a clobbered register");

    anvil_mir_func_destroy(live);
    anvil_mir_func_destroy(fn);
}

/* Printer scratch at an instruction excludes its operands, its result and
 * the values live across it from those registers, and nothing elsewhere. */
static void test_linear_scan_keeps_operands_out_of_printer_scratch(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_printer_scratch");
    CHECK(fn != NULL, "MachineIR function should be created for scratch test");
    if (!fn) return;

    anvil_mir_vreg_t a = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t b = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t across = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t quotient = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t sum = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t after = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t divide[] = { a, b }, add[] = { quotient, across }, ret[] = { sum }, later[] = { sum, sum };
    CHECK(anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, a, 7) && anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, b, 3) &&
          anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, across, 1) && anvil_mir_add_instr(fn, ANVIL_MIR_OP_SDIV, quotient, divide, 2) &&
          anvil_mir_set_instr_scratch(fn, anvil_mir_num_instrs(fn) - 1, ANVIL_MIR_REG_GPR, 3) &&
          anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, sum, add, 2) && anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, after, later, 2) &&
          anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, ret, 1),
          "scratch function should build");

    anvil_regalloc_class_config_t configs[] = { { ANVIL_MIR_REG_GPR, 6, NULL } };
    CHECK(anvil_regalloc_linear_scan_classes(fn, configs, 1) && anvil_mir_num_spills(fn) == 0, "scratch allocation should succeed without spills");
    anvil_mir_vreg_t excluded[] = { a, b, across, quotient };
    for (size_t i = 0; i < sizeof(excluded) / sizeof(excluded[0]); i++) {
        const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(fn, excluded[i]);
        CHECK(assignment && assignment->phys_reg > 1, "operands, result and live-across values avoid the instruction's scratch");
    }
    const anvil_regalloc_assignment_t *free_after = anvil_mir_get_assignment(fn, after);
    CHECK(free_after && free_after->phys_reg <= 1, "a value away from the instruction takes its scratch registers");
    char error[160] = { 0 };
    CHECK(anvil_mir_verify(fn, error, sizeof(error)), "scratch allocation should verify");
    anvil_mir_func_destroy(fn);
}

/* A value live across a call in a cold block is saved and restored there:
 * only the copy crosses the call, and the value keeps a register the call
 * clobbers instead of being spilled for the whole function. */
static void test_cold_calls_split_values_around_them(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_cold_split");
    CHECK(fn != NULL, "MachineIR function should be created for cold split test");
    if (!fn) return;

    anvil_mir_block_t entry = anvil_mir_current_block(fn);
    anvil_mir_block_t rare = anvil_mir_add_block(fn, "rare");
    anvil_mir_block_t merge = anvil_mir_add_block(fn, "merge");
    anvil_mir_vreg_t cond = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t value = anvil_mir_add_vreg_ex(fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t value_use[] = { value };
    CHECK(anvil_mir_set_block_cold(fn, rare, true), "rare block should be marked cold");
    CHECK(anvil_mir_set_current_block(fn, entry) && anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, value, 1) &&
          anvil_mir_add_instr_imm(fn, ANVIL_MIR_OP_MOV, cond, 1) && anvil_mir_add_cond_branch(fn, cond, rare, merge),
          "cold split entry should be valid");
    CHECK(anvil_mir_set_current_block(fn, rare) &&
          anvil_mir_add_call(fn, ANVIL_MIR_NO_VREG, NULL, 0, "clobbering", ANVIL_CC_CDECL, false, 0) &&
          anvil_mir_set_instr_clobbers(fn, anvil_mir_num_instrs(fn) - 1, ANVIL_MIR_REG_GPR, 3) && anvil_mir_add_branch(fn, merge),
          "cold block should call a clobbering routine");
    CHECK(anvil_mir_set_current_block(fn, merge) && anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, value_use, 1),
          "merge should use the value after the call");

    anvil_regalloc_class_config_t configs[] = { { ANVIL_MIR_REG_GPR, 2, NULL, 0, 0, false } };
    CHECK(anvil_regalloc_linear_scan_classes(fn, configs, 1), "cold split allocation should succeed");
    const anvil_regalloc_assignment_t *assignment = anvil_mir_get_assignment(fn, value);
    CHECK(assignment && !assignment->spilled, "a value live across a cold call should keep its register");
    size_t copies = 0;
    for (size_t i = 0; i < anvil_mir_num_instrs(fn); i++) {
        anvil_mir_instr_info_t info;
        if (anvil_mir_get_instr_info(fn, i, &info) && info.op == ANVIL_MIR_OP_COPY && info.block == rare)
            copies++;
    }
    CHECK(copies == 2, "the cold block should save and restore the value around its call");
    char error[192] = { 0 };
    CHECK(anvil_mir_verify(fn, error, sizeof(error)), "cold split function should still verify");
    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_invalidates_allocations_after_new_vreg(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_invalidate");
    CHECK(fn != NULL, "MachineIR function should be created for invalidation test");
    if (!fn) return;

    anvil_mir_vreg_t v0 = anvil_mir_add_vreg(fn);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, v0, NULL, 0);

    CHECK(anvil_regalloc_linear_scan(fn, 1),
          "linear scan should allocate before invalidation");
    CHECK(anvil_mir_get_assignment(fn, v0) != NULL,
          "allocation result should exist before mutation");

    anvil_mir_vreg_t v1 = anvil_mir_add_vreg(fn);
    CHECK(v1 == 1, "new vreg should be added after allocation");
    CHECK(anvil_mir_get_assignment(fn, v0) == NULL,
          "adding a vreg should invalidate stale allocation results");

    anvil_mir_func_destroy(fn);
}

static bool assignment_uses_phys(const anvil_regalloc_assignment_t *assignment,
                                 const int *phys_regs,
                                 size_t num_phys_regs)
{
    if (!assignment || assignment->spilled) return false;

    for (size_t i = 0; i < num_phys_regs; i++) {
        if (assignment->phys_reg == phys_regs[i]) return true;
    }
    return false;
}

static void test_materialize_spills_inserts_loads_stores_and_scratch_temps(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_spill_materialize");
    CHECK(fn != NULL, "MachineIR function should be created for spill materialization");
    if (!fn) return;

    anvil_mir_vreg_t live = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t s0 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t s1 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t sum = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t all_uses[] = { live, s0, s1 };
    anvil_mir_vreg_t add_uses[] = { s0, s1 };
    anvil_mir_vreg_t ret_uses[] = { sum };

    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, live, NULL, 0),
          "spill materialization should define live value");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, s0, NULL, 0),
          "spill materialization should define first spill candidate");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, s1, NULL, 0),
          "spill materialization should define second spill candidate");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, sum, add_uses, 2),
          "spill materialization should use two spill candidates together");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG,
                              all_uses, 3),
          "spill materialization should keep all values live");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG,
                              ret_uses, 1),
          "spill materialization should return sum");

    int alloc_regs[] = { 0 };
    anvil_regalloc_class_config_t alloc_configs[] = {
        { ANVIL_MIR_REG_GPR, 1, alloc_regs },
    };
    CHECK(anvil_regalloc_linear_scan_classes(fn, alloc_configs, 1),
          "spill materialization test should allocate with spills");
    CHECK(anvil_mir_num_spills(fn) >= 2,
          "spill materialization test should force at least two spill slots");

    size_t original_instrs = anvil_mir_num_instrs(fn);
    bool spilled_before[4] = { false, false, false, false };
    for (anvil_mir_vreg_t v = 0; v < 4; v++) {
        const anvil_regalloc_assignment_t *assignment =
            anvil_mir_get_assignment(fn, v);
        spilled_before[v] = assignment && assignment->spilled;
    }

    int scratch_regs[] = { 8, 9, 10 };
    anvil_regalloc_class_config_t scratch_configs[] = {
        { ANVIL_MIR_REG_GPR, 3, scratch_regs },
    };
    CHECK(anvil_mir_materialize_spills(fn, scratch_configs, 1),
          "spill materialization should insert spill loads and stores");
    CHECK(anvil_mir_num_instrs(fn) > original_instrs,
          "spill materialization should expand the instruction stream");

    size_t spill_loads = 0;
    size_t spill_stores = 0;
    bool saw_two_adjacent_spill_loads = false;
    bool saw_store_for_spilled_def = false;

    for (size_t i = 0; i < anvil_mir_num_instrs(fn); i++) {
        anvil_mir_instr_info_t info;
        CHECK(anvil_mir_get_instr_info(fn, i, &info),
              "materialized spill instruction should be inspectable");

        if (info.op == ANVIL_MIR_OP_SPILL_LOAD) {
            spill_loads++;
            CHECK(info.spill_slot >= 0,
                  "spill load should carry a valid spill slot");
            const anvil_regalloc_assignment_t *assignment =
                anvil_mir_get_assignment(fn, info.def);
            CHECK(assignment_uses_phys(assignment, scratch_regs, 3),
                  "spill load temp should use a reserved scratch register");
            if (i + 1 < anvil_mir_num_instrs(fn)) {
                anvil_mir_instr_info_t next;
                CHECK(anvil_mir_get_instr_info(fn, i + 1, &next),
                      "next instruction after spill load should be inspectable");
                if (next.op == ANVIL_MIR_OP_SPILL_LOAD &&
                    next.block == info.block &&
                    next.def != info.def) {
                    const anvil_regalloc_assignment_t *next_assignment =
                        anvil_mir_get_assignment(fn, next.def);
                    if (assignment && next_assignment &&
                        assignment->phys_reg != next_assignment->phys_reg) {
                        saw_two_adjacent_spill_loads = true;
                    }
                }
            }
        } else if (info.op == ANVIL_MIR_OP_SPILL_STORE) {
            spill_stores++;
            CHECK(info.spill_slot >= 0,
                  "spill store should carry a valid spill slot");
            anvil_mir_vreg_t src = anvil_mir_get_instr_use(fn, i, 0);
            const anvil_regalloc_assignment_t *assignment =
                anvil_mir_get_assignment(fn, src);
            CHECK(assignment_uses_phys(assignment, scratch_regs, 3),
                  "spill store source should use a reserved scratch register");
            saw_store_for_spilled_def = true;
        } else {
            if (info.def < 4 && spilled_before[info.def]) {
                CHECK(false, "normal instruction should not define spilled original vreg");
            }
            for (size_t u = 0; u < info.num_uses; u++) {
                anvil_mir_vreg_t use = anvil_mir_get_instr_use(fn, i, u);
                if (use < 4 && spilled_before[use]) {
                    CHECK(false, "normal instruction should not use spilled original vreg");
                }
            }
        }
    }

    CHECK(spill_loads > 0, "spill materialization should emit spill loads");
    CHECK(spill_stores > 0, "spill materialization should emit spill stores");
    CHECK(saw_two_adjacent_spill_loads,
          "two spilled operands in one instruction should use distinct scratch temps");
    CHECK(saw_store_for_spilled_def,
          "spilled definitions should be stored back to their spill slots");

    for (int slot = 0; slot < (int)anvil_mir_num_spills(fn); slot++) {
        anvil_mir_spill_slot_info_t slot_info;
        CHECK(anvil_mir_get_spill_slot_info(fn, slot, &slot_info),
              "spill slot metadata should be inspectable");
        CHECK(slot_info.reg_class == ANVIL_MIR_REG_GPR &&
              slot_info.size_bits == 64,
              "GPR spill slot should preserve class and size");
    }

    anvil_mir_func_destroy(fn);
}

static void test_materialize_spills_rejects_insufficient_scratch_registers(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_spill_scratch_limit");
    CHECK(fn != NULL, "MachineIR function should be created for scratch limit test");
    if (!fn) return;

    anvil_mir_vreg_t live = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t s0 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t s1 = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t sum = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t all_uses[] = { live, s0, s1 };
    anvil_mir_vreg_t add_uses[] = { s0, s1 };

    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, live, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, s0, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, s1, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_ADD, sum, add_uses, 2);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, all_uses, 3);

    int alloc_regs[] = { 0 };
    anvil_regalloc_class_config_t alloc_configs[] = {
        { ANVIL_MIR_REG_GPR, 1, alloc_regs },
    };
    CHECK(anvil_regalloc_linear_scan_classes(fn, alloc_configs, 1),
          "scratch limit test should allocate with spills");

    size_t original_instrs = anvil_mir_num_instrs(fn);
    int scratch_regs[] = { 8 };
    anvil_regalloc_class_config_t scratch_configs[] = {
        { ANVIL_MIR_REG_GPR, 1, scratch_regs },
    };
    CHECK(!anvil_mir_materialize_spills(fn, scratch_configs, 1),
          "materialization should reject too few scratch registers");
    CHECK(anvil_mir_num_instrs(fn) == original_instrs,
          "failed materialization should leave instruction stream unchanged");

    anvil_mir_func_destroy(fn);
}

static void test_materialize_spills_counts_bundle_scratch_demand(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_spill_bundle_scratch");
    CHECK(fn != NULL, "MachineIR function should be created for bundle scratch test");
    if (!fn) return;

    anvil_mir_vreg_t live = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t a = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t b = anvil_mir_add_vreg(fn);
    anvil_mir_vreg_t all_uses[] = { live, a, b };
    anvil_mir_vreg_t a_use[] = { a };
    anvil_mir_vreg_t b_use[] = { b };

    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, live, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, a, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, b, NULL, 0);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, all_uses, 3);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET_VALUE_PART, ANVIL_MIR_NO_VREG, a_use, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET_VALUE_PART, ANVIL_MIR_NO_VREG, b_use, 1);
    anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);

    int alloc_regs[] = { 0 };
    anvil_regalloc_class_config_t alloc_configs[] = { { ANVIL_MIR_REG_GPR, 1, alloc_regs } };
    CHECK(anvil_regalloc_linear_scan_classes(fn, alloc_configs, 1), "bundle scratch test should allocate with spills");
    CHECK(anvil_mir_get_assignment(fn, a)->spilled && anvil_mir_get_assignment(fn, b)->spilled,
          "both return parts should be spilled");

    /* Each part alone needs one scratch register, but the bundle shares
     * them: two reloads cannot fit in one register. */
    size_t original_instrs = anvil_mir_num_instrs(fn);
    size_t original_vregs = anvil_mir_num_vregs(fn);
    int scratch_regs[] = { 8 };
    anvil_regalloc_class_config_t scratch_configs[] = { { ANVIL_MIR_REG_GPR, 1, scratch_regs } };
    CHECK(!anvil_mir_materialize_spills(fn, scratch_configs, 1),
          "materialization should reject a bundle that needs more scratch registers than exist");
    CHECK(anvil_mir_num_instrs(fn) == original_instrs && anvil_mir_num_vregs(fn) == original_vregs,
          "rejected materialization should leave the function unchanged");

    int two_scratch[] = { 8, 9 };
    scratch_configs[0].num_phys_regs = 2;
    scratch_configs[0].phys_regs = two_scratch;
    CHECK(anvil_mir_materialize_spills(fn, scratch_configs, 1), "two scratch registers should cover the bundle");
    anvil_mir_func_destroy(fn);
}

static void test_materialize_spills_allows_unrelated_fixed_scratch_class(void)
{
    anvil_mir_func_t *fn = anvil_mir_func_create("mir_cross_class_scratch");
    CHECK(fn != NULL, "cross-class scratch MIR should be created");
    if (!fn) return;

    anvil_mir_vreg_t callee = anvil_mir_add_vreg_ex(
        fn, ANVIL_MIR_REG_GPR, 64);
    anvil_mir_vreg_t fp = anvil_mir_add_vreg_ex(
        fn, ANVIL_MIR_REG_FPR, 32);
    anvil_mir_vreg_t fp_use[] = { fp };
    anvil_mir_vreg_t callee_use[] = { callee };
    CHECK(anvil_mir_set_fixed_reg(fn, callee, 11),
          "indirect callee should model fixed scratch GPR R11");
    CHECK(anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, callee, NULL, 0) &&
          anvil_mir_add_instr(fn, ANVIL_MIR_OP_MOV, fp, NULL, 0) &&
          anvil_mir_add_instr(fn, ANVIL_MIR_OP_KEEPALIVE,
                              ANVIL_MIR_NO_VREG, fp_use, 1) &&
          anvil_mir_add_call(fn, ANVIL_MIR_NO_VREG, callee_use, 1, NULL,
                             ANVIL_CC_SYSV, false, 0) &&
          anvil_mir_add_instr(fn, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, NULL, 0),
          "cross-class scratch regression should build its instruction stream");

    int gpr_alloc[] = { 3 };
    anvil_regalloc_class_config_t alloc[] = {
        { ANVIL_MIR_REG_GPR, 1, gpr_alloc },
        { ANVIL_MIR_REG_FPR, 0, NULL },
    };
    CHECK(anvil_regalloc_linear_scan_classes(fn, alloc, 2),
          "cross-class scratch regression should allocate with an FPR spill");
    CHECK(anvil_mir_num_spills(fn) == 1,
          "cross-class scratch regression should have exactly one FPR spill");

    int gpr_scratch[] = { 10, 11, 15 };
    int fpr_scratch[] = { 8, 9, 10 };
    anvil_regalloc_class_config_t scratch[] = {
        { ANVIL_MIR_REG_GPR, 3, gpr_scratch },
        { ANVIL_MIR_REG_FPR, 3, fpr_scratch },
    };
    CHECK(anvil_mir_materialize_spills(fn, scratch, 2),
          "a fixed scratch GPR must not block materializing an unrelated FPR spill");
    CHECK(anvil_mir_verify(fn, NULL, 0),
          "cross-class materialized spill MIR should remain structurally valid");
    anvil_mir_func_destroy(fn);
}

static void test_machine_ir_rejects_non_boolean_compare_and_conditions(void)
{
    anvil_mir_func_t *cmp = anvil_mir_func_create("bad_cmp_width");
    CHECK(cmp != NULL, "bad-width compare MIR should be created");
    if (cmp) {
        anvil_mir_vreg_t a = anvil_mir_add_vreg_ex(cmp, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t b = anvil_mir_add_vreg_ex(cmp, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t bad = anvil_mir_add_vreg_ex(cmp, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t uses[] = { a, b };
        CHECK(anvil_mir_add_instr_imm(cmp, ANVIL_MIR_OP_MOV, a, 1) &&
              anvil_mir_add_instr_imm(cmp, ANVIL_MIR_OP_MOV, b, 2) &&
              anvil_mir_add_instr(cmp, ANVIL_MIR_OP_CMP_EQ, bad, uses, 2) &&
              anvil_mir_add_instr(cmp, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "bad-width compare stream should build");
        CHECK(!anvil_mir_verify(cmp, NULL, 0),
              "integer compares must define a normalized GPR8 boolean");
        anvil_mir_func_destroy(cmp);
    }

    anvil_mir_func_t *branch = anvil_mir_func_create("bad_branch_width");
    CHECK(branch != NULL, "bad-width branch MIR should be created");
    if (branch) {
        anvil_mir_block_t entry = anvil_mir_current_block(branch);
        anvil_mir_block_t yes = anvil_mir_add_block(branch, "yes");
        anvil_mir_block_t no = anvil_mir_add_block(branch, "no");
        anvil_mir_vreg_t bad = anvil_mir_add_vreg_ex(
            branch, ANVIL_MIR_REG_GPR, 32);
        CHECK(anvil_mir_set_current_block(branch, entry) &&
              anvil_mir_add_instr_imm(branch, ANVIL_MIR_OP_MOV, bad, 1) &&
              anvil_mir_add_cond_branch(branch, bad, yes, no) &&
              anvil_mir_set_current_block(branch, yes) &&
              anvil_mir_add_instr(branch, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0) &&
              anvil_mir_set_current_block(branch, no) &&
              anvil_mir_add_instr(branch, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "bad-width branch stream should build");
        CHECK(!anvil_mir_verify(branch, NULL, 0),
              "conditional branches must consume a GPR8 boolean");
        anvil_mir_func_destroy(branch);
    }

    anvil_mir_func_t *select = anvil_mir_func_create("bad_select_width");
    CHECK(select != NULL, "bad-width select MIR should be created");
    if (select) {
        anvil_mir_vreg_t cond = anvil_mir_add_vreg_ex(
            select, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t a = anvil_mir_add_vreg_ex(select, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t b = anvil_mir_add_vreg_ex(select, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t out = anvil_mir_add_vreg_ex(select, ANVIL_MIR_REG_GPR, 32);
        anvil_mir_vreg_t uses[] = { cond, a, b };
        CHECK(anvil_mir_add_instr_imm(select, ANVIL_MIR_OP_MOV, cond, 1) &&
              anvil_mir_add_instr_imm(select, ANVIL_MIR_OP_MOV, a, 2) &&
              anvil_mir_add_instr_imm(select, ANVIL_MIR_OP_MOV, b, 3) &&
              anvil_mir_add_instr(select, ANVIL_MIR_OP_SELECT, out, uses, 3) &&
              anvil_mir_add_instr(select, ANVIL_MIR_OP_RET,
                                  ANVIL_MIR_NO_VREG, NULL, 0),
              "bad-width select stream should build");
        CHECK(!anvil_mir_verify(select, NULL, 0),
              "select must consume a GPR8 boolean condition");
        anvil_mir_func_destroy(select);
    }
}

int main(void)
{
    test_machine_ir_tracks_vregs_and_instructions();
    test_strbuf_failure_is_sticky_and_detach_is_atomic();
    test_machine_ir_tracks_blocks_and_branch_targets();
    test_machine_ir_verifier_accepts_valid_function();
    test_machine_ir_verifier_rejects_unterminated_blocks();
    test_machine_ir_verifier_rejects_register_class_mismatch();
    test_machine_ir_verifier_rejects_use_before_def();
    test_machine_ir_verifier_tracks_definitions_across_diamond();
    test_machine_ir_verifier_rejects_unreachable_and_width_mismatch();
    test_machine_ir_verifier_handles_loops_and_rejects_other();
    test_machine_ir_verifier_models_liveins_and_abi_bundles();
    test_machine_ir_coalesces_redundant_nonfixed_copies();
    test_machine_ir_coalescing_preserves_fixed_register_copies();
    test_machine_ir_coalescing_does_not_cross_nondominating_blocks();
    test_linear_scan_allocates_register_classes_independently();
    test_address_arithmetic_folds_into_accesses();
    test_local_cse_reuses_pure_values_until_redefinition();
    test_loop_tests_rotate_and_phi_copies_retarget();
    test_linear_scan_respects_fixed_physical_registers();
    test_linear_scan_moves_values_displaced_by_fixed_registers();
    test_linear_scan_uses_explicit_allocatable_register_list();
    test_linear_scan_reuses_expired_registers_without_spills();
    test_linear_scan_marks_spills_when_pressure_exceeds_registers();
    test_linear_scan_extends_loop_carried_values_to_backedge_exit();
    test_split_keeps_return_bundles_contiguous();
    test_linear_scan_keeps_registers_across_clobbers_in_holes();
    test_linear_scan_keeps_operands_out_of_printer_scratch();
    test_cold_calls_split_values_around_them();
    test_machine_ir_invalidates_allocations_after_new_vreg();
    test_materialize_spills_inserts_loads_stores_and_scratch_temps();
    test_materialize_spills_rejects_insufficient_scratch_registers();
    test_materialize_spills_counts_bundle_scratch_demand();
    test_materialize_spills_allows_unrelated_fixed_scratch_class();
    test_machine_ir_rejects_non_boolean_compare_and_conditions();

    if (failures) {
        fprintf(stderr, "%d MachineIR/regalloc regression test(s) failed\n", failures);
        return 1;
    }

    printf("MachineIR/regalloc regression tests passed\n");
    return 0;
}
