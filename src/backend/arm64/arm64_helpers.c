/*
 * ANVIL - ARM64 Backend Helper Functions
 * 
 * Implementation of helper functions for the ARM64 code generator.
 */

#include "arm64_internal.h"

/* ============================================================================
 * Register Name Tables
 * ============================================================================ */

const char *arm64_xreg_names[33] = {
    "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
    "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
    "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
    "x24", "x25", "x26", "x27", "x28", "x29", "x30", "sp", "xzr"
};

const char *arm64_wreg_names[33] = {
    "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7",
    "w8", "w9", "w10", "w11", "w12", "w13", "w14", "w15",
    "w16", "w17", "w18", "w19", "w20", "w21", "w22", "w23",
    "w24", "w25", "w26", "w27", "w28", "w29", "w30", "wsp", "wzr"
};

const char *arm64_dreg_names[32] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15",
    "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
    "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31"
};

const char *arm64_sreg_names[32] = {
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
    "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
    "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31"
};

/* ============================================================================
 * Type Helpers
 * ============================================================================ */

int arm64_type_size(anvil_type_t *type)
{
    if (!type) return 8;
    
    switch (type->kind) {
        case ANVIL_TYPE_VOID:
            return 0;
        case ANVIL_TYPE_I1:
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return 1;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return 2;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_F32:
            return 4;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F64:
        case ANVIL_TYPE_PTR:
            return 8;
        case ANVIL_TYPE_ARRAY:
            return type->data.array.count * arm64_type_size(type->data.array.elem);
        case ANVIL_TYPE_STRUCT:
            {
                int size = 0;
                for (size_t i = 0; i < type->data.struc.num_fields; i++) {
                    int field_size = arm64_type_size(type->data.struc.fields[i]);
                    int field_align = arm64_type_align(type->data.struc.fields[i]);
                    /* Align field */
                    size = (size + field_align - 1) & ~(field_align - 1);
                    size += field_size;
                }
                /* Align struct to its alignment */
                int struct_align = arm64_type_align(type);
                size = (size + struct_align - 1) & ~(struct_align - 1);
                return size > 0 ? size : 8;
            }
        case ANVIL_TYPE_FUNC:
            return 8;  /* Function pointer */
        default:
            return 8;
    }
}

int arm64_type_align(anvil_type_t *type)
{
    if (!type) return 8;
    
    switch (type->kind) {
        case ANVIL_TYPE_VOID:
            return 1;
        case ANVIL_TYPE_I1:
        case ANVIL_TYPE_I8:
        case ANVIL_TYPE_U8:
            return 1;
        case ANVIL_TYPE_I16:
        case ANVIL_TYPE_U16:
            return 2;
        case ANVIL_TYPE_I32:
        case ANVIL_TYPE_U32:
        case ANVIL_TYPE_F32:
            return 4;
        case ANVIL_TYPE_I64:
        case ANVIL_TYPE_U64:
        case ANVIL_TYPE_F64:
        case ANVIL_TYPE_PTR:
            return 8;
        case ANVIL_TYPE_ARRAY:
            return arm64_type_align(type->data.array.elem);
        case ANVIL_TYPE_STRUCT:
            {
                int max_align = 1;
                for (size_t i = 0; i < type->data.struc.num_fields; i++) {
                    int field_align = arm64_type_align(type->data.struc.fields[i]);
                    if (field_align > max_align) max_align = field_align;
                }
                return max_align;
            }
        case ANVIL_TYPE_FUNC:
            return 8;
        default:
            return 8;
    }
}

bool arm64_type_is_float(anvil_type_t *type)
{
    if (!type) return false;
    return type->kind == ANVIL_TYPE_F32 || type->kind == ANVIL_TYPE_F64;
}
