/*
 * ANVIL - Loop Unrolling Pass
 * 
 * Unrolls small loops with known trip counts to reduce branch overhead
 * and enable further optimizations.
 * 
 * Supported loop patterns:
 * - Simple counted loops with constant bounds
 * - Loops with single back-edge
 * - Loops without complex control flow (no nested loops, no early exits)
 * 
 * Unrolling strategies:
 * - Full unroll: for very small trip counts (<=8)
 * - Partial unroll: for larger loops (factor of 2 or 4)
 */

#include "anvil/anvil_internal.h"
#include "anvil/anvil_opt.h"
#include <stdlib.h>
#include <string.h>

/* Configuration */
#define MAX_FULL_UNROLL_TRIP_COUNT 8
#define MAX_PARTIAL_UNROLL_FACTOR 4
#define MAX_LOOP_BODY_INSTRS 32

/* Loop information structure */
typedef struct {
    anvil_block_t *header;       /* Loop header (condition check) */
    anvil_block_t *body;         /* Loop body */
    anvil_block_t *latch;        /* Back-edge block (jumps to header) */
    anvil_block_t *exit;         /* Exit block */
    anvil_block_t *preheader;    /* Block before loop */
    
    anvil_value_t *iv;           /* Induction variable (PHI node) */
    anvil_value_t *iv_init;      /* Initial value */
    anvil_value_t *iv_step;      /* Step value (increment) */
    anvil_value_t *iv_limit;     /* Loop bound */
    
    anvil_op_t cmp_op;           /* Comparison operation */
    int64_t trip_count;          /* Number of iterations (-1 if unknown) */
    size_t body_instr_count;     /* Number of instructions in body */
    
    bool is_simple;              /* True if loop is simple enough to unroll */
} loop_info_t;

/* Check if a value is a constant integer */
static bool is_const_int(anvil_value_t *val)
{
    return val && val->kind == ANVIL_VAL_CONST_INT;
}

/* Get integer constant value */
static int64_t get_const_int(anvil_value_t *val)
{
    return val->data.i;
}

/* Check if block is a loop header (has PHI nodes and conditional branch) */
static bool is_loop_header(anvil_block_t *block)
{
    if (!block) return false;
    if (!block->first) return false;
    if (!block->last) return false;
    
    /* Must have at least one PHI node as first instruction */
    if (block->first->op != ANVIL_OP_PHI) return false;
    
    /* Must have a conditional branch as terminator */
    if (block->last->op != ANVIL_OP_BR_COND) return false;
    
    return true;
}

/* Find the preheader block (predecessor that's not the latch) */
static anvil_block_t *find_preheader(anvil_func_t *func, anvil_block_t *header, 
                                      anvil_block_t *latch)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        if (block == latch) continue;
        
        anvil_instr_t *term = block->last;
        if (!term) continue;
        
        if (term->op == ANVIL_OP_BR && term->true_block == header) {
            return block;
        }
        if (term->op == ANVIL_OP_BR_COND) {
            if (term->true_block == header || term->false_block == header) {
                return block;
            }
        }
    }
    return NULL;
}

/* Find the latch block (block that branches back to header) */
static anvil_block_t *find_latch(anvil_func_t *func, anvil_block_t *header)
{
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        if (block == header) continue;
        
        anvil_instr_t *term = block->last;
        if (!term) continue;
        
        if (term->op == ANVIL_OP_BR && term->true_block == header) {
            return block;
        }
    }
    return NULL;
}

/* Count instructions in a block (excluding PHI and terminator) */
static size_t count_body_instrs(anvil_block_t *block)
{
    size_t count = 0;
    for (anvil_instr_t *instr = block->first; instr; instr = instr->next) {
        if (instr->op == ANVIL_OP_PHI) continue;
        if (instr->op == ANVIL_OP_BR || instr->op == ANVIL_OP_BR_COND) continue;
        count++;
    }
    return count;
}

/* Analyze a potential loop and fill in loop_info */
static bool analyze_loop(anvil_func_t *func, anvil_block_t *header, loop_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->trip_count = -1;
    info->header = header;
    
    /* Find latch block (block that branches back to header) */
    info->latch = find_latch(func, header);
    if (!info->latch) return false;
    
    /* Find preheader (predecessor that's not the latch) */
    info->preheader = find_preheader(func, header, info->latch);
    if (!info->preheader) return false;
    
    /* Get terminator and find exit block */
    anvil_instr_t *term = header->last;
    if (!term || term->op != ANVIL_OP_BR_COND) return false;
    
    /* Determine which branch is the body and which is the exit.
     * The body is the branch that eventually leads back to header.
     * For simple loops: body == latch (single block loop body)
     * For multi-block: body leads to latch which leads to header
     */
    anvil_block_t *true_target = term->true_block;
    anvil_block_t *false_target = term->false_block;
    
    /* Check if true branch leads to loop body */
    bool true_is_body = (true_target == info->latch);
    if (!true_is_body && true_target && true_target != info->exit && true_target->last) {
        /* Check if true_target eventually goes to latch or header */
        anvil_instr_t *t = true_target->last;
        if (t && t->op == ANVIL_OP_BR && t->true_block &&
            (t->true_block == info->latch || t->true_block == header)) {
            true_is_body = true;
        }
    }
    
    if (true_is_body) {
        info->body = true_target;
        info->exit = false_target;
    } else {
        info->body = false_target;
        info->exit = true_target;
    }
    
    /* Verify body leads back to header (directly or via latch) */
    if (info->body != info->latch) {
        anvil_instr_t *body_term = info->body->last;
        if (!body_term || body_term->op != ANVIL_OP_BR) return false;
        if (body_term->true_block != info->latch && body_term->true_block != header) {
            return false;  /* Complex control flow */
        }
    }
    
    /* Find induction variable (first PHI in header) */
    anvil_instr_t *phi_instr = header->first;
    if (!phi_instr || phi_instr->op != ANVIL_OP_PHI) return false;
    
    info->iv = phi_instr->result;
    
    /* Get initial value and step from PHI incoming values */
    if (phi_instr->num_operands < 2 || phi_instr->num_phi_incoming < 2) return false;
    
    for (size_t i = 0; i < phi_instr->num_phi_incoming; i++) {
        if (phi_instr->phi_blocks[i] == info->preheader) {
            info->iv_init = phi_instr->operands[i];
        } else if (phi_instr->phi_blocks[i] == info->latch || 
                   phi_instr->phi_blocks[i] == info->body) {
            info->iv_step = phi_instr->operands[i];
        }
    }
    
    if (!info->iv_init || !info->iv_step) return false;
    
    /* Find the comparison instruction */
    anvil_instr_t *cmp_instr = NULL;
    if (term->num_operands > 0 && term->operands[0]) {
        anvil_value_t *cond = term->operands[0];
        if (cond->kind == ANVIL_VAL_INSTR) {
            cmp_instr = cond->data.instr;
        }
    }
    
    if (!cmp_instr) return false;
    
    info->cmp_op = cmp_instr->op;
    
    /* Get loop bound from comparison */
    if (cmp_instr->num_operands >= 2) {
        anvil_value_t *cmp_lhs = cmp_instr->operands[0];
        anvil_value_t *cmp_rhs = cmp_instr->operands[1];
        
        /* Determine which operand is the IV and which is the bound */
        if (cmp_lhs == info->iv) {
            info->iv_limit = cmp_rhs;
        } else if (cmp_rhs == info->iv) {
            info->iv_limit = cmp_lhs;
        } else {
            return false;
        }
    }
    
    /* Count body instructions */
    info->body_instr_count = count_body_instrs(info->body);
    if (info->body != info->latch) {
        info->body_instr_count += count_body_instrs(info->latch);
    }
    
    /* Calculate trip count if possible */
    if (is_const_int(info->iv_init) && is_const_int(info->iv_limit)) {
        int64_t init = get_const_int(info->iv_init);
        int64_t limit = get_const_int(info->iv_limit);
        
        /* Try to determine step value */
        int64_t step = 1;  /* Default assumption */
        
        /* Check if iv_step is result of add instruction */
        if (info->iv_step && info->iv_step->kind == ANVIL_VAL_INSTR) {
            anvil_instr_t *step_instr = info->iv_step->data.instr;
            if (step_instr->op == ANVIL_OP_ADD && step_instr->num_operands >= 2) {
                anvil_value_t *step_rhs = step_instr->operands[1];
                if (is_const_int(step_rhs)) {
                    step = get_const_int(step_rhs);
                }
            }
        }
        
        if (step > 0) {
            switch (info->cmp_op) {
                case ANVIL_OP_CMP_LT:
                case ANVIL_OP_CMP_ULT:
                    info->trip_count = (limit - init + step - 1) / step;
                    break;
                case ANVIL_OP_CMP_LE:
                case ANVIL_OP_CMP_ULE:
                    info->trip_count = (limit - init) / step + 1;
                    break;
                case ANVIL_OP_CMP_NE:
                    info->trip_count = (limit - init) / step;
                    break;
                default:
                    break;
            }
        }
    }
    
    /* Determine if loop is simple enough to unroll */
    info->is_simple = (info->body_instr_count <= MAX_LOOP_BODY_INSTRS);
    
    return true;
}

/* Value mapping for cloning */
typedef struct {
    anvil_value_t *orig;
    anvil_value_t *clone;
} value_map_entry_t;

typedef struct {
    value_map_entry_t *entries;
    size_t count;
    size_t cap;
} value_map_t;

static void value_map_init(value_map_t *map)
{
    map->entries = NULL;
    map->count = 0;
    map->cap = 0;
}

static void value_map_destroy(value_map_t *map)
{
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
    map->cap = 0;
}

static void value_map_add(value_map_t *map, anvil_value_t *orig, anvil_value_t *clone)
{
    if (map->count >= map->cap) {
        size_t new_cap = map->cap ? map->cap * 2 : 16;
        value_map_entry_t *new_entries = realloc(map->entries, 
                                                  new_cap * sizeof(value_map_entry_t));
        if (!new_entries) return;
        map->entries = new_entries;
        map->cap = new_cap;
    }
    map->entries[map->count].orig = orig;
    map->entries[map->count].clone = clone;
    map->count++;
}

static anvil_value_t *value_map_get(value_map_t *map, anvil_value_t *orig)
{
    for (size_t i = 0; i < map->count; i++) {
        if (map->entries[i].orig == orig) {
            return map->entries[i].clone;
        }
    }
    return orig;  /* Return original if not mapped */
}

/* Insert instruction at end of block (before terminator) */
static void insert_instr_before_term(anvil_block_t *block, anvil_instr_t *instr)
{
    instr->parent = block;
    
    if (!block->first) {
        block->first = block->last = instr;
        instr->prev = instr->next = NULL;
        return;
    }
    
    /* Insert before terminator */
    anvil_instr_t *term = block->last;
    if (term->op == ANVIL_OP_BR || term->op == ANVIL_OP_BR_COND || 
        term->op == ANVIL_OP_RET) {
        instr->next = term;
        instr->prev = term->prev;
        if (term->prev) {
            term->prev->next = instr;
        } else {
            block->first = instr;
        }
        term->prev = instr;
    } else {
        /* No terminator, append at end */
        instr->prev = block->last;
        instr->next = NULL;
        block->last->next = instr;
        block->last = instr;
    }
}

/* Clone an instruction with value remapping */
static anvil_instr_t *clone_instr_with_map(anvil_ctx_t *ctx, anvil_instr_t *orig, 
                                            value_map_t *map)
{
    if (!orig) return NULL;
    if (orig->op == ANVIL_OP_PHI) return NULL;
    if (orig->op == ANVIL_OP_BR || orig->op == ANVIL_OP_BR_COND) return NULL;
    if (orig->op == ANVIL_OP_RET) return NULL;
    
    anvil_type_t *type = orig->result ? orig->result->type : ctx->type_void;
    anvil_instr_t *clone = anvil_instr_create(ctx, orig->op, type, NULL);
    if (!clone) return NULL;
    
    /* Copy operands with remapping */
    for (size_t i = 0; i < orig->num_operands; i++) {
        anvil_value_t *op = orig->operands[i];
        anvil_value_t *mapped = value_map_get(map, op);
        anvil_instr_add_operand(clone, mapped);
    }
    
    /* Map the result */
    if (orig->result && clone->result) {
        value_map_add(map, orig->result, clone->result);
    }
    
    return clone;
}

/* Create a constant for the IV at a specific iteration */
static anvil_value_t *make_iv_const(anvil_ctx_t *ctx, anvil_type_t *type, 
                                     int64_t init, int64_t step, int iter)
{
    int64_t val = init + step * iter;
    
    switch (type->kind) {
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return anvil_const_i8(ctx, (int8_t)val);
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return anvil_const_i16(ctx, (int16_t)val);
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
            return anvil_const_i32(ctx, (int32_t)val);
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
            return anvil_const_i64(ctx, val);
        default:
            return anvil_const_i32(ctx, (int32_t)val);
    }
}

/* Get step value from IV update instruction */
static int64_t get_step_value(loop_info_t *info)
{
    if (!info->iv_step) return 1;
    
    if (info->iv_step->kind == ANVIL_VAL_INSTR) {
        anvil_instr_t *step_instr = info->iv_step->data.instr;
        if (step_instr->op == ANVIL_OP_ADD && step_instr->num_operands >= 2) {
            anvil_value_t *step_rhs = step_instr->operands[1];
            if (is_const_int(step_rhs)) {
                return get_const_int(step_rhs);
            }
        }
    }
    return 1;
}

/* Perform full loop unrolling */
static bool unroll_loop_full(anvil_func_t *func, loop_info_t *info)
{
    if (info->trip_count <= 0 || info->trip_count > MAX_FULL_UNROLL_TRIP_COUNT) {
        return false;
    }
    
    anvil_ctx_t *ctx = func->parent->ctx;
    
    /* Get IV info */
    int64_t init_val = is_const_int(info->iv_init) ? get_const_int(info->iv_init) : 0;
    int64_t step_val = get_step_value(info);
    anvil_type_t *iv_type = info->iv->type;
    
    /* For full unrolling:
     * 1. Clone body instructions N times into preheader
     * 2. Replace IV with constant for each iteration
     * 3. Redirect preheader to exit block
     * 4. Mark loop blocks as dead (will be cleaned by DCE/SimplifyCFG)
     */
    
    value_map_t map;
    value_map_init(&map);
    
    /* Clone body for each iteration */
    for (int64_t iter = 0; iter < info->trip_count; iter++) {
        /* Create constant for IV at this iteration */
        anvil_value_t *iv_const = make_iv_const(ctx, iv_type, init_val, step_val, (int)iter);
        value_map_add(&map, info->iv, iv_const);
        
        /* Clone body instructions */
        for (anvil_instr_t *instr = info->body->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_PHI) continue;
            if (instr->op == ANVIL_OP_BR || instr->op == ANVIL_OP_BR_COND) continue;
            
            anvil_instr_t *clone = clone_instr_with_map(ctx, instr, &map);
            if (clone) {
                insert_instr_before_term(info->preheader, clone);
            }
        }
        
        /* If body != latch, also clone latch instructions */
        if (info->body != info->latch) {
            for (anvil_instr_t *instr = info->latch->first; instr; instr = instr->next) {
                if (instr->op == ANVIL_OP_PHI) continue;
                if (instr->op == ANVIL_OP_BR || instr->op == ANVIL_OP_BR_COND) continue;
                
                anvil_instr_t *clone = clone_instr_with_map(ctx, instr, &map);
                if (clone) {
                    insert_instr_before_term(info->preheader, clone);
                }
            }
        }
    }
    
    /* Redirect preheader to exit block */
    anvil_instr_t *preheader_term = info->preheader->last;
    if (preheader_term && preheader_term->op == ANVIL_OP_BR) {
        preheader_term->true_block = info->exit;
    }
    
    /* Mark header's PHI result with final value for uses after loop */
    anvil_value_t *final_iv = make_iv_const(ctx, iv_type, init_val, step_val, 
                                             (int)info->trip_count);
    value_map_add(&map, info->iv, final_iv);
    
    value_map_destroy(&map);
    
    return true;
}

/* Perform partial loop unrolling */
static bool unroll_loop_partial(anvil_func_t *func, loop_info_t *info, int factor)
{
    if (factor <= 1 || factor > MAX_PARTIAL_UNROLL_FACTOR) {
        return false;
    }
    
    anvil_ctx_t *ctx = func->parent->ctx;
    
    /* Partial unrolling:
     * 1. Duplicate body instructions (factor-1) times within the loop
     * 2. Multiply the step by factor
     * 3. Add epilogue for remaining iterations (if trip count not divisible)
     * 
     * For simplicity, we only do partial unroll when trip count is unknown
     * or divisible by factor.
     */
    
    /* Check if we can safely unroll */
    if (info->trip_count > 0 && (info->trip_count % factor) != 0) {
        /* Would need epilogue - skip for now */
        return false;
    }
    
    int64_t step_val = get_step_value(info);
    
    value_map_t map;
    value_map_init(&map);
    
    /* Start with IV mapped to itself */
    value_map_add(&map, info->iv, info->iv);
    
    /* Clone body (factor-1) times after original body */
    anvil_instr_t *insert_point = info->latch->last;  /* Before branch back */
    
    for (int i = 1; i < factor; i++) {
        /* Update IV mapping: add step to previous IV */
        anvil_value_t *prev_iv = value_map_get(&map, info->iv);
        
        /* Create add instruction for new IV value */
        anvil_value_t *step_const = make_iv_const(ctx, info->iv->type, step_val, 1, 0);
        anvil_instr_t *new_iv_instr = anvil_instr_create(ctx, ANVIL_OP_ADD, 
                                                          info->iv->type, NULL);
        if (new_iv_instr) {
            anvil_instr_add_operand(new_iv_instr, prev_iv);
            anvil_instr_add_operand(new_iv_instr, step_const);
            
            /* Insert before terminator */
            new_iv_instr->parent = info->latch;
            new_iv_instr->next = insert_point;
            new_iv_instr->prev = insert_point->prev;
            if (insert_point->prev) {
                insert_point->prev->next = new_iv_instr;
            } else {
                info->latch->first = new_iv_instr;
            }
            insert_point->prev = new_iv_instr;
            
            /* Update map */
            value_map_add(&map, info->iv, new_iv_instr->result);
        }
        
        /* Clone body instructions */
        for (anvil_instr_t *instr = info->body->first; instr; instr = instr->next) {
            if (instr->op == ANVIL_OP_PHI) continue;
            if (instr->op == ANVIL_OP_BR || instr->op == ANVIL_OP_BR_COND) continue;
            
            /* Skip IV increment - we handle it specially */
            if (instr->result == info->iv_step) continue;
            
            anvil_instr_t *clone = clone_instr_with_map(ctx, instr, &map);
            if (clone) {
                clone->parent = info->latch;
                clone->next = insert_point;
                clone->prev = insert_point->prev;
                if (insert_point->prev) {
                    insert_point->prev->next = clone;
                } else {
                    info->latch->first = clone;
                }
                insert_point->prev = clone;
            }
        }
    }
    
    /* Update the original IV increment to use factor * step */
    if (info->iv_step && info->iv_step->kind == ANVIL_VAL_INSTR) {
        anvil_instr_t *step_instr = info->iv_step->data.instr;
        if (step_instr->op == ANVIL_OP_ADD && step_instr->num_operands >= 2) {
            /* Replace step constant with factor * step */
            anvil_value_t *new_step = make_iv_const(ctx, info->iv->type, 
                                                     step_val * factor, 1, 0);
            step_instr->operands[1] = new_step;
        }
    }
    
    value_map_destroy(&map);
    
    return true;
}

/* Main loop unrolling pass */
bool anvil_pass_loop_unroll(anvil_func_t *func)
{
    if (!func || !func->blocks) return false;
    
    bool changed = false;
    
    /* Find and analyze loops */
    for (anvil_block_t *block = func->blocks; block; block = block->next) {
        /* Safety checks */
        if (!block->first || !block->last) continue;
        if (!is_loop_header(block)) continue;
        
        loop_info_t info;
        if (!analyze_loop(func, block, &info)) continue;
        
        if (!info.is_simple) continue;
        
        /* For now, only attempt full unrolling for very simple cases */
        if (info.trip_count > 0 && info.trip_count <= MAX_FULL_UNROLL_TRIP_COUNT) {
            /* Full unroll for small trip counts */
            if (unroll_loop_full(func, &info)) {
                changed = true;
            }
        }
        /* Partial unrolling disabled for now - needs more testing */
    }
    
    return changed;
}
