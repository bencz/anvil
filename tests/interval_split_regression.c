#include <anvil/anvil_machine.h>
#include <stdio.h>

static bool run_case(anvil_mir_reg_class_t reg_class, bool cross_block)
{
    anvil_mir_func_t *func = anvil_mir_func_create("split_intervals");
    anvil_mir_vreg_t left = anvil_mir_add_vreg_typed(func, reg_class, 64, false);
    anvil_mir_vreg_t right = anvil_mir_add_vreg_typed(func, reg_class, 64, false);
    anvil_mir_vreg_t value = anvil_mir_add_vreg_typed(func, reg_class, 64, false);
    anvil_mir_set_live_in(func, left, true);
    anvil_mir_set_live_in(func, right, true);
    anvil_mir_set_fixed_reg(func, left, 1);
    anvil_mir_set_fixed_reg(func, right, 2);
    anvil_mir_vreg_t operands[] = { left, right };
    anvil_mir_add_instr(func, ANVIL_MIR_OP_ADD, value, operands, 2);
    for (unsigned use = 0; use < 3; use++)
        anvil_mir_add_instr(func, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, &value, 1);

    for (unsigned call = 0; call < 2; call++)
    {
        size_t index = anvil_mir_num_instrs(func);
        anvil_mir_add_call(func, ANVIL_MIR_NO_VREG, NULL, 0, "clobber", ANVIL_CC_SYSV, false, 0);
        anvil_mir_set_instr_clobbers(func, index, reg_class, 1);
        if (cross_block && call == 0)
        {
            anvil_mir_block_t source = anvil_mir_current_block(func);
            anvil_mir_block_t next = anvil_mir_add_block(func, "next");
            anvil_mir_set_current_block(func, source);
            anvil_mir_add_branch(func, next);
            anvil_mir_set_current_block(func, next);
        }

        for (unsigned use = 0; use < 3; use++)
            anvil_mir_add_instr(func, ANVIL_MIR_OP_KEEPALIVE, ANVIL_MIR_NO_VREG, &value, 1);
    }

    anvil_mir_add_instr(func, ANVIL_MIR_OP_RET, ANVIL_MIR_NO_VREG, NULL, 0);
    int physical[] = { 0 };
    anvil_regalloc_class_config_t config = { reg_class, 1, physical };
    bool valid = anvil_regalloc_linear_scan_classes(func, &config, 1);
    char error[256] = { 0 };
    valid &= anvil_mir_verify(func, error, sizeof(error));
    size_t loads = 0;
    size_t stores = 0;
    for (size_t index = 0; index < anvil_mir_num_instrs(func); index++)
    {
        anvil_mir_instr_info_t instruction;
        anvil_mir_get_instr_info(func, index, &instruction);
        loads += instruction.op == ANVIL_MIR_OP_SPILL_LOAD;
        stores += instruction.op == ANVIL_MIR_OP_SPILL_STORE;
    }

    valid &= loads == (cross_block ? 0 : 2) && stores == (cross_block ? 0 : 1);
    int scratch[] = { 3, 4, 5 };
    anvil_regalloc_class_config_t scratch_config = { reg_class, 3, scratch };
    valid &= anvil_mir_materialize_spills(func, &scratch_config, 1);
    valid &= anvil_mir_verify(func, error, sizeof(error));
    if (!valid)
        fprintf(stderr, "splitting failure: class=%d cross_block=%d loads=%zu stores=%zu error=%s\n", reg_class, cross_block, loads, stores, error);

    anvil_mir_func_destroy(func);
    return valid;
}

int main(void)
{
    return run_case(ANVIL_MIR_REG_GPR, false) && run_case(ANVIL_MIR_REG_FPR, false) &&
           run_case(ANVIL_MIR_REG_GPR, true) && run_case(ANVIL_MIR_REG_FPR, true) ? 0 : 1;
}
