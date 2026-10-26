#include "abi_layout.h"

static bool leaf_is_scalar(const anvil_type_t *type)
{
    switch (type->kind) {
        case ANVIL_TYPE_I1:
        case ANVIL_TYPE_I8: case ANVIL_TYPE_I16: case ANVIL_TYPE_I32: case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U8: case ANVIL_TYPE_U16: case ANVIL_TYPE_U32: case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F32: case ANVIL_TYPE_F64:
        case ANVIL_TYPE_PTR:
            return true;
        default:
            return false;
    }
}

bool anvil_abi_visit_leaves(anvil_type_t *type, size_t base,
                            anvil_abi_leaf_fn visit, void *user)
{
    if (!type) return false;
    if (leaf_is_scalar(type)) return visit(user, type, base);
    if (type->kind == ANVIL_TYPE_ARRAY) {
        anvil_type_t *element = type->data.array.elem;
        for (size_t i = 0; i < type->data.array.count; i++)
            if (!anvil_abi_visit_leaves(element, base + i * element->size, visit, user))
                return false;
        return true;
    }
    if (type->kind == ANVIL_TYPE_STRUCT && type->data.struc.complete) {
        for (size_t i = 0; i < type->data.struc.num_fields; i++)
            if (!anvil_abi_visit_leaves(type->data.struc.fields[i],
                                        base + type->data.struc.offsets[i],
                                        visit, user))
                return false;
        return true;
    }
    return false;
}

typedef struct {
    anvil_type_t *base;
    bool homogeneous;
} homogeneous_state_t;

static bool homogeneous_leaf(void *user, anvil_type_t *leaf, size_t offset)
{
    homogeneous_state_t *state = user;
    if ((leaf->kind != ANVIL_TYPE_F32 && leaf->kind != ANVIL_TYPE_F64) ||
        (state->base && state->base->kind != leaf->kind) ||
        offset % leaf->size != 0) {
        state->homogeneous = false;
        return false;
    }
    state->base = leaf;
    return true;
}

anvil_type_t *anvil_abi_homogeneous_float(anvil_type_t *type, size_t *count)
{
    /* Union members overlap, so the element count is the number of base
     * elements the object holds, not the number of leaves visited. */
    homogeneous_state_t state = { NULL, true };
    if (!anvil_abi_visit_leaves(type, 0, homogeneous_leaf, &state) ||
        !state.homogeneous || !state.base ||
        type->size % state.base->size != 0)
        return NULL;
    if (count) *count = type->size / state.base->size;
    return state.base;
}

static bool aligned_leaf(void *user, anvil_type_t *leaf, size_t offset)
{
    bool *unaligned = user;
    if (leaf->align && offset % leaf->align != 0) {
        *unaligned = true;
        return false;
    }
    return true;
}

bool anvil_abi_has_unaligned_leaf(anvil_type_t *type)
{
    bool unaligned = false;
    (void)anvil_abi_visit_leaves(type, 0, aligned_leaf, &unaligned);
    return unaligned;
}
