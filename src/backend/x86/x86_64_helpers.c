/*
 * ANVIL - x86-64 Backend Helper Functions
 *
 * Register name tables and type size/alignment helpers.
 */

#include "x86_64_internal.h"

bool x64_type_is_float(anvil_type_t *type)
{
    if (!type)
        return false;
    return type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64;
}
