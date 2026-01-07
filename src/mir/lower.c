#include "lower.h"
#include "../core/str.h"
#include "../backend/backend.h"
#include <string.h>
#include <stdio.h>

static int get_type_size(AnvilType* type) {
    if (!type) return 8;
    return (int)type->size;
}

static void ctx_init(AnvilLowerCtx* ctx, AnvilModule* mod) {
    ctx->arena = mod->arena;
    ctx->ir_module = mod;
    ctx->mir = anvil_mir_create(mod->arena);
    ctx->current_func = NULL;
    ctx->current_block = NULL;
    anvil_hash_init(&ctx->value_map);
    anvil_hash_init(&ctx->block_map);
    ctx->next_vreg = 1;
    ctx->next_vreg_fp = 1;
    ctx->abi = NULL;
    ctx->ret_reg_int = 0;
    ctx->ret_reg_fp = 0;
}

static void ctx_free(AnvilLowerCtx* ctx) {
    anvil_hash_free(&ctx->value_map);
    anvil_hash_free(&ctx->block_map);
}

static void emit(AnvilLowerCtx* ctx, AnvilMInst* inst) {
    anvil_mblock_append(ctx->current_block, inst);
}

static bool is_fp_type(AnvilType* type) {
    return type && (type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64);
}

int anvil_lower_value(AnvilLowerCtx* ctx, AnvilValue* val) {
    if (!val) return -1;
    
    char key[32];
    snprintf(key, sizeof(key), "v%u", val->id);
    void* existing = anvil_hash_get(&ctx->value_map, key);
    if (existing) {
        return (int)(intptr_t)existing;
    }
    
    int vreg = ctx->next_vreg++;
    char* stored_key = anvil_arena_strdup(ctx->arena, key);
    anvil_hash_insert(&ctx->value_map, stored_key, (void*)(intptr_t)vreg);
    
    if (anvil_value_is_const_int(val)) {
        int size = get_type_size(val->type);
        AnvilMOperand dst = anvil_mop_vreg(vreg, size);
        AnvilMInst* mov = anvil_minst_mov_imm(ctx->arena, dst, val->i64, size);
        emit(ctx, mov);
    } else if (val->kind == ANVIL_VALUE_CONST_FLOAT) {
        int size = get_type_size(val->type);
        AnvilMOperand dst = anvil_mop_vreg_fp(vreg, size);
        AnvilMInst* mov = anvil_minst_mov_imm(ctx->arena, dst, 0, size);
        mov->operands[0].is_fp = true;
        emit(ctx, mov);
    }
    
    return vreg;
}

AnvilMOperand anvil_lower_to_operand(AnvilLowerCtx* ctx, AnvilValue* val) {
    if (!val) return anvil_mop_none();
    
    int size = get_type_size(val->type);
    bool is_fp = is_fp_type(val->type);
    
    if (anvil_value_is_const_int(val)) {
        return anvil_mop_imm(val->i64, size);
    }
    
    int vreg = anvil_lower_value(ctx, val);
    return is_fp ? anvil_mop_vreg_fp(vreg, size) : anvil_mop_vreg(vreg, size);
}

static AnvilCondCode inst_to_cc(AnvilInstKind kind, bool is_signed) {
    switch (kind) {
        case ANVIL_INST_EQ: return ANVIL_CC_EQ;
        case ANVIL_INST_NE: return ANVIL_CC_NE;
        case ANVIL_INST_LT: return is_signed ? ANVIL_CC_LT : ANVIL_CC_ULT;
        case ANVIL_INST_LE: return is_signed ? ANVIL_CC_LE : ANVIL_CC_ULE;
        case ANVIL_INST_GT: return is_signed ? ANVIL_CC_GT : ANVIL_CC_UGT;
        case ANVIL_INST_GE: return is_signed ? ANVIL_CC_GE : ANVIL_CC_UGE;
        default: return ANVIL_CC_EQ;
    }
}

void anvil_lower_inst(AnvilLowerCtx* ctx, AnvilInst* inst) {
    AnvilMInstKind mir_kind;
    
    switch (inst->kind) {
        case ANVIL_INST_NOP:
            emit(ctx, anvil_minst_create(ctx->arena, ANVIL_MIR_NOP));
            break;
            
        case ANVIL_INST_ADD:
        case ANVIL_INST_SUB:
        case ANVIL_INST_MUL:
        case ANVIL_INST_DIV:
        case ANVIL_INST_MOD:
        case ANVIL_INST_AND:
        case ANVIL_INST_OR:
        case ANVIL_INST_XOR:
        case ANVIL_INST_SHL:
        case ANVIL_INST_SHR:
        case ANVIL_INST_SAR: {
            switch (inst->kind) {
                case ANVIL_INST_ADD: mir_kind = ANVIL_MIR_ADD; break;
                case ANVIL_INST_SUB: mir_kind = ANVIL_MIR_SUB; break;
                case ANVIL_INST_MUL: mir_kind = ANVIL_MIR_MUL; break;
                case ANVIL_INST_DIV: mir_kind = anvil_type_is_signed(inst->operands[0]->type) ? ANVIL_MIR_IDIV : ANVIL_MIR_DIV; break;
                case ANVIL_INST_MOD: mir_kind = ANVIL_MIR_MOD; break;
                case ANVIL_INST_AND: mir_kind = ANVIL_MIR_AND; break;
                case ANVIL_INST_OR: mir_kind = ANVIL_MIR_OR; break;
                case ANVIL_INST_XOR: mir_kind = ANVIL_MIR_XOR; break;
                case ANVIL_INST_SHL: mir_kind = ANVIL_MIR_SHL; break;
                case ANVIL_INST_SHR: mir_kind = ANVIL_MIR_SHR; break;
                case ANVIL_INST_SAR: mir_kind = ANVIL_MIR_SAR; break;
                default: mir_kind = ANVIL_MIR_NOP; break;
            }
            
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand lhs = anvil_lower_to_operand(ctx, inst->operands[0]);
            AnvilMOperand rhs = anvil_lower_to_operand(ctx, inst->operands[1]);
            int size = get_type_size(inst->result->type);
            AnvilMOperand dst = anvil_mop_vreg(dst_vreg, size);
            
            AnvilMInst* mov = anvil_minst_mov(ctx->arena, dst, lhs);
            emit(ctx, mov);
            
            AnvilMInst* op = anvil_minst_binary(ctx->arena, mir_kind, dst, rhs);
            emit(ctx, op);
            break;
        }
        
        case ANVIL_INST_FADD:
        case ANVIL_INST_FSUB:
        case ANVIL_INST_FMUL:
        case ANVIL_INST_FDIV: {
            switch (inst->kind) {
                case ANVIL_INST_FADD: mir_kind = ANVIL_MIR_FADD; break;
                case ANVIL_INST_FSUB: mir_kind = ANVIL_MIR_FSUB; break;
                case ANVIL_INST_FMUL: mir_kind = ANVIL_MIR_FMUL; break;
                case ANVIL_INST_FDIV: mir_kind = ANVIL_MIR_FDIV; break;
                default: mir_kind = ANVIL_MIR_NOP; break;
            }
            
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand lhs = anvil_lower_to_operand(ctx, inst->operands[0]);
            AnvilMOperand rhs = anvil_lower_to_operand(ctx, inst->operands[1]);
            int size = get_type_size(inst->result->type);
            AnvilMOperand dst = anvil_mop_vreg_fp(dst_vreg, size);
            
            AnvilMInst* mov = anvil_minst_mov(ctx->arena, dst, lhs);
            mov->operands[0].is_fp = true;
            mov->operands[1].is_fp = true;
            emit(ctx, mov);
            
            AnvilMInst* op = anvil_minst_binary(ctx->arena, mir_kind, dst, rhs);
            op->operands[0].is_fp = true;
            op->operands[1].is_fp = true;
            emit(ctx, op);
            break;
        }
        
        case ANVIL_INST_FNEG:
        case ANVIL_INST_FABS:
        case ANVIL_INST_FSQRT: {
            switch (inst->kind) {
                case ANVIL_INST_FNEG: mir_kind = ANVIL_MIR_FNEG; break;
                case ANVIL_INST_FABS: mir_kind = ANVIL_MIR_FABS; break;
                case ANVIL_INST_FSQRT: mir_kind = ANVIL_MIR_FSQRT; break;
                default: mir_kind = ANVIL_MIR_FNEG; break;
            }
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand src = anvil_lower_to_operand(ctx, inst->operands[0]);
            int size = get_type_size(inst->result->type);
            AnvilMOperand dst = anvil_mop_vreg_fp(dst_vreg, size);
            
            AnvilMInst* op = anvil_minst_create(ctx->arena, mir_kind);
            op->operands[0] = dst;
            op->operands[1] = src;
            op->num_operands = 2;
            op->operands[0].is_fp = true;
            op->operands[1].is_fp = true;
            emit(ctx, op);
            break;
        }
        
        case ANVIL_INST_FMIN:
        case ANVIL_INST_FMAX: {
            mir_kind = inst->kind == ANVIL_INST_FMIN ? ANVIL_MIR_FMIN : ANVIL_MIR_FMAX;
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand lhs = anvil_lower_to_operand(ctx, inst->operands[0]);
            AnvilMOperand rhs = anvil_lower_to_operand(ctx, inst->operands[1]);
            int size = get_type_size(inst->result->type);
            AnvilMOperand dst = anvil_mop_vreg_fp(dst_vreg, size);
            
            AnvilMInst* op = anvil_minst_create_with_capacity(ctx->arena, mir_kind, 3);
            op->operands[0] = dst;
            op->operands[1] = lhs;
            op->operands[2] = rhs;
            op->num_operands = 3;
            op->operands[0].is_fp = true;
            op->operands[1].is_fp = true;
            op->operands[2].is_fp = true;
            emit(ctx, op);
            break;
        }
        
        case ANVIL_INST_FMADD:
        case ANVIL_INST_FMSUB: {
            mir_kind = inst->kind == ANVIL_INST_FMADD ? ANVIL_MIR_FMADD : ANVIL_MIR_FMSUB;
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand a = anvil_lower_to_operand(ctx, inst->operands[0]);
            AnvilMOperand b = anvil_lower_to_operand(ctx, inst->operands[1]);
            AnvilMOperand c = anvil_lower_to_operand(ctx, inst->operands[2]);
            int size = get_type_size(inst->result->type);
            AnvilMOperand dst = anvil_mop_vreg_fp(dst_vreg, size);
            
            AnvilMInst* op = anvil_minst_create_with_capacity(ctx->arena, mir_kind, 4);
            op->operands[0] = dst;
            op->operands[1] = a;
            op->operands[2] = b;
            op->operands[3] = c;
            op->num_operands = 4;
            for (int i = 0; i < 4; i++) op->operands[i].is_fp = true;
            emit(ctx, op);
            break;
        }
        
        case ANVIL_INST_FCMP: {
            AnvilMOperand lhs = anvil_lower_to_operand(ctx, inst->operands[0]);
            AnvilMOperand rhs = anvil_lower_to_operand(ctx, inst->operands[1]);
            
            AnvilMInst* cmp = anvil_minst_create(ctx->arena, ANVIL_MIR_FCMP);
            cmp->operands[0] = lhs;
            cmp->operands[1] = rhs;
            cmp->num_operands = 2;
            cmp->operands[0].is_fp = true;
            cmp->operands[1].is_fp = true;
            emit(ctx, cmp);
            
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand dst = anvil_mop_vreg(dst_vreg, 1);
            
            AnvilMInst* setcc = anvil_minst_create(ctx->arena, ANVIL_MIR_SETCC);
            setcc->cc = ANVIL_CC_EQ;
            setcc->operands[0] = dst;
            setcc->num_operands = 1;
            emit(ctx, setcc);
            break;
        }
        
        case ANVIL_INST_VADD:
        case ANVIL_INST_VSUB:
        case ANVIL_INST_VMUL:
        case ANVIL_INST_VDIV: {
            switch (inst->kind) {
                case ANVIL_INST_VADD: mir_kind = ANVIL_MIR_ADDPD; break;
                case ANVIL_INST_VSUB: mir_kind = ANVIL_MIR_SUBPD; break;
                case ANVIL_INST_VMUL: mir_kind = ANVIL_MIR_MULPD; break;
                case ANVIL_INST_VDIV: mir_kind = ANVIL_MIR_DIVPD; break;
                default: mir_kind = ANVIL_MIR_ADDPD; break;
            }
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand lhs = anvil_lower_to_operand(ctx, inst->operands[0]);
            AnvilMOperand rhs = anvil_lower_to_operand(ctx, inst->operands[1]);
            int size = get_type_size(inst->result->type);
            AnvilMOperand dst = anvil_mop_vreg_fp(dst_vreg, size);
            
            AnvilMInst* mov = anvil_minst_mov(ctx->arena, dst, lhs);
            mov->operands[0].is_fp = true;
            mov->operands[1].is_fp = true;
            emit(ctx, mov);
            
            AnvilMInst* op = anvil_minst_binary(ctx->arena, mir_kind, dst, rhs);
            op->operands[0].is_fp = true;
            op->operands[1].is_fp = true;
            emit(ctx, op);
            break;
        }
        
        case ANVIL_INST_VLOAD: {
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            int size = get_type_size(inst->result->type);
            AnvilMOperand dst = anvil_mop_vreg_fp(dst_vreg, size);
            AnvilMOperand src = anvil_lower_to_operand(ctx, inst->operands[0]);
            
            AnvilMInst* load = anvil_minst_create(ctx->arena, ANVIL_MIR_MOVUPS);
            load->operands[0] = dst;
            load->operands[1] = src;
            load->num_operands = 2;
            load->operands[0].is_fp = true;
            emit(ctx, load);
            break;
        }
        
        case ANVIL_INST_VSTORE: {
            AnvilMOperand dst = anvil_lower_to_operand(ctx, inst->operands[0]);
            AnvilMOperand src = anvil_lower_to_operand(ctx, inst->operands[1]);
            
            AnvilMInst* store = anvil_minst_create(ctx->arena, ANVIL_MIR_MOVUPS);
            store->operands[0] = dst;
            store->operands[1] = src;
            store->num_operands = 2;
            store->operands[1].is_fp = true;
            emit(ctx, store);
            break;
        }
        
        case ANVIL_INST_NEG:
        case ANVIL_INST_NOT: {
            mir_kind = inst->kind == ANVIL_INST_NEG ? ANVIL_MIR_NEG : ANVIL_MIR_NOT;
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand src = anvil_lower_to_operand(ctx, inst->operands[0]);
            int size = get_type_size(inst->result->type);
            AnvilMOperand dst = anvil_mop_vreg(dst_vreg, size);
            
            AnvilMInst* mov = anvil_minst_mov(ctx->arena, dst, src);
            emit(ctx, mov);
            
            AnvilMInst* op = anvil_minst_unary(ctx->arena, mir_kind, dst);
            emit(ctx, op);
            break;
        }
        
        case ANVIL_INST_EQ:
        case ANVIL_INST_NE:
        case ANVIL_INST_LT:
        case ANVIL_INST_LE:
        case ANVIL_INST_GT:
        case ANVIL_INST_GE: {
            AnvilMOperand lhs = anvil_lower_to_operand(ctx, inst->operands[0]);
            AnvilMOperand rhs = anvil_lower_to_operand(ctx, inst->operands[1]);
            
            int tmp_vreg = ctx->next_vreg++;
            int size = get_type_size(inst->operands[0]->type);
            AnvilMOperand tmp = anvil_mop_vreg(tmp_vreg, size);
            
            if (lhs.kind == ANVIL_MOP_IMM) {
                AnvilMInst* mov = anvil_minst_mov_imm(ctx->arena, tmp, lhs.imm.value, size);
                emit(ctx, mov);
                lhs = tmp;
            }
            
            AnvilMInst* cmp = anvil_minst_cmp(ctx->arena, lhs, rhs);
            emit(ctx, cmp);
            
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand dst = anvil_mop_vreg(dst_vreg, 1);
            
            bool is_signed = anvil_type_is_signed(inst->operands[0]->type);
            AnvilCondCode cc = inst_to_cc(inst->kind, is_signed);
            
            AnvilMInst* setcc = anvil_minst_create(ctx->arena, ANVIL_MIR_SETCC);
            setcc->cc = cc;
            setcc->operands[0] = dst;
            setcc->num_operands = 1;
            setcc->defs[0] = dst;
            setcc->num_defs = 1;
            emit(ctx, setcc);
            break;
        }
        
        case ANVIL_INST_LOAD: {
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            int size = get_type_size(inst->result->type);
            bool load_is_fp = is_fp_type(inst->result->type);
            AnvilMOperand dst = load_is_fp ? anvil_mop_vreg_fp(dst_vreg, size) : anvil_mop_vreg(dst_vreg, size);
            
            AnvilValue* src_val = inst->operands[0];
            if (src_val && (src_val->kind == ANVIL_VALUE_PARAM || 
                           (src_val->kind == ANVIL_VALUE_VAR && src_val->var.is_param))) {
                int src_vreg = anvil_lower_value(ctx, src_val);
                AnvilMOperand src = load_is_fp ? anvil_mop_vreg_fp(src_vreg, size) : anvil_mop_vreg(src_vreg, size);
                AnvilMInst* mov = anvil_minst_mov(ctx->arena, dst, src);
                mov->operands[0].is_fp = load_is_fp;
                mov->operands[1].is_fp = load_is_fp;
                emit(ctx, mov);
            } else {
                int src_vreg = anvil_lower_value(ctx, src_val);
                AnvilMOperand src = anvil_mop_mem(src_vreg, 0, size);
                src.mem.base_is_vreg = true;
                
                AnvilMInst* load = anvil_minst_create(ctx->arena, ANVIL_MIR_LOAD);
                load->operands[0] = dst;
                load->operands[1] = src;
                load->num_operands = 2;
                load->defs[0] = dst;
                load->num_defs = 1;
                emit(ctx, load);
            }
            break;
        }
        
        case ANVIL_INST_STORE: {
            int ptr_vreg = anvil_lower_value(ctx, inst->operands[0]);
            AnvilMOperand val = anvil_lower_to_operand(ctx, inst->operands[1]);
            int size = get_type_size(inst->operands[1]->type);
            
            AnvilMOperand dst = anvil_mop_mem(ptr_vreg, 0, size);
            dst.mem.base_is_vreg = true;
            
            AnvilMInst* store = anvil_minst_create(ctx->arena, ANVIL_MIR_STORE);
            store->operands[0] = dst;
            store->operands[1] = val;
            store->num_operands = 2;
            emit(ctx, store);
            break;
        }
        
        case ANVIL_INST_BR: {
            char label[64];
            snprintf(label, sizeof(label), ".L%s_%d", 
                     ctx->current_func->name, inst->br.target->id);
            AnvilMInst* jmp = anvil_minst_jmp(ctx->arena, 
                anvil_arena_strdup(ctx->arena, label));
            emit(ctx, jmp);
            break;
        }
        
        case ANVIL_INST_BR_COND: {
            AnvilMOperand cond = anvil_lower_to_operand(ctx, inst->operands[0]);
            
            int tmp_vreg = ctx->next_vreg++;
            AnvilMOperand tmp = anvil_mop_vreg(tmp_vreg, 1);
            if (cond.kind == ANVIL_MOP_IMM) {
                AnvilMInst* mov = anvil_minst_mov_imm(ctx->arena, tmp, cond.imm.value, 1);
                emit(ctx, mov);
                cond = tmp;
            }
            
            AnvilMInst* test = anvil_minst_create(ctx->arena, ANVIL_MIR_TEST);
            test->operands[0] = cond;
            test->operands[1] = cond;
            test->num_operands = 2;
            emit(ctx, test);
            
            char then_label[64], else_label[64];
            snprintf(then_label, sizeof(then_label), ".L%s_%d",
                     ctx->current_func->name, inst->br_cond.then_block->id);
            snprintf(else_label, sizeof(else_label), ".L%s_%d",
                     ctx->current_func->name, inst->br_cond.else_block->id);
            
            AnvilMInst* jne = anvil_minst_jcc(ctx->arena, ANVIL_CC_NE,
                anvil_arena_strdup(ctx->arena, then_label));
            emit(ctx, jne);
            
            AnvilMInst* jmp = anvil_minst_jmp(ctx->arena,
                anvil_arena_strdup(ctx->arena, else_label));
            emit(ctx, jmp);
            break;
        }
        
        case ANVIL_INST_RET: {
            if (inst->operands[0]) {
                AnvilMOperand val = anvil_lower_to_operand(ctx, inst->operands[0]);
                int size = get_type_size(inst->operands[0]->type);
                bool ret_is_fp = is_fp_type(inst->operands[0]->type);
                
                AnvilMOperand ret_reg;
                if (ret_is_fp && ctx->ret_reg_fp >= 0) {
                    ret_reg = anvil_mop_preg_fp(ctx->ret_reg_fp, size);
                } else {
                    ret_reg = anvil_mop_preg(ctx->ret_reg_int, size);
                }
                
                AnvilMInst* mov = anvil_minst_mov(ctx->arena, ret_reg, val);
                if (ret_is_fp) {
                    mov->operands[0].is_fp = true;
                    mov->operands[1].is_fp = true;
                }
                emit(ctx, mov);
            }
            emit(ctx, anvil_minst_ret(ctx->arena));
            break;
        }
        
        case ANVIL_INST_RET_VOID: {
            emit(ctx, anvil_minst_ret(ctx->arena));
            break;
        }
        
        case ANVIL_INST_CALL: {
            size_t num_args = anvil_vec_len(&inst->call.args);
            int capacity = (int)num_args + 2;
            if (capacity < 4) capacity = 4;
            
            AnvilMInst* call = anvil_minst_create_with_capacity(ctx->arena, ANVIL_MIR_CALL, capacity);
            call->operands[0] = anvil_mop_func(inst->call.func_name);
            call->num_operands = 1;
            call->is_variadic = inst->call.is_variadic;
            call->num_fixed_args = inst->call.num_fixed_args;
            
            for (size_t i = 0; i < num_args; i++) {
                AnvilValue** arg = (AnvilValue**)anvil_vec_get(&inst->call.args, i);
                int size = get_type_size((*arg)->type);
                
                if ((*arg)->kind == ANVIL_VALUE_CONST_STRING) {
                    char label_name[64];
                    snprintf(label_name, sizeof(label_name), ".Lstr%u", (*arg)->id);
                    anvil_mir_add_string(ctx->mir, (int)(*arg)->id, (*arg)->str);
                    AnvilMOperand label = anvil_mop_label(
                        anvil_arena_strdup(ctx->arena, label_name), (int)(*arg)->id);
                    label.size = 8;
                    call->operands[call->num_operands++] = label;
                } else if (anvil_value_is_const_int(*arg)) {
                    call->operands[call->num_operands++] = anvil_mop_imm((*arg)->i64, size);
                } else {
                    int vreg = anvil_lower_value(ctx, *arg);
                    call->operands[call->num_operands++] = anvil_mop_vreg(vreg, size);
                }
            }
            
            emit(ctx, call);
            
            if (inst->result) {
                int dst_vreg = anvil_lower_value(ctx, inst->result);
                int size = get_type_size(inst->result->type);
                AnvilMOperand dst = anvil_mop_vreg(dst_vreg, size);
                int ret_preg = ctx->ret_reg_int;
                AnvilMOperand ret_reg = anvil_mop_preg(ret_preg, size);
                
                AnvilMInst* mov = anvil_minst_mov(ctx->arena, dst, ret_reg);
                emit(ctx, mov);
            }
            break;
        }
        
        case ANVIL_INST_ZEXT:
        case ANVIL_INST_SEXT:
        case ANVIL_INST_TRUNC: {
            mir_kind = inst->kind == ANVIL_INST_ZEXT ? ANVIL_MIR_ZEXT :
                       inst->kind == ANVIL_INST_SEXT ? ANVIL_MIR_SEXT : ANVIL_MIR_TRUNC;
            
            int dst_vreg = anvil_lower_value(ctx, inst->result);
            AnvilMOperand src = anvil_lower_to_operand(ctx, inst->operands[0]);
            int dst_size = get_type_size(inst->result->type);
            AnvilMOperand dst = anvil_mop_vreg(dst_vreg, dst_size);
            
            AnvilMInst* ext = anvil_minst_create(ctx->arena, mir_kind);
            ext->operands[0] = dst;
            ext->operands[1] = src;
            ext->num_operands = 2;
            ext->defs[0] = dst;
            ext->num_defs = 1;
            emit(ctx, ext);
            break;
        }
        
        default:
            break;
    }
}

void anvil_lower_block(AnvilLowerCtx* ctx, AnvilBlock* block) {
    char label[64];
    snprintf(label, sizeof(label), ".L%s_%d", ctx->current_func->name, block->id);
    
    AnvilMBlock* mblock = anvil_mfunc_add_block(ctx->current_func, 
        anvil_arena_strdup(ctx->arena, label));
    ctx->current_block = mblock;
    
    char key[64];
    snprintf(key, sizeof(key), "b%d", block->id);
    anvil_hash_insert(&ctx->block_map, anvil_arena_strdup(ctx->arena, key), mblock);
    
    AnvilMInst* label_inst = anvil_minst_label(ctx->arena, mblock->name);
    emit(ctx, label_inst);
    
    for (AnvilInst* inst = block->first; inst; inst = inst->next) {
        anvil_lower_inst(ctx, inst);
    }
}

void anvil_lower_func(AnvilLowerCtx* ctx, AnvilFunc* func) {
    ctx->current_func = anvil_mir_add_func(ctx->mir, func->name, func->ret_type);
    ctx->next_vreg = 1;
    anvil_hash_clear(&ctx->value_map);
    anvil_hash_clear(&ctx->block_map);
    
    for (size_t i = 0; i < anvil_vec_len(&func->params); i++) {
        AnvilVar** var = (AnvilVar**)anvil_vec_get(&func->params, i);
        int size = get_type_size((*var)->type);
        bool param_is_fp = is_fp_type((*var)->type);
        
        int vreg_id = ctx->next_vreg++;
        
        char key[32];
        snprintf(key, sizeof(key), "v%u", (*var)->value->id);
        anvil_hash_insert(&ctx->value_map, anvil_arena_strdup(ctx->arena, key), 
                          (void*)(intptr_t)vreg_id);
        
        AnvilMOperand* param = (AnvilMOperand*)anvil_vec_push(&ctx->current_func->params);
        if (param_is_fp) {
            *param = anvil_mop_vreg_fp(vreg_id, size);
        } else {
            *param = anvil_mop_vreg(vreg_id, size);
        }
        ctx->current_func->num_params++;
    }
    
    ctx->current_block = ctx->current_func->entry;
    
    for (AnvilBlock* block = func->entry; block; block = block->next) {
        if (block == func->entry) {
            for (AnvilInst* inst = block->first; inst; inst = inst->next) {
                anvil_lower_inst(ctx, inst);
            }
        } else {
            anvil_lower_block(ctx, block);
        }
    }
    
    ctx->current_func->next_vreg_id = ctx->next_vreg;
}

AnvilMIR* anvil_lower_module(AnvilModule* mod) {
    return anvil_lower_module_with_abi(mod, NULL);
}

AnvilMIR* anvil_lower_module_with_abi(AnvilModule* mod, const struct AnvilABI* abi) {
    AnvilLowerCtx ctx;
    ctx_init(&ctx, mod);
    
    if (abi) {
        ctx.abi = abi;
        ctx.ret_reg_int = abi->ret_reg_int_lo;
        ctx.ret_reg_fp = abi->ret_reg_float;
    }
    
    for (AnvilFunc* func = mod->first_func; func; func = func->next) {
        anvil_lower_func(&ctx, func);
    }
    
    AnvilMIR* result = ctx.mir;
    ctx_free(&ctx);
    return result;
}
