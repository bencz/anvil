/*
 * Regression tests for the x86 (32-bit) -> MachineIR lowering path.
 */

#include <anvil/anvil.h>
#include <anvil/anvil_x86_mir.h>
#include <anvil/anvil_machine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/host.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "[FAIL] %s\n", msg); \
        failures++; \
    } \
} while (0)

static anvil_ctx_t *new_x86_ctx(void)
{
    anvil_ctx_t *ctx = anvil_ctx_create();
    CHECK(ctx != NULL, "context should be created");
    if (!ctx) return NULL;

    CHECK(anvil_ctx_set_target(ctx, ANVIL_ARCH_X86) == ANVIL_OK,
          "x86 target should be available");
    return ctx;
}

static void check_contains(const char *text, const char *needle, const char *msg)
{
    CHECK(text && needle && strstr(text, needle) != NULL, msg);
}

static void assemble_check(const char *assembly, const char *message)
{
    CHECK(anvil_test_host.assemble_i386(assembly, message) <= 0, message);
}

static char *emit_func(anvil_func_t *fn)
{
    anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(fn);
    if (!mir) return NULL;
    if (!anvil_x86_regalloc_mir(mir)) {
        anvil_mir_func_destroy(mir);
        return NULL;
    }
    char *text = NULL;
    size_t len = 0;
    if (!anvil_x86_emit_mir(mir, &text, &len)) {
        anvil_mir_func_destroy(mir);
        return NULL;
    }
    anvil_mir_func_destroy(mir);
    return text;
}

static void test_cdecl_stack_args_add_ret(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_iadd");
    CHECK(mod != NULL, "module should be created");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32, i32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "iadd", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "integer add function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
            anvil_build_ret(ctx, sum);

            anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(fn);
            CHECK(mir != NULL, "integer add should lower to MachineIR");
            if (mir) {
                bool saw_stack0 = false;
                bool saw_stack4 = false;
                bool saw_add = false;
                bool saw_ret = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    CHECK(anvil_mir_get_instr_info(mir, i, &info),
                          "iadd MIR instruction should be inspectable");
                    if (info.op == ANVIL_MIR_OP_INCOMING_STACK_ARG) {
                        if (info.imm == 0) saw_stack0 = true;
                        if (info.imm == 4) saw_stack4 = true;
                    }
                    if (info.op == ANVIL_MIR_OP_ADD) saw_add = true;
                    if (info.op == ANVIL_MIR_OP_RET) saw_ret = true;
                }
                CHECK(saw_stack0,
                      "first cdecl arg should be an incoming stack arg at 0");
                CHECK(saw_stack4,
                      "second cdecl arg should be an incoming stack arg at 4");
                CHECK(saw_add, "integer add should lower to MIR ADD");
                CHECK(saw_ret, "function should lower a RET");

                CHECK(anvil_x86_regalloc_mir(mir),
                      "x86 MachineIR regalloc should succeed for integer add");

                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_emit_mir(mir, &asm_text, &asm_len),
                      "integer add MIR should emit x86 assembly");
                if (asm_text) {
                    check_contains(asm_text, "\tpushl %ebp\n",
                                   "function should emit a prologue");
                    check_contains(asm_text, "8(%ebp)",
                                   "stack args should be read off ebp");
                    check_contains(asm_text, "\tret\n",
                                   "function should emit a return");
                    assemble_check(asm_text,
                                   "integer add asm should assemble with as --32");
                    free(asm_text);
                }
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_cmp_predicates_setcc(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_cmp");
    CHECK(mod != NULL, "module should be created for compares");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *u32 = anvil_type_u32(ctx);
        anvil_type_t *params[] = { i32, i32, u32, u32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 4, false);
        anvil_func_t *fn = anvil_func_create(mod, "cmps", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "compare function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *ua = anvil_func_get_param(fn, 2);
            anvil_value_t *ub = anvil_func_get_param(fn, 3);
            anvil_value_t *lt = anvil_build_cmp_lt(ctx, a, b, "lt");
            anvil_value_t *ult = anvil_build_cmp_ult(ctx, ua, ub, "ult");
            anvil_value_t *lt_i = anvil_build_zext(ctx, lt, i32, "lt_i");
            anvil_value_t *ult_i = anvil_build_zext(ctx, ult, i32, "ult_i");
            anvil_value_t *acc = anvil_build_add(ctx, lt_i, ult_i, "acc");
            anvil_build_ret(ctx, acc);

            char *asm_text = emit_func(fn);
            CHECK(asm_text != NULL, "compare function should emit assembly");
            if (asm_text) {
                check_contains(asm_text, "setl ",
                               "signed lt should use setl");
                check_contains(asm_text, "setb ",
                               "unsigned lt should use setb");
                check_contains(asm_text, "movzbl",
                               "setcc result should be zero-extended");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_shifts_use_cl(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_shift");
    CHECK(mod != NULL, "module should be created for shifts");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32, i32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "shifter", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "shift function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *sh = anvil_build_shl(ctx, a, b, "sh");
            anvil_build_ret(ctx, sh);

            char *asm_text = emit_func(fn);
            CHECK(asm_text != NULL, "shift function should emit assembly");
            if (asm_text) {
                check_contains(asm_text, "%cl",
                               "variable shift should route count through cl");
                check_contains(asm_text, "shll",
                               "shl should emit shll");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_div_mod_eax_edx(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_div");
    CHECK(mod != NULL, "module should be created for div/mod");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *u32 = anvil_type_u32(ctx);
        anvil_type_t *params[] = { i32, i32, u32, u32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 4, false);
        anvil_func_t *fn = anvil_func_create(mod, "divmod", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "div/mod function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *ua = anvil_func_get_param(fn, 2);
            anvil_value_t *ub = anvil_func_get_param(fn, 3);
            anvil_value_t *sd = anvil_build_sdiv(ctx, a, b, "sd");
            anvil_value_t *ud = anvil_build_udiv(ctx, ua, ub, "ud");
            anvil_value_t *sm = anvil_build_smod(ctx, a, b, "sm");
            anvil_value_t *um = anvil_build_umod(ctx, ua, ub, "um");
            anvil_value_t *ud_i = anvil_build_bitcast(ctx, ud, i32, "ud_i");
            anvil_value_t *um_i = anvil_build_bitcast(ctx, um, i32, "um_i");
            anvil_value_t *acc0 = anvil_build_add(ctx, sd, ud_i, "acc0");
            anvil_value_t *acc1 = anvil_build_add(ctx, sm, um_i, "acc1");
            anvil_value_t *acc = anvil_build_add(ctx, acc0, acc1, "acc");
            anvil_build_ret(ctx, acc);

            anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(fn);
            CHECK(mir != NULL, "div/mod should lower to MachineIR");
            if (mir) {
                bool saw_sdiv = false, saw_udiv = false;
                bool saw_smod = false, saw_umod = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    anvil_mir_get_instr_info(mir, i, &info);
                    if (info.op == ANVIL_MIR_OP_SDIV) saw_sdiv = true;
                    if (info.op == ANVIL_MIR_OP_UDIV) saw_udiv = true;
                    if (info.op == ANVIL_MIR_OP_SMOD) saw_smod = true;
                    if (info.op == ANVIL_MIR_OP_UMOD) saw_umod = true;
                }
                CHECK(saw_sdiv && saw_udiv && saw_smod && saw_umod,
                      "div/mod opcodes should lower to MIR div/mod");
                CHECK(anvil_x86_regalloc_mir(mir),
                      "div/mod MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_emit_mir(mir, &asm_text, &asm_len),
                      "div/mod MIR should emit assembly");
                if (asm_text) {
                    check_contains(asm_text, "idivl",
                                   "signed division should use idivl");
                    check_contains(asm_text, "\tdivl",
                                   "unsigned division should use divl");
                    check_contains(asm_text, "cltd",
                                   "signed division should sign-extend with cltd");
                    free(asm_text);
                }
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_select_to_branch_diamond(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_select");
    CHECK(mod != NULL, "module should be created for select");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32, i32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "pick", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "select function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *cond = anvil_build_cmp_gt(ctx, a, b, "gt");
            anvil_value_t *picked = anvil_build_select(ctx, cond, a, b, "picked");
            anvil_build_ret(ctx, picked);

            anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(fn);
            CHECK(mir != NULL, "select should lower to MachineIR");
            if (mir) {
                bool saw_select = false;
                bool saw_cond_branch = false;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    anvil_mir_get_instr_info(mir, i, &info);
                    if (info.op == ANVIL_MIR_OP_SELECT) saw_select = true;
                    if (info.op == ANVIL_MIR_OP_BR_COND) saw_cond_branch = true;
                }
                CHECK(!saw_select,
                      "select must be lowered to a branch, not a MIR SELECT");
                CHECK(saw_cond_branch,
                      "select lowering should emit a conditional branch");
                CHECK(anvil_mir_num_blocks(mir) >= 4,
                      "select lowering should introduce then/else/join blocks");
                CHECK(anvil_x86_regalloc_mir(mir),
                      "select-as-branch MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_emit_mir(mir, &asm_text, &asm_len),
                      "select MIR should emit assembly");
                free(asm_text);
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_regalloc_spill_forcing(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_spill");
    CHECK(mod != NULL, "module should be created for spilling");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[8];
        for (size_t i = 0; i < 8; i++) params[i] = i32;
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 8, false);
        anvil_func_t *fn = anvil_func_create(mod, "spiller", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "spill-forcing function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *vals[8];
            for (size_t i = 0; i < 8; i++) vals[i] = anvil_func_get_param(fn, i);
            anvil_value_t *acc = anvil_build_add(ctx, vals[0], vals[1], "a0");
            for (size_t i = 2; i < 8; i++) {
                acc = anvil_build_add(ctx, acc, vals[i], "a");
            }
            for (size_t i = 0; i < 8; i++) {
                acc = anvil_build_mul(ctx, acc, vals[i], "m");
            }
            anvil_build_ret(ctx, acc);

            anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(fn);
            CHECK(mir != NULL, "spill-forcing function should lower to MIR");
            if (mir) {
                CHECK(anvil_x86_regalloc_mir(mir),
                      "regalloc must succeed with only 3 allocatable GPRs");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_emit_mir(mir, &asm_text, &asm_len),
                      "spill-forcing MIR should emit assembly");
                assemble_check(asm_text,
                               "spill-forcing asm should assemble with as --32");
                free(asm_text);
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static int assemble_link_run(const char *assembly, const char *driver, const char *message)
{
    return anvil_test_host.run_i386(assembly, driver, message);
}

static void test_sub_spilled_operands_stay_distinct(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_sub_spill");
    CHECK(mod != NULL, "module should be created for sub spill test");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32, i32, i32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 3, false);
        anvil_func_t *fn = anvil_func_create(mod, "sub_spiller", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "sub spill function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *c = anvil_func_get_param(fn, 2);

            anvil_value_t *live[8];
            for (int i = 0; i < 8; i++) {
                live[i] = anvil_build_add(ctx, a, anvil_const_i32(ctx, i + 1),
                                          "live");
            }
            anvil_value_t *d = anvil_build_sub(ctx, a, b, "d");
            anvil_value_t *acc = d;
            for (int i = 0; i < 8; i++) {
                acc = anvil_build_add(ctx, acc, live[i], "acc");
            }
            anvil_value_t *r = anvil_build_sub(ctx, acc, c, "r");
            anvil_build_ret(ctx, r);

            char *asm_text = emit_func(fn);
            CHECK(asm_text != NULL, "sub spill function should emit assembly");
            if (asm_text) {
                check_contains(asm_text, "\tsubl ",
                               "spilled sub should still emit a subl");
                assemble_check(asm_text,
                               "spilled sub asm should assemble with as --32");
                static const char driver[] =
                    "extern int sub_spiller(int, int, int);\n"
                    "int main(void){\n"
                    "  int r = sub_spiller(100, 7, 3);\n"
                    "  return r == 926 ? 0 : 1;\n"
                    "}\n";
                int rc = assemble_link_run(asm_text, driver,
                    "spilled sub should execute as a - b, not b - b");
                if (rc >= 0) {
                    CHECK(rc == 0,
                          "non-commutative sub with spilled operands must "
                          "compute a - b (first operand must not be lost)");
                }
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_fastcall_register_args_move_in_parallel(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_fastcall_parallel");
    CHECK(mod != NULL, "module should be created for fastcall parallel moves");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *pair[] = { i32, i32 };
        anvil_type_t *callee_type = anvil_type_func_cc(ctx, i32, pair, 2, false, ANVIL_CC_FASTCALL);
        anvil_func_t *callee = anvil_func_declare(mod, "fast_pair", callee_type);
        anvil_type_t *params[] = { i32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *fn = anvil_func_create(mod, "fastcall_spiller", fn_type, ANVIL_LINK_EXTERNAL);
        CHECK(callee && fn, "fastcall parallel functions should be created");
        if (callee && fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *live[8];
            for (int i = 0; i < 8; i++)
                live[i] = anvil_build_add(ctx, a, anvil_const_i32(ctx, i + 1), "live");

            /* Spilled arguments reload into EAX, ECX, EDX in order; both
             * argument orders must survive the moves into ECX/EDX. */
            anvil_value_t *forward[] = { live[6], live[7] };
            anvil_value_t *backward[] = { live[7], live[6] };
            anvil_value_t *r1 = NULL, *r2 = NULL;
            anvil_build_call_checked(ctx, anvil_func_get_value(callee), forward, 2, "r1", &r1);
            anvil_build_call_checked(ctx, anvil_func_get_value(callee), backward, 2, "r2", &r2);
            anvil_value_t *acc = anvil_build_add(ctx, anvil_build_mul(ctx, r1, anvil_const_i32(ctx, 3), "r1x3"), r2, "acc");
            for (int i = 0; i < 8; i++)
                acc = anvil_build_add(ctx, acc, live[i], "acc");
            anvil_build_ret(ctx, acc);

            char *asm_text = emit_func(fn);
            CHECK(asm_text != NULL, "fastcall parallel moves should emit assembly");
            if (asm_text) {
                static const char driver[] =
                    "__attribute__((fastcall)) int fast_pair(int x, int y) { return x * 1000 + y; }\n"
                    "extern int fastcall_spiller(int);\n"
                    "int main(void){ return fastcall_spiller(10) == 69187 ? 0 : 1; }\n";
                int rc = assemble_link_run(asm_text, driver, "fastcall register arguments should move in parallel");
                if (rc >= 0)
                    CHECK(rc == 0, "fastcall register arguments must not overwrite each other");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_atomics_execute_on_i486(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;
    CHECK(anvil_ctx_set_cpu(ctx, ANVIL_CPU_X86_I486) == ANVIL_OK, "i486 CPU model should be selectable");
    anvil_module_t *mod = anvil_module_create(ctx, "x86_atomics");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *i8 = anvil_type_i8(ctx);
    anvil_type_t *params[] = { anvil_type_ptr(ctx, i32), anvil_type_ptr(ctx, i8) };
    anvil_func_t *fn = anvil_func_create(mod, "atomics", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *word = anvil_func_get_param(fn, 0);
    anvil_value_t *byte = anvil_func_get_param(fn, 1);
    anvil_memory_order_t sc = ANVIL_ORDER_SEQ_CST;

    /* *word starts at 10, *byte at 0x0f. */
    anvil_value_t *add = anvil_build_atomic_rmw(ctx, ANVIL_ATOMIC_ADD, word, anvil_const_i32(ctx, 5), sc, "add");   /* 10 -> 15 */
    anvil_value_t *sub = anvil_build_atomic_rmw(ctx, ANVIL_ATOMIC_SUB, word, anvil_const_i32(ctx, 3), sc, "sub");   /* 15 -> 12 */
    anvil_value_t *orv = anvil_build_atomic_rmw(ctx, ANVIL_ATOMIC_OR, word, anvil_const_i32(ctx, 0x100), sc, "or"); /* 12 -> 0x10c */
    anvil_value_t *xch = anvil_build_atomic_rmw(ctx, ANVIL_ATOMIC_EXCHANGE, word, anvil_const_i32(ctx, 40), sc, "xchg"); /* 0x10c -> 40 */
    anvil_value_t *hit = anvil_build_atomic_cmpxchg(ctx, word, anvil_const_i32(ctx, 40), anvil_const_i32(ctx, 41), sc, sc, "hit");
    anvil_value_t *miss = anvil_build_atomic_cmpxchg(ctx, word, anvil_const_i32(ctx, 7), anvil_const_i32(ctx, 99), sc, sc, "miss");
    anvil_value_t *band = anvil_build_atomic_rmw(ctx, ANVIL_ATOMIC_AND, byte, anvil_const_i8(ctx, 0x3c), sc, "band"); /* 0x0f -> 0x0c */
    anvil_value_t *bxor = anvil_build_atomic_rmw(ctx, ANVIL_ATOMIC_XOR, byte, anvil_const_i8(ctx, 0x01), sc, "bxor"); /* 0x0c -> 0x0d */
    CHECK(anvil_build_atomic_fence(ctx, sc), "fence should build");
    anvil_build_atomic_store(ctx, anvil_const_i32(ctx, 1000), word, sc);
    anvil_value_t *now = anvil_build_atomic_load(ctx, word, sc, "now");

    /* now + add + sub + orv + xch + hit + miss + band + bxor */
    anvil_value_t *acc = anvil_build_add(ctx, now, add, "a1");
    acc = anvil_build_add(ctx, acc, sub, "a2");
    acc = anvil_build_add(ctx, acc, orv, "a3");
    acc = anvil_build_add(ctx, acc, xch, "a4");
    acc = anvil_build_add(ctx, acc, hit, "a5");
    acc = anvil_build_add(ctx, acc, miss, "a6");
    acc = anvil_build_add(ctx, acc, anvil_build_zext(ctx, band, i32, "b1"), "a7");
    acc = anvil_build_add(ctx, acc, anvil_build_zext(ctx, bxor, i32, "b2"), "a8");
    anvil_build_ret(ctx, acc);

    char *text = NULL;
    size_t len = 0;
    CHECK(anvil_module_codegen(mod, &text, &len) == ANVIL_OK && text, "i486 atomics should generate code");
    if (text) {
        check_contains(text, "lock cmpxchg", "i486 compare-exchange should use lock cmpxchg");
        check_contains(text, "lock xadd", "i486 fetch-add should use lock xadd");
        check_contains(text, "lock orl $0, (%esp)", "a seq_cst fence without SSE2 uses a locked or");
        /* 1000 + 10 + 15 + 12 + 0x10c + 40 + 41 + 0x0f + 0x0c = 1413 */
        static const char driver[] =
            "extern int atomics(int *, unsigned char *);\n"
            "int main(void){ int w = 10; unsigned char b = 0x0f;\n"
            "  int r = atomics(&w, &b);\n"
            "  return (r == 1413 && w == 1000 && b == 0x0d) ? 0 : 1; }\n";
        int rc = assemble_link_run(text, driver, "i486 atomics should execute");
        if (rc >= 0)
            CHECK(rc == 0, "i486 atomics must return the previous values and update memory");
        free(text);
    }
    anvil_module_destroy(mod);

    /* The i386 model has neither CMPXCHG nor XADD. */
    CHECK(anvil_ctx_set_cpu(ctx, ANVIL_CPU_X86_I386) == ANVIL_OK, "i386 CPU model should be selectable");
    CHECK(!anvil_atomic_is_lock_free(ctx, i32), "i386 has no lock-free compare-exchange");
    mod = anvil_module_create(ctx, "x86_atomics_386");
    fn = anvil_func_create(mod, "cas386", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_build_ret(ctx, anvil_build_atomic_cmpxchg(ctx, anvil_func_get_param(fn, 0), anvil_const_i32(ctx, 1), anvil_const_i32(ctx, 2), sc, sc, "cas"));
    text = NULL;
    CHECK(anvil_module_codegen(mod, &text, &len) != ANVIL_OK, "i386 compare-exchange must be rejected");
    free(text);
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_fp_add_returns_via_st0(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_fadd");
    CHECK(mod != NULL, "module should be created for FP add");
    if (mod) {
        anvil_type_t *f64 = anvil_type_f64(ctx);
        anvil_type_t *params[] = { f64, f64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, f64, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "fadd", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "FP add function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *sum = anvil_build_fadd(ctx, a, b, "sum");
            anvil_build_ret(ctx, sum);

            char *asm_text = emit_func(fn);
            CHECK(asm_text != NULL, "FP add function should emit assembly");
            if (asm_text) {
                check_contains(asm_text, "addsd",
                               "FP add should use addsd");
                check_contains(asm_text, "fldl",
                               "FP return should bridge through x87 ST0");
                assemble_check(asm_text,
                               "FP add asm should assemble with as --32");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_i64_pair_load_store_cmp(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_i64");
    CHECK(mod != NULL, "module should be created for i64 pairs");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *params[] = { i64, i64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "wide", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "i64 function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *anded = anvil_build_and(ctx, a, b, "anded");
            anvil_build_ret(ctx, anded);

            anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(fn);
            CHECK(mir != NULL, "i64 and should lower as a register pair");
            if (mir) {
                int incoming = 0;
                int and_count = 0;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    anvil_mir_get_instr_info(mir, i, &info);
                    if (info.op == ANVIL_MIR_OP_INCOMING_STACK_ARG) incoming++;
                    if (info.op == ANVIL_MIR_OP_AND) and_count++;
                }
                CHECK(incoming == 4,
                      "two i64 params should split into four incoming halves");
                CHECK(and_count == 2,
                      "i64 and should lower to two 32-bit AND ops");
                CHECK(anvil_x86_regalloc_mir(mir),
                      "i64 pair MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_emit_mir(mir, &asm_text, &asm_len),
                      "i64 pair MIR should emit assembly");
                assemble_check(asm_text,
                               "i64 pair asm should assemble with as --32");
                free(asm_text);
                anvil_mir_func_destroy(mir);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_i64_load_store_pair_offsets(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_i64_mem");
    CHECK(mod != NULL, "module should be created for i64 load/store");
    if (mod) {
        anvil_type_t *i64 = anvil_type_i64(ctx);
        anvil_type_t *p64 = anvil_type_ptr(ctx, i64);
        anvil_type_t *params[] = { p64, p64 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i64, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "wide_mem", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "i64 mem function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *pa = anvil_func_get_param(fn, 0);
            anvil_value_t *pb = anvil_func_get_param(fn, 1);
            anvil_value_t *va = anvil_build_load(ctx, i64, pa, "va");
            anvil_value_t *vb = anvil_build_load(ctx, i64, pb, "vb");
            anvil_value_t *x = anvil_build_xor(ctx, va, vb, "x");
            anvil_build_store(ctx, x, pa);
            anvil_build_ret(ctx, x);

            char *asm_text = emit_func(fn);
            CHECK(asm_text != NULL, "i64 load/store should emit assembly");
            if (asm_text) {
                /* little-endian: hi half at offset +4 from base */
                check_contains(asm_text, "4(%",
                               "i64 load/store should access hi half at +4");
                assemble_check(asm_text,
                               "i64 load/store asm should assemble with as --32");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_assemble_control_and_memory(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_asm");
    CHECK(mod != NULL, "module should be created for assemble check");
    if (!mod) {
        anvil_ctx_destroy(ctx);
        return;
    }

    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_value_t *gp = anvil_module_add_global(mod, "g_counter", i32,
                                                ANVIL_LINK_EXTERNAL);
    CHECK(gp != NULL, "global should be created");

    /* function with many stack args + cmp/branch + load/store + global ref */
    anvil_type_t *params[6];
    for (size_t i = 0; i < 6; i++) params[i] = i32;
    anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 6, false);
    anvil_func_t *fn = anvil_func_create(mod, "control_mem", fn_type,
                                         ANVIL_LINK_EXTERNAL);
    CHECK(fn != NULL, "control/mem function should be created");
    if (fn && gp) {
        anvil_block_t *entry = anvil_func_get_entry(fn);
        anvil_block_t *then_b = anvil_block_create(fn, "then");
        anvil_block_t *else_b = anvil_block_create(fn, "else");

        anvil_set_insert_point(ctx, entry);
        anvil_value_t *a = anvil_func_get_param(fn, 0);
        anvil_value_t *b = anvil_func_get_param(fn, 1);
        anvil_value_t *slot = anvil_build_alloca(ctx, i32, "slot");
        anvil_build_store(ctx, a, slot);
        anvil_value_t *loaded = anvil_build_load(ctx, i32, slot, "loaded");
        anvil_build_store(ctx, loaded, gp);
        anvil_value_t *cond = anvil_build_cmp_lt(ctx, a, b, "lt");
        anvil_build_br_cond(ctx, cond, then_b, else_b);

        anvil_set_insert_point(ctx, then_b);
        anvil_value_t *gl = anvil_build_load(ctx, i32, gp, "gl");
        anvil_build_ret(ctx, gl);

        anvil_set_insert_point(ctx, else_b);
        anvil_build_ret(ctx, b);

        char *asm_text = emit_func(fn);
        CHECK(asm_text != NULL, "control/mem function should emit assembly");
        if (asm_text) {
            check_contains(asm_text, "g_counter",
                           "global reference should appear in assembly");
            assemble_check(asm_text,
                           "control/mem asm should assemble with as --32");
            free(asm_text);
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static anvil_func_t *make_caller(anvil_ctx_t *ctx, anvil_module_t *mod,
                                 const char *name, anvil_cc_t cc)
{
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *cp[3] = { i32, i32, i32 };
    anvil_type_t *ct = anvil_type_func_cc(ctx, i32, cp, 3, false, cc);
    anvil_func_t *callee = anvil_func_declare(mod, "callee3", ct);
    anvil_type_t *params[] = { i32, i32 };
    anvil_type_t *ft = anvil_type_func_cc(ctx, i32, params, 2, false, cc);
    anvil_func_t *fn = anvil_func_create(mod, name, ft, ANVIL_LINK_EXTERNAL);
    if (!callee || !fn) return NULL;
    anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
    anvil_value_t *a = anvil_func_get_param(fn, 0);
    anvil_value_t *b = anvil_func_get_param(fn, 1);
    anvil_value_t *c = anvil_const_i32(ctx, 7);
    anvil_value_t *args[3] = { a, b, c };
    anvil_value_t *call = NULL;
    anvil_build_call_checked(ctx, anvil_func_get_value(callee), args, 3,
                             "call", &call);
    anvil_build_ret(ctx, call);
    return fn;
}

static void test_calling_conventions_and_platforms(void)
{
    struct { anvil_cc_t cc; const char *name; } ccs[] = {
        { ANVIL_CC_CDECL, "cdecl_fn" },
        { ANVIL_CC_STDCALL, "stdcall_fn" },
        { ANVIL_CC_FASTCALL, "fastcall_fn" },
    };
    anvil_abi_t abis[] = { ANVIL_ABI_SYSV, ANVIL_ABI_DARWIN, ANVIL_ABI_WIN64 };

    for (size_t c = 0; c < 3; c++) {
        for (size_t p = 0; p < 3; p++) {
            anvil_ctx_t *ctx = new_x86_ctx();
            if (!ctx) return;
            anvil_ctx_set_abi(ctx, abis[p]);
            anvil_module_t *mod = anvil_module_create(ctx, "x86_cc");
            anvil_func_t *fn = mod ? make_caller(ctx, mod, ccs[c].name, ccs[c].cc)
                                   : NULL;
            CHECK(fn != NULL, "cc/platform function should be built");
            if (fn) {
                anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(fn);
                CHECK(mir != NULL, "cc/platform should lower to MIR");
                if (mir) {
                    CHECK(anvil_x86_regalloc_mir(mir),
                          "cc/platform MIR should allocate");
                    char *t = NULL; size_t l = 0;
                    CHECK(anvil_x86_emit_mir_abi(mir, fn, abis[p],
                                                 ANVIL_SYNTAX_GAS, &t, &l),
                          "cc/platform MIR should emit assembly");
                    if (t) {
                        if (abis[p] == ANVIL_ABI_SYSV) {
                            assemble_check(t, "ELF cc asm should assemble");
                        }
                        if (ccs[c].cc == ANVIL_CC_STDCALL) {
                            check_contains(t, "ret $8",
                                           "stdcall callee should pop its args");
                        }
                        if (ccs[c].cc == ANVIL_CC_FASTCALL) {
                            check_contains(t, "%ecx",
                                           "fastcall should use ecx for an arg");
                        }
                        free(t);
                    }
                    anvil_mir_func_destroy(mir);
                }
            }
            if (mod) anvil_module_destroy(mod);
            anvil_ctx_destroy(ctx);
        }
    }
}

static char *emit_func_abi(anvil_func_t *fn, anvil_abi_t abi)
{
    anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(fn);
    char *text = NULL;
    size_t len = 0;
    if (mir && anvil_x86_regalloc_mir(mir))
        anvil_x86_emit_mir_abi(mir, fn, abi, ANVIL_SYNTAX_GAS, &text, &len);
    anvil_mir_func_destroy(mir);
    return text;
}

static void test_coff_decoration_matches_between_caller_and_callee(void)
{
    anvil_cc_t ccs[] = { ANVIL_CC_STDCALL, ANVIL_CC_FASTCALL };
    const char *expected[] = { "_take@16", "@take@16" };
    for (size_t c = 0; c < 2; c++) {
        anvil_ctx_t *ctx = new_x86_ctx();
        if (!ctx) return;
        anvil_ctx_set_abi(ctx, ANVIL_ABI_WIN64);
        anvil_module_t *mod = anvil_module_create(ctx, "x86_coff_decor");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *fields[] = { i32, i32, i32 };
        anvil_type_t *triple = anvil_type_struct(ctx, "triple", fields, 3);
        anvil_type_t *params[] = { anvil_type_ptr(ctx, triple), i32 };
        anvil_param_attr_t attrs[] = { ANVIL_PARAM_BYVAL, ANVIL_PARAM_PLAIN };
        anvil_type_t *take_type = anvil_type_func_attrs(ctx, i32, params, attrs, 2, false, ccs[c]);
        anvil_func_t *take = anvil_func_create(mod, "take", take_type, ANVIL_LINK_EXTERNAL);
        anvil_type_t *caller_params[] = { anvil_type_ptr(ctx, triple) };
        anvil_func_t *caller = anvil_func_create(mod, "give", anvil_type_func(ctx, i32, caller_params, 1, false), ANVIL_LINK_EXTERNAL);
        CHECK(take && caller, "COFF decoration functions should be created");
        if (take && caller) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(take));
            anvil_build_ret(ctx, anvil_func_get_param(take, 1));
            anvil_set_insert_point(ctx, anvil_func_get_entry(caller));
            anvil_value_t *args[] = { anvil_func_get_param(caller, 0), anvil_const_i32(ctx, 5) };
            anvil_value_t *result = NULL;
            anvil_build_call_checked(ctx, anvil_func_get_value(take), args, 2, "r", &result);
            anvil_build_ret(ctx, result);

            /* 12-byte by-value struct + one word: the callee pops 16 bytes,
             * so both sides must agree on @16. */
            char *callee_text = emit_func_abi(take, ANVIL_ABI_WIN64);
            char *caller_text = emit_func_abi(caller, ANVIL_ABI_WIN64);
            check_contains(callee_text, expected[c], "COFF callee decoration should count the by-value struct bytes");
            check_contains(caller_text, expected[c], "COFF caller decoration should match the callee");
            free(callee_text);
            free(caller_text);
        }
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

static void test_module_unwind_and_stack_directives(void)
{
    anvil_abi_t abis[] = { ANVIL_ABI_SYSV, ANVIL_ABI_WIN64 };
    for (size_t p = 0; p < 2; p++) {
        anvil_ctx_t *ctx = new_x86_ctx();
        if (!ctx) return;
        anvil_ctx_set_abi(ctx, abis[p]);
        anvil_module_t *mod = anvil_module_create(ctx, "x86_unwind");
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32 };
        anvil_func_t *fn = anvil_func_create(mod, "unwind_me", anvil_type_func(ctx, i32, params, 1, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_build_ret(ctx, anvil_build_add(ctx, anvil_func_get_param(fn, 0), anvil_const_i32(ctx, 1), "r"));
        char *text = NULL;
        size_t len = 0;
        CHECK(anvil_module_codegen(mod, &text, &len) == ANVIL_OK && text, "unwind module should generate code");
        if (text) {
            bool elf = abis[p] == ANVIL_ABI_SYSV;
            CHECK((strstr(text, ".cfi_startproc") != NULL) == elf && (strstr(text, ".cfi_endproc") != NULL) == elf,
                  "ELF functions carry DWARF CFI; COFF ones do not");
            CHECK((strstr(text, ".note.GNU-stack") != NULL) == elf, "ELF modules mark the stack non-executable");
            if (elf)
                assemble_check(text, "CFI module should assemble with as --32");
            free(text);
        }
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

static void test_constant_and_common_globals_use_their_sections(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;
    anvil_module_t *mod = anvil_module_create(ctx, "x86_sections");
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_value_t *table = anvil_module_add_global(mod, "ro_table", i32, ANVIL_LINK_EXTERNAL);
    anvil_value_t *pointer = anvil_module_add_global(mod, "ro_pointer", anvil_type_ptr(ctx, i32), ANVIL_LINK_EXTERNAL);
    anvil_value_t *common = anvil_module_add_global(mod, "tentative", i32, ANVIL_LINK_COMMON);
    CHECK(table && pointer && common, "section globals should be created");
    CHECK(anvil_global_set_initializer(table, anvil_const_i32(ctx, 7)) && anvil_global_set_constant(table, true) &&
          anvil_global_set_initializer(pointer, anvil_const_symbol_addr(table)) &&
          anvil_global_set_constant(pointer, true) && anvil_global_is_constant(table) && !anvil_global_is_constant(common),
          "globals should accept the constant attribute");
    anvil_value_t *declared = anvil_module_add_extern(mod, "elsewhere", i32);
    CHECK(declared && !anvil_global_set_constant(declared, true), "a declaration cannot be marked constant");
    char *text = NULL;
    size_t len = 0;
    CHECK(anvil_module_codegen(mod, &text, &len) == ANVIL_OK && text, "section module should generate code");
    if (text) {
        const char *rodata = strstr(text, ".section .rodata\n");
        const char *relro = strstr(text, ".section .data.rel.ro");
        CHECK(rodata && strstr(rodata, "ro_table:"), "a constant without relocations goes to .rodata");
        CHECK(relro && strstr(relro, "ro_pointer:"), "a constant holding an address goes to .data.rel.ro");
        check_contains(text, ".comm tentative, 4, 4", "an uninitialized COMMON global is a .comm symbol");
        assemble_check(text, "section module should assemble with as --32");
        free(text);
    }
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_setcc_boolean_in_esi_edi(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_setcc_esi_edi");
    CHECK(mod != NULL, "module should be created for setcc esi/edi");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { i32, i32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "esi_edi_max", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "setcc esi/edi function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *c = anvil_build_cmp_gt(ctx, a, b, "c");
            anvil_value_t *m = anvil_build_select(ctx, c, a, b, "m");
            anvil_build_ret(ctx, m);

            char *asm_text = emit_func(fn);
            CHECK(asm_text != NULL, "setcc esi/edi MIR should emit assembly");
            if (asm_text) {
                CHECK(strstr(asm_text, "%dil") == NULL,
                      "32-bit emitter must not produce %dil");
                CHECK(strstr(asm_text, "%sil") == NULL,
                      "32-bit emitter must not produce %sil");
                CHECK(strstr(asm_text, "%spl") == NULL,
                      "32-bit emitter must not produce %spl");
                CHECK(strstr(asm_text, "%bpl") == NULL,
                      "32-bit emitter must not produce %bpl");
                /* The compare feeds the select's branch: it branches on the
                 * flags instead of materializing a boolean. */
                CHECK(strstr(asm_text, "\tjg ") != NULL || strstr(asm_text, "\tjle ") != NULL,
                      "the compare must branch on its flags");
                assemble_check(asm_text,
                               "setcc-derived boolean asm should assemble with as --32");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_byte_compare_fixed_eax_result_survives_scratch_restore(void)
{
    anvil_mir_func_t *mir = anvil_mir_func_create("fixed_eax_cmp");
    CHECK(mir != NULL, "fixed-EAX compare MIR should be created");
    if (!mir) return;
    anvil_mir_vreg_t lhs = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t rhs = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t dst = anvil_mir_add_vreg_ex(mir, ANVIL_MIR_REG_GPR, 8);
    anvil_mir_vreg_t uses[] = { lhs, rhs };
    /* Bytes live only in registers with a byte form (EBX, ECX here). */
    CHECK(anvil_mir_set_fixed_reg(mir, lhs, 3) &&
          anvil_mir_set_fixed_reg(mir, rhs, 1) &&
          anvil_mir_set_fixed_reg(mir, dst, 0) &&
          anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, lhs, 2) &&
          anvil_mir_add_instr_imm(mir, ANVIL_MIR_OP_MOV, rhs, 1) &&
          anvil_mir_add_instr(mir, ANVIL_MIR_OP_CMP_GT, dst, uses, 2) &&
          anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET_VALUE_PART,
                              ANVIL_MIR_NO_VREG, &dst, 1) &&
          anvil_mir_add_instr(mir, ANVIL_MIR_OP_RET,
                              ANVIL_MIR_NO_VREG, NULL, 0),
          "fixed-EAX compare stream should build");
    CHECK(anvil_x86_regalloc_mir(mir), "fixed-EAX compare should allocate");
    char *text = NULL;
    size_t len = 0;
    CHECK(anvil_x86_emit_mir(mir, &text, &len),
          "fixed-EAX compare should emit");
    if (text) {
        CHECK(strstr(text, "\tcmpb $1, %bl\n") != NULL && strstr(text, "\tsetg %al\n") != NULL,
              "the byte compare must use the byte registers directly and set EAX's byte");
        assemble_check(text, "fixed-EAX byte compare should assemble with as --32");
        free(text);
    }
    anvil_mir_func_destroy(mir);
}

static void test_i8_byte_ops_no_high_byte_regs(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_i8_byte_ops");
    CHECK(mod != NULL, "module should be created for i8 byte ops");
    if (mod) {
        anvil_type_t *i8 = anvil_type_i8(ctx);
        anvil_type_t *params[] = { i8, i8, i8, i8 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i8, params, 4, false);
        anvil_func_t *fn = anvil_func_create(mod, "i8_ops", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "i8 byte ops function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *c = anvil_func_get_param(fn, 2);
            anvil_value_t *d = anvil_func_get_param(fn, 3);
            anvil_value_t *sum = anvil_build_add(ctx, a, b, "sum");
            anvil_value_t *both = anvil_build_and(ctx, sum, c, "both");
            anvil_value_t *sh = anvil_build_shl(ctx, both, d, "sh");
            anvil_value_t *cmp = anvil_build_cmp_lt(ctx, sh, d, "cmp");
            anvil_value_t *pick = anvil_build_select(ctx, cmp, sh, c, "pick");
            anvil_build_ret(ctx, pick);

            char *asm_text = emit_func(fn);
            CHECK(asm_text != NULL, "i8 byte ops MIR should emit assembly");
            if (asm_text) {
                CHECK(strstr(asm_text, "%dil") == NULL,
                      "i8 ops must not produce %dil");
                CHECK(strstr(asm_text, "%sil") == NULL,
                      "i8 ops must not produce %sil");
                CHECK(strstr(asm_text, "%spl") == NULL,
                      "i8 ops must not produce %spl");
                CHECK(strstr(asm_text, "%bpl") == NULL,
                      "i8 ops must not produce %bpl");
                assemble_check(asm_text,
                               "i8 byte-op asm should assemble with as --32");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_local_array_i8_store_address(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_arr_i8");
    CHECK(mod != NULL, "module should be created for local array i8 store");
    if (mod) {
        anvil_type_t *i8 = anvil_type_i8(ctx);
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *arr = anvil_type_array(ctx, i8, 4);
        anvil_type_t *params[] = { i8, i8 };
        anvil_type_t *fn_type = anvil_type_func(ctx, i32, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "arr_i8", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "local array i8 store function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *buf = anvil_build_alloca(ctx, arr, "buf");
            anvil_value_t *idx0[] = { anvil_const_i32(ctx, 0),
                                      anvil_const_i32(ctx, 0) };
            anvil_value_t *idx1[] = { anvil_const_i32(ctx, 0),
                                      anvil_const_i32(ctx, 1) };
            anvil_value_t *p0 = anvil_build_gep(ctx, arr, buf, idx0, 2, "p0");
            anvil_value_t *p1 = anvil_build_gep(ctx, arr, buf, idx1, 2, "p1");
            anvil_build_store(ctx, a, p0);
            anvil_build_store(ctx, b, p1);
            anvil_value_t *l0 = anvil_build_load(ctx, i8, p0, "l0");
            anvil_value_t *l1 = anvil_build_load(ctx, i8, p1, "l1");
            anvil_value_t *e0 = anvil_build_sext(ctx, l0, i32, "e0");
            anvil_value_t *e1 = anvil_build_sext(ctx, l1, i32, "e1");
            anvil_value_t *sum = anvil_build_add(ctx, e0, e1, "sum");
            anvil_build_ret(ctx, sum);

            char *asm_text = emit_func(fn);
            CHECK(asm_text != NULL, "local array i8 store should emit assembly");
            if (asm_text) {
                check_contains(asm_text, "movb",
                               "i8 store into local array should use movb");
                check_contains(asm_text, "leal",
                               "local array address should come from leal");
                assemble_check(asm_text,
                               "local array i8 store asm should assemble");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

static void test_small_struct_return_pair(void)
{
    anvil_ctx_t *ctx = new_x86_ctx();
    if (!ctx) return;

    anvil_module_t *mod = anvil_module_create(ctx, "x86_small_struct");
    CHECK(mod != NULL, "module should be created for small struct return");
    if (mod) {
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *fields[] = { i32, i32 };
        anvil_type_t *pt = anvil_type_struct(ctx, "Pt", fields, 2);
        anvil_type_t *ppt = anvil_type_ptr(ctx, pt);
        anvil_type_t *params[] = { i32, i32 };
        anvil_type_t *fn_type = anvil_type_func(ctx, pt, params, 2, false);
        anvil_func_t *fn = anvil_func_create(mod, "mk_pt", fn_type,
                                             ANVIL_LINK_EXTERNAL);
        CHECK(fn != NULL, "small struct return function should be created");
        if (fn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
            anvil_value_t *a = anvil_func_get_param(fn, 0);
            anvil_value_t *b = anvil_func_get_param(fn, 1);
            anvil_value_t *p = anvil_build_alloca(ctx, pt, "p");
            anvil_value_t *fx = anvil_build_struct_gep(ctx, pt, p, 0, "fx");
            anvil_value_t *fy = anvil_build_struct_gep(ctx, pt, p, 1, "fy");
            anvil_build_store(ctx, a, fx);
            anvil_build_store(ctx, b, fy);
            anvil_value_t *v = anvil_build_load(ctx, pt, p, "v");
            anvil_build_ret(ctx, v);

            anvil_mir_func_t *mir = anvil_x86_lower_func_to_mir(fn);
            CHECK(mir != NULL, "small struct return should lower as a pair");
            if (mir) {
                int load_count = 0;
                int ret_count = 0;
                for (size_t i = 0; i < anvil_mir_num_instrs(mir); i++) {
                    anvil_mir_instr_info_t info;
                    anvil_mir_get_instr_info(mir, i, &info);
                    if (info.op == ANVIL_MIR_OP_LOAD) load_count++;
                    if (info.op == ANVIL_MIR_OP_RET) ret_count++;
                }
                CHECK(load_count >= 2,
                      "8-byte struct load should split into two 32-bit loads");
                CHECK(ret_count == 1, "function should have one ret");
                CHECK(anvil_x86_regalloc_mir(mir),
                      "small struct pair MIR should allocate");
                char *asm_text = NULL;
                size_t asm_len = 0;
                CHECK(anvil_x86_emit_mir(mir, &asm_text, &asm_len),
                      "small struct pair MIR should emit assembly");
                check_contains(asm_text, "4(%",
                               "struct hi half should be accessed at +4");
                assemble_check(asm_text,
                               "small struct return asm should assemble");
                free(asm_text);
                anvil_mir_func_destroy(mir);
            }
        }

        anvil_type_t *sum_params[] = { ppt };
        anvil_type_t *sum_type = anvil_type_func(ctx, i32, sum_params, 1, false);
        anvil_func_t *sumfn = anvil_func_create(mod, "sum_pt", sum_type,
                                                ANVIL_LINK_EXTERNAL);
        if (sumfn) {
            anvil_set_insert_point(ctx, anvil_func_get_entry(sumfn));
            anvil_value_t *pp = anvil_func_get_param(sumfn, 0);
            anvil_value_t *gx = anvil_build_struct_gep(ctx, pt, pp, 0, "gx");
            anvil_value_t *gy = anvil_build_struct_gep(ctx, pt, pp, 1, "gy");
            anvil_value_t *vx = anvil_build_load(ctx, i32, gx, "vx");
            anvil_value_t *vy = anvil_build_load(ctx, i32, gy, "vy");
            anvil_value_t *s = anvil_build_add(ctx, vx, vy, "s");
            anvil_build_ret(ctx, s);

            char *asm_text = emit_func(sumfn);
            CHECK(asm_text != NULL, "struct consumer should emit assembly");
            if (asm_text) {
                assemble_check(asm_text,
                               "struct consumer asm should assemble");
                free(asm_text);
            }
        }
    }

    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
}

/* The CPU model picks the floating-point unit: SSE2 registers on the
 * Pentium 4, the x87 FPU before it (FUCOMIP from the P6 on, FNSTSW/SAHF
 * earlier), with the stack empty between instructions. */
static void test_fp_unit_follows_cpu_model(void)
{
    static const struct {
        anvil_cpu_model_t cpu;
        const char *present;
        const char *absent;
        const char *compare;
    } cases[] = {
        { ANVIL_CPU_X86_I486, "fldl", "xmm", "fnstsw %ax" },
        { ANVIL_CPU_X86_PENTIUM_PRO, "fldl", "xmm", "fucomip" },
        { ANVIL_CPU_X86_PENTIUM4, "xmm", "fucom", "ucomisd" },
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        anvil_ctx_t *ctx = new_x86_ctx();
        if (!ctx) return;
        CHECK(anvil_ctx_set_cpu(ctx, cases[c].cpu) == ANVIL_OK, "i386 CPU model should be selectable");
        anvil_module_t *mod = anvil_module_create(ctx, "fp_unit");
        anvil_type_t *f64 = anvil_type_f64(ctx);
        anvil_type_t *i32 = anvil_type_i32(ctx);
        anvil_type_t *params[] = { f64, i32 };
        anvil_func_t *fn = anvil_func_create(mod, "fp_mix", anvil_type_func(ctx, i32, params, 2, false), ANVIL_LINK_EXTERNAL);
        anvil_set_insert_point(ctx, anvil_func_get_entry(fn));
        anvil_value_t *x = anvil_func_get_param(fn, 0);
        anvil_value_t *sum = anvil_build_fadd(ctx, x, anvil_build_sitofp(ctx, anvil_func_get_param(fn, 1), f64, "n"), "sum");
        anvil_value_t *less = anvil_build_fcmp(ctx, ANVIL_FCMP_OLT, sum, x, "less");
        anvil_value_t *pick = anvil_build_select(ctx, less, anvil_build_fneg(ctx, sum, "neg"), sum, "pick");
        CHECK(anvil_build_ret(ctx, anvil_build_fptosi(ctx, pick, i32, "result")), "FP function should build");

        char *text = NULL;
        size_t len = 0;
        CHECK(anvil_module_codegen(mod, &text, &len) == ANVIL_OK && text, "FP function should emit for every i386 CPU model");
        if (text) {
            check_contains(text, cases[c].present, "the FP unit should match the CPU model");
            CHECK(strstr(text, cases[c].absent) == NULL, "the other FP unit's instructions must not appear");
            check_contains(text, cases[c].compare, "the FP compare should match the CPU model");
            assemble_check(text, "FP code should assemble for every i386 CPU model");
        }
        free(text);
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
    }
}

int main(void)
{
    test_cdecl_stack_args_add_ret();
    test_assemble_control_and_memory();
    test_calling_conventions_and_platforms();
    test_coff_decoration_matches_between_caller_and_callee();
    test_module_unwind_and_stack_directives();
    test_constant_and_common_globals_use_their_sections();
    test_cmp_predicates_setcc();
    test_shifts_use_cl();
    test_div_mod_eax_edx();
    test_select_to_branch_diamond();
    test_regalloc_spill_forcing();
    test_sub_spilled_operands_stay_distinct();
    test_fastcall_register_args_move_in_parallel();
    test_atomics_execute_on_i486();
    test_fp_add_returns_via_st0();
    test_i64_pair_load_store_cmp();
    test_i64_load_store_pair_offsets();
    test_setcc_boolean_in_esi_edi();
    test_byte_compare_fixed_eax_result_survives_scratch_restore();
    test_i8_byte_ops_no_high_byte_regs();
    test_local_array_i8_store_address();
    test_small_struct_return_pair();
    test_fp_unit_follows_cpu_model();

    if (failures) {
        fprintf(stderr, "%d x86 MIR lowering test(s) failed\n", failures);
        return 1;
    }

    printf("x86 MIR lowering tests passed\n");
    return 0;
}
