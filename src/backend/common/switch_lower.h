/* Switch lowering shared by the MachineIR lowerers: the cases sorted, runs
 * of consecutive values with one destination merged into one unsigned
 * range test, and the rest decided by a balanced tree of compares instead
 * of one compare per case.  The target supplies its blocks, its PHI-aware
 * edges and its boolean values through callbacks. */
#ifndef ANVIL_SWITCH_LOWER_H
#define ANVIL_SWITCH_LOWER_H

#include "anvil/anvil_machine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    anvil_mir_func_t *mir;
    void *user;
    anvil_mir_vreg_t selector;      /* one register of `bits` bits */
    unsigned bits;
    bool is_signed;                 /* the order of the tree's compares */
    anvil_mir_block_t default_target;
    /* A fresh block for one more test (the current block is kept). */
    anvil_mir_block_t (*new_block)(void *user);
    /* The block a branch to case `index`'s destination takes (an edge
     * block holding its PHI moves, as the target lowers them). */
    anvil_mir_block_t (*case_target)(void *user, size_t index);
    /* A value for a compare's result, in the target's boolean form. */
    anvil_mir_vreg_t (*new_bool)(void *user);
} anvil_switch_lowering_t;

enum { ANVIL_SWITCH_TREE_MIN_CASES = 4 };

/* Lowers the cases (values[i] -> destination identity targets[i]: equal
 * identities share one destination) from the current block.  False on
 * allocation failure.  Callers use it from ANVIL_SWITCH_TREE_MIN_CASES
 * cases on; fewer stay a chain of compares. */
bool anvil_mir_lower_switch_tree(const anvil_switch_lowering_t *lowering, const int64_t *values, const size_t *targets, size_t count);

#endif
