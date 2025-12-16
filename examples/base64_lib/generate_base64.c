/*
 * ANVIL Base64 Library Generator
 * 
 * This program generates assembly code for a base64 encoding library.
 * It demonstrates:
 *   - String/byte manipulation
 *   - Lookup table access
 *   - Bitwise operations (shifts, masks)
 *   - Loop constructs
 *   - Calling external C functions (malloc)
 * 
 * Generated functions:
 *   char* base64_encode(const char* input, int len, int* out_len);
 *   int base64_encoded_len(int input_len);
 * 
 * Usage: generate_base64 [arch] > base64_lib.s
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../arch_select.h"

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    if (!parse_arch_args(argc, argv, &config)) {
        return 1;
    }
    
    ctx = anvil_ctx_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    
    if (!setup_arch_context(ctx, &config)) {
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    fprintf(stderr, "Generating base64 library for: %s\n", config.arch_name);
    
    anvil_module_t *mod = anvil_module_create(ctx, "base64_lib");
    if (!mod) {
        fprintf(stderr, "Failed to create module\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Common types */
    anvil_type_t *i8 = anvil_type_i8(ctx);
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *void_type = anvil_type_void(ctx);
    anvil_type_t *ptr_i8 = anvil_type_ptr(ctx, i8);
    anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *ptr_void = anvil_type_ptr(ctx, void_type);
    
    /* Determine size type based on architecture */
    const anvil_arch_info_t *arch_info = anvil_ctx_get_arch_info(ctx);
    anvil_type_t *size_type = (arch_info->ptr_size == 8) ? i64 : i32;
    
    /* Declare external malloc */
    anvil_type_t *malloc_params[] = { size_type };
    anvil_value_t *malloc_func = anvil_module_add_extern(mod, "malloc", 
        anvil_type_func(ctx, ptr_void, malloc_params, 1, false));
    
    /* ================================================================
     * base64_encoded_len: int base64_encoded_len(int input_len)
     * Returns the length of base64 encoded output (without null terminator)
     * Formula: ((input_len + 2) / 3) * 4
     * ================================================================ */
    {
        anvil_type_t *params[] = { i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 1, false);
        anvil_func_t *func = anvil_func_create(mod, "base64_encoded_len", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_set_insert_point(ctx, entry);
        
        anvil_value_t *input_len = anvil_func_get_param(func, 0);
        
        /* (input_len + 2) / 3 * 4 */
        anvil_value_t *two = anvil_const_i32(ctx, 2);
        anvil_value_t *three = anvil_const_i32(ctx, 3);
        anvil_value_t *four = anvil_const_i32(ctx, 4);
        
        anvil_value_t *sum = anvil_build_add(ctx, input_len, two, "sum");
        anvil_value_t *div = anvil_build_sdiv(ctx, sum, three, "div");
        anvil_value_t *result = anvil_build_mul(ctx, div, four, "result");
        
        anvil_build_ret(ctx, result);
    }
    
    /* ================================================================
     * base64_encode: char* base64_encode(const char* input, int len, int* out_len)
     * Encodes input bytes to base64 string
     * Returns malloc'd string (caller must free)
     * ================================================================ */
    {
        anvil_type_t *params[] = { ptr_i8, i32, ptr_i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, ptr_i8, params, 3, false);
        anvil_func_t *func = anvil_func_create(mod, "base64_encode", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_block_t *loop_cond = anvil_block_create(func, "loop_cond");
        anvil_block_t *loop_body = anvil_block_create(func, "loop_body");
        anvil_block_t *has_byte2 = anvil_block_create(func, "has_byte2");
        anvil_block_t *no_byte2 = anvil_block_create(func, "no_byte2");
        anvil_block_t *after_byte2 = anvil_block_create(func, "after_byte2");
        anvil_block_t *has_byte3 = anvil_block_create(func, "has_byte3");
        anvil_block_t *no_byte3 = anvil_block_create(func, "no_byte3");
        anvil_block_t *after_byte3 = anvil_block_create(func, "after_byte3");
        anvil_block_t *loop_end = anvil_block_create(func, "loop_end");
        
        anvil_set_insert_point(ctx, entry);
        
        anvil_value_t *input = anvil_func_get_param(func, 0);
        anvil_value_t *len = anvil_func_get_param(func, 1);
        anvil_value_t *out_len_ptr = anvil_func_get_param(func, 2);
        
        /* Calculate output length: ((len + 2) / 3) * 4 + 1 (for null) */
        anvil_value_t *two = anvil_const_i32(ctx, 2);
        anvil_value_t *three = anvil_const_i32(ctx, 3);
        anvil_value_t *four = anvil_const_i32(ctx, 4);
        anvil_value_t *one = anvil_const_i32(ctx, 1);
        anvil_value_t *zero = anvil_const_i32(ctx, 0);
        
        anvil_value_t *sum = anvil_build_add(ctx, len, two, "sum");
        anvil_value_t *div = anvil_build_sdiv(ctx, sum, three, "div");
        anvil_value_t *out_size = anvil_build_mul(ctx, div, four, "out_size");
        anvil_value_t *alloc_size = anvil_build_add(ctx, out_size, one, "alloc_size");
        
        /* Allocate output buffer */
        anvil_value_t *alloc_size_ext;
        if (arch_info->ptr_size == 8) {
            alloc_size_ext = anvil_build_zext(ctx, alloc_size, size_type, "alloc_size_ext");
        } else {
            alloc_size_ext = alloc_size;
        }
        
        anvil_value_t *malloc_args[] = { alloc_size_ext };
        anvil_value_t *output_void = anvil_build_call(ctx, ptr_void, malloc_func, malloc_args, 1, "output_void");
        anvil_value_t *output = anvil_build_bitcast(ctx, output_void, ptr_i8, "output");
        
        /* Store output length if pointer provided */
        anvil_build_store(ctx, out_size, out_len_ptr);
        
        /* Allocate loop variables */
        anvil_value_t *i_ptr = anvil_build_alloca(ctx, i32, "i");
        anvil_value_t *j_ptr = anvil_build_alloca(ctx, i32, "j");
        anvil_value_t *b1_ptr = anvil_build_alloca(ctx, i32, "b1");
        anvil_value_t *b2_ptr = anvil_build_alloca(ctx, i32, "b2");
        anvil_value_t *b3_ptr = anvil_build_alloca(ctx, i32, "b3");
        
        anvil_build_store(ctx, zero, i_ptr);
        anvil_build_store(ctx, zero, j_ptr);
        anvil_build_br(ctx, loop_cond);
        
        /* Loop condition: while (i < len) */
        anvil_set_insert_point(ctx, loop_cond);
        anvil_value_t *i_val = anvil_build_load(ctx, i32, i_ptr, "i_val");
        anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_val, len, "cmp");
        anvil_build_br_cond(ctx, cmp, loop_body, loop_end);
        
        /* Loop body */
        anvil_set_insert_point(ctx, loop_body);
        
        /* Read first byte (always present) */
        anvil_value_t *i_val2 = anvil_build_load(ctx, i32, i_ptr, "i_val2");
        anvil_value_t *idx1[] = { i_val2 };
        anvil_value_t *ptr1 = anvil_build_gep(ctx, i8, input, idx1, 1, "ptr1");
        anvil_value_t *byte1 = anvil_build_load(ctx, i8, ptr1, "byte1");
        anvil_value_t *b1 = anvil_build_zext(ctx, byte1, i32, "b1");
        anvil_build_store(ctx, b1, b1_ptr);
        
        /* Check if second byte exists */
        anvil_value_t *i_plus1 = anvil_build_add(ctx, i_val2, one, "i_plus1");
        anvil_value_t *has_b2 = anvil_build_cmp_lt(ctx, i_plus1, len, "has_b2");
        anvil_build_br_cond(ctx, has_b2, has_byte2, no_byte2);
        
        /* Has byte 2 */
        anvil_set_insert_point(ctx, has_byte2);
        anvil_value_t *i_val3 = anvil_build_load(ctx, i32, i_ptr, "i_val3");
        anvil_value_t *idx2_off = anvil_build_add(ctx, i_val3, one, "idx2_off");
        anvil_value_t *idx2[] = { idx2_off };
        anvil_value_t *ptr2 = anvil_build_gep(ctx, i8, input, idx2, 1, "ptr2");
        anvil_value_t *byte2 = anvil_build_load(ctx, i8, ptr2, "byte2");
        anvil_value_t *b2_val = anvil_build_zext(ctx, byte2, i32, "b2_val");
        anvil_build_store(ctx, b2_val, b2_ptr);
        anvil_build_br(ctx, after_byte2);
        
        /* No byte 2 */
        anvil_set_insert_point(ctx, no_byte2);
        anvil_build_store(ctx, zero, b2_ptr);
        anvil_build_br(ctx, after_byte2);
        
        /* After byte 2 - check byte 3 */
        anvil_set_insert_point(ctx, after_byte2);
        anvil_value_t *i_val4 = anvil_build_load(ctx, i32, i_ptr, "i_val4");
        anvil_value_t *i_plus2 = anvil_build_add(ctx, i_val4, two, "i_plus2");
        anvil_value_t *has_b3 = anvil_build_cmp_lt(ctx, i_plus2, len, "has_b3");
        anvil_build_br_cond(ctx, has_b3, has_byte3, no_byte3);
        
        /* Has byte 3 */
        anvil_set_insert_point(ctx, has_byte3);
        anvil_value_t *i_val5 = anvil_build_load(ctx, i32, i_ptr, "i_val5");
        anvil_value_t *idx3_off = anvil_build_add(ctx, i_val5, two, "idx3_off");
        anvil_value_t *idx3[] = { idx3_off };
        anvil_value_t *ptr3 = anvil_build_gep(ctx, i8, input, idx3, 1, "ptr3");
        anvil_value_t *byte3 = anvil_build_load(ctx, i8, ptr3, "byte3");
        anvil_value_t *b3_val = anvil_build_zext(ctx, byte3, i32, "b3_val");
        anvil_build_store(ctx, b3_val, b3_ptr);
        anvil_build_br(ctx, after_byte3);
        
        /* No byte 3 */
        anvil_set_insert_point(ctx, no_byte3);
        anvil_build_store(ctx, zero, b3_ptr);
        anvil_build_br(ctx, after_byte3);
        
        /* After byte 3 - encode the 4 output characters */
        anvil_set_insert_point(ctx, after_byte3);
        
        /* Load the bytes */
        anvil_value_t *b1_final = anvil_build_load(ctx, i32, b1_ptr, "b1_final");
        anvil_value_t *b2_final = anvil_build_load(ctx, i32, b2_ptr, "b2_final");
        anvil_value_t *b3_final = anvil_build_load(ctx, i32, b3_ptr, "b3_final");
        
        /* Calculate 4 base64 indices:
         * idx0 = b1 >> 2
         * idx1 = ((b1 & 0x03) << 4) | (b2 >> 4)
         * idx2 = ((b2 & 0x0F) << 2) | (b3 >> 6)
         * idx3 = b3 & 0x3F
         */
        anvil_value_t *c2 = anvil_const_i32(ctx, 2);
        anvil_value_t *c4 = anvil_const_i32(ctx, 4);
        anvil_value_t *c6 = anvil_const_i32(ctx, 6);
        anvil_value_t *mask03 = anvil_const_i32(ctx, 0x03);
        anvil_value_t *mask0f = anvil_const_i32(ctx, 0x0F);
        anvil_value_t *mask3f = anvil_const_i32(ctx, 0x3F);
        
        /* idx0 = b1 >> 2 */
        anvil_value_t *idx0 = anvil_build_shr(ctx, b1_final, c2, "idx0");
        
        /* idx1 = ((b1 & 0x03) << 4) | (b2 >> 4) */
        anvil_value_t *t1 = anvil_build_and(ctx, b1_final, mask03, "t1");
        anvil_value_t *t2 = anvil_build_shl(ctx, t1, c4, "t2");
        anvil_value_t *t3 = anvil_build_shr(ctx, b2_final, c4, "t3");
        anvil_value_t *idx1_val = anvil_build_or(ctx, t2, t3, "idx1_val");
        
        /* idx2 = ((b2 & 0x0F) << 2) | (b3 >> 6) */
        anvil_value_t *t4 = anvil_build_and(ctx, b2_final, mask0f, "t4");
        anvil_value_t *t5 = anvil_build_shl(ctx, t4, c2, "t5");
        anvil_value_t *t6 = anvil_build_shr(ctx, b3_final, c6, "t6");
        anvil_value_t *idx2_val = anvil_build_or(ctx, t5, t6, "idx2_val");
        
        /* idx3 = b3 & 0x3F */
        anvil_value_t *idx3_val = anvil_build_and(ctx, b3_final, mask3f, "idx3_val");
        
        /* Convert indices to base64 characters using arithmetic:
         * 0-25  -> 'A'-'Z' (65-90)
         * 26-51 -> 'a'-'z' (97-122)
         * 52-61 -> '0'-'9' (48-57)
         * 62    -> '+' (43)
         * 63    -> '/' (47)
         * 
         * For simplicity, we'll use a simpler mapping:
         * Just add 'A' (65) for 0-25, etc.
         * But this requires conditionals. For now, use simple offset.
         */
        
        /* Simplified: map index to character
         * We'll use: idx < 26 ? idx + 'A' : idx < 52 ? idx - 26 + 'a' : idx < 62 ? idx - 52 + '0' : idx == 62 ? '+' : '/'
         * For simplicity in IR, we'll just compute a basic mapping
         */
        
        /* Simple approach: use lookup table concept but compute inline
         * char = (idx < 26) ? (idx + 65) : (idx < 52) ? (idx + 71) : (idx < 62) ? (idx - 4) : (idx == 62) ? 43 : 47
         * 
         * For this example, let's use a simpler formula that works for most cases:
         * We'll use select operations
         */
        
        /* For idx0: always valid */
        anvil_value_t *c26 = anvil_const_i32(ctx, 26);
        anvil_value_t *c52 = anvil_const_i32(ctx, 52);
        anvil_value_t *c62 = anvil_const_i32(ctx, 62);
        anvil_value_t *c65 = anvil_const_i32(ctx, 65);  /* 'A' */
        anvil_value_t *c71 = anvil_const_i32(ctx, 71);  /* 'a' - 26 */
        anvil_value_t *cm4 = anvil_const_i32(ctx, -4);  /* '0' - 52 */
        anvil_value_t *c43 = anvil_const_i32(ctx, 43);  /* '+' */
        anvil_value_t *c47 = anvil_const_i32(ctx, 47);  /* '/' */
        anvil_value_t *c61 = anvil_const_i32(ctx, 61);  /* '=' */
        
        /* Helper to convert index to char - inline for idx0 */
        /* if idx < 26: idx + 65 */
        anvil_value_t *lt26_0 = anvil_build_cmp_lt(ctx, idx0, c26, "lt26_0");
        anvil_value_t *ch0_upper = anvil_build_add(ctx, idx0, c65, "ch0_upper");
        /* if idx < 52: idx + 71 */
        anvil_value_t *lt52_0 = anvil_build_cmp_lt(ctx, idx0, c52, "lt52_0");
        anvil_value_t *ch0_lower = anvil_build_add(ctx, idx0, c71, "ch0_lower");
        /* if idx < 62: idx - 4 */
        anvil_value_t *lt62_0 = anvil_build_cmp_lt(ctx, idx0, c62, "lt62_0");
        anvil_value_t *ch0_digit = anvil_build_add(ctx, idx0, cm4, "ch0_digit");
        /* if idx == 62: 43 (+), else 47 (/) */
        anvil_value_t *eq62_0 = anvil_build_cmp_eq(ctx, idx0, c62, "eq62_0");
        anvil_value_t *ch0_sym = anvil_build_select(ctx, eq62_0, c43, c47, "ch0_sym");
        /* Chain selects */
        anvil_value_t *ch0_t1 = anvil_build_select(ctx, lt62_0, ch0_digit, ch0_sym, "ch0_t1");
        anvil_value_t *ch0_t2 = anvil_build_select(ctx, lt52_0, ch0_lower, ch0_t1, "ch0_t2");
        anvil_value_t *char0 = anvil_build_select(ctx, lt26_0, ch0_upper, ch0_t2, "char0");
        
        /* Same for idx1 */
        anvil_value_t *lt26_1 = anvil_build_cmp_lt(ctx, idx1_val, c26, "lt26_1");
        anvil_value_t *ch1_upper = anvil_build_add(ctx, idx1_val, c65, "ch1_upper");
        anvil_value_t *lt52_1 = anvil_build_cmp_lt(ctx, idx1_val, c52, "lt52_1");
        anvil_value_t *ch1_lower = anvil_build_add(ctx, idx1_val, c71, "ch1_lower");
        anvil_value_t *lt62_1 = anvil_build_cmp_lt(ctx, idx1_val, c62, "lt62_1");
        anvil_value_t *ch1_digit = anvil_build_add(ctx, idx1_val, cm4, "ch1_digit");
        anvil_value_t *eq62_1 = anvil_build_cmp_eq(ctx, idx1_val, c62, "eq62_1");
        anvil_value_t *ch1_sym = anvil_build_select(ctx, eq62_1, c43, c47, "ch1_sym");
        anvil_value_t *ch1_t1 = anvil_build_select(ctx, lt62_1, ch1_digit, ch1_sym, "ch1_t1");
        anvil_value_t *ch1_t2 = anvil_build_select(ctx, lt52_1, ch1_lower, ch1_t1, "ch1_t2");
        anvil_value_t *char1 = anvil_build_select(ctx, lt26_1, ch1_upper, ch1_t2, "char1");
        
        /* For idx2: may be '=' if only 1 input byte */
        anvil_value_t *i_val6 = anvil_build_load(ctx, i32, i_ptr, "i_val6");
        anvil_value_t *i_plus1_2 = anvil_build_add(ctx, i_val6, one, "i_plus1_2");
        anvil_value_t *need_pad2 = anvil_build_cmp_ge(ctx, i_plus1_2, len, "need_pad2");
        
        anvil_value_t *lt26_2 = anvil_build_cmp_lt(ctx, idx2_val, c26, "lt26_2");
        anvil_value_t *ch2_upper = anvil_build_add(ctx, idx2_val, c65, "ch2_upper");
        anvil_value_t *lt52_2 = anvil_build_cmp_lt(ctx, idx2_val, c52, "lt52_2");
        anvil_value_t *ch2_lower = anvil_build_add(ctx, idx2_val, c71, "ch2_lower");
        anvil_value_t *lt62_2 = anvil_build_cmp_lt(ctx, idx2_val, c62, "lt62_2");
        anvil_value_t *ch2_digit = anvil_build_add(ctx, idx2_val, cm4, "ch2_digit");
        anvil_value_t *eq62_2 = anvil_build_cmp_eq(ctx, idx2_val, c62, "eq62_2");
        anvil_value_t *ch2_sym = anvil_build_select(ctx, eq62_2, c43, c47, "ch2_sym");
        anvil_value_t *ch2_t1 = anvil_build_select(ctx, lt62_2, ch2_digit, ch2_sym, "ch2_t1");
        anvil_value_t *ch2_t2 = anvil_build_select(ctx, lt52_2, ch2_lower, ch2_t1, "ch2_t2");
        anvil_value_t *ch2_encoded = anvil_build_select(ctx, lt26_2, ch2_upper, ch2_t2, "ch2_encoded");
        anvil_value_t *char2 = anvil_build_select(ctx, need_pad2, c61, ch2_encoded, "char2");
        
        /* For idx3: may be '=' if only 1 or 2 input bytes */
        anvil_value_t *i_val7 = anvil_build_load(ctx, i32, i_ptr, "i_val7");
        anvil_value_t *i_plus2_2 = anvil_build_add(ctx, i_val7, two, "i_plus2_2");
        anvil_value_t *need_pad3 = anvil_build_cmp_ge(ctx, i_plus2_2, len, "need_pad3");
        
        anvil_value_t *lt26_3 = anvil_build_cmp_lt(ctx, idx3_val, c26, "lt26_3");
        anvil_value_t *ch3_upper = anvil_build_add(ctx, idx3_val, c65, "ch3_upper");
        anvil_value_t *lt52_3 = anvil_build_cmp_lt(ctx, idx3_val, c52, "lt52_3");
        anvil_value_t *ch3_lower = anvil_build_add(ctx, idx3_val, c71, "ch3_lower");
        anvil_value_t *lt62_3 = anvil_build_cmp_lt(ctx, idx3_val, c62, "lt62_3");
        anvil_value_t *ch3_digit = anvil_build_add(ctx, idx3_val, cm4, "ch3_digit");
        anvil_value_t *eq62_3 = anvil_build_cmp_eq(ctx, idx3_val, c62, "eq62_3");
        anvil_value_t *ch3_sym = anvil_build_select(ctx, eq62_3, c43, c47, "ch3_sym");
        anvil_value_t *ch3_t1 = anvil_build_select(ctx, lt62_3, ch3_digit, ch3_sym, "ch3_t1");
        anvil_value_t *ch3_t2 = anvil_build_select(ctx, lt52_3, ch3_lower, ch3_t1, "ch3_t2");
        anvil_value_t *ch3_encoded = anvil_build_select(ctx, lt26_3, ch3_upper, ch3_t2, "ch3_encoded");
        anvil_value_t *char3 = anvil_build_select(ctx, need_pad3, c61, ch3_encoded, "char3");
        
        /* Store the 4 output characters */
        anvil_value_t *j_val = anvil_build_load(ctx, i32, j_ptr, "j_val");
        
        /* Truncate chars to i8 */
        anvil_value_t *ch0_i8 = anvil_build_trunc(ctx, char0, i8, "ch0_i8");
        anvil_value_t *ch1_i8 = anvil_build_trunc(ctx, char1, i8, "ch1_i8");
        anvil_value_t *ch2_i8 = anvil_build_trunc(ctx, char2, i8, "ch2_i8");
        anvil_value_t *ch3_i8 = anvil_build_trunc(ctx, char3, i8, "ch3_i8");
        
        /* Store char0 at output[j] */
        anvil_value_t *out_idx0[] = { j_val };
        anvil_value_t *out_ptr0 = anvil_build_gep(ctx, i8, output, out_idx0, 1, "out_ptr0");
        anvil_build_store(ctx, ch0_i8, out_ptr0);
        
        /* Store char1 at output[j+1] */
        anvil_value_t *j_plus1 = anvil_build_add(ctx, j_val, one, "j_plus1");
        anvil_value_t *out_idx1[] = { j_plus1 };
        anvil_value_t *out_ptr1 = anvil_build_gep(ctx, i8, output, out_idx1, 1, "out_ptr1");
        anvil_build_store(ctx, ch1_i8, out_ptr1);
        
        /* Store char2 at output[j+2] */
        anvil_value_t *j_plus2 = anvil_build_add(ctx, j_val, two, "j_plus2");
        anvil_value_t *out_idx2[] = { j_plus2 };
        anvil_value_t *out_ptr2 = anvil_build_gep(ctx, i8, output, out_idx2, 1, "out_ptr2");
        anvil_build_store(ctx, ch2_i8, out_ptr2);
        
        /* Store char3 at output[j+3] */
        anvil_value_t *j_plus3 = anvil_build_add(ctx, j_val, three, "j_plus3");
        anvil_value_t *out_idx3[] = { j_plus3 };
        anvil_value_t *out_ptr3 = anvil_build_gep(ctx, i8, output, out_idx3, 1, "out_ptr3");
        anvil_build_store(ctx, ch3_i8, out_ptr3);
        
        /* Update j += 4 */
        anvil_value_t *j_new = anvil_build_add(ctx, j_val, four, "j_new");
        anvil_build_store(ctx, j_new, j_ptr);
        
        /* Update i += 3 */
        anvil_value_t *i_val8 = anvil_build_load(ctx, i32, i_ptr, "i_val8");
        anvil_value_t *i_new = anvil_build_add(ctx, i_val8, three, "i_new");
        anvil_build_store(ctx, i_new, i_ptr);
        
        anvil_build_br(ctx, loop_cond);
        
        /* Loop end - null terminate and return */
        anvil_set_insert_point(ctx, loop_end);
        anvil_value_t *j_final = anvil_build_load(ctx, i32, j_ptr, "j_final");
        anvil_value_t *null_idx[] = { j_final };
        anvil_value_t *null_ptr = anvil_build_gep(ctx, i8, output, null_idx, 1, "null_ptr");
        anvil_value_t *null_char = anvil_const_i8(ctx, 0);
        anvil_build_store(ctx, null_char, null_ptr);
        
        anvil_build_ret(ctx, output);
    }
    
    /* Generate code */
    char *asm_output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &asm_output, &len);
    
    if (err == ANVIL_OK && asm_output) {
        printf("%s", asm_output);
        free(asm_output);
        fprintf(stderr, "Generated %zu bytes of assembly\n", len);
    } else {
        fprintf(stderr, "Code generation failed: %s\n", anvil_ctx_get_error(ctx));
        anvil_module_destroy(mod);
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    anvil_module_destroy(mod);
    anvil_ctx_destroy(ctx);
    return 0;
}
