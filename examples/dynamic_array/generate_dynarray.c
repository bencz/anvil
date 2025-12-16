/*
 * ANVIL Dynamic Array Library Generator
 * 
 * This program generates assembly code for a dynamic array library
 * that demonstrates calling C library functions like malloc, free,
 * memcpy, and qsort from ANVIL-generated code.
 * 
 * The generated functions are:
 *   int* array_create(int capacity);           // malloc wrapper
 *   void array_destroy(int* arr);              // free wrapper
 *   int* array_copy(int* src, int count);      // malloc + memcpy
 *   int array_sum(int* arr, int count);        // sum all elements
 *   int array_max(int* arr, int count);        // find maximum
 *   int array_min(int* arr, int count);        // find minimum
 *   int array_count_if(int* arr, int n, int threshold);  // count elements > threshold
 *   void array_scale(int* arr, int n, int factor);       // multiply all by factor
 * 
 * Usage: generate_dynarray [arch] > dynarray_lib.s
 * 
 * Then compile and link with test_dynarray.c
 */

#include <anvil/anvil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../arch_select.h"

/* Helper to declare an external C function */
static anvil_value_t *declare_extern_func(anvil_ctx_t *ctx, anvil_module_t *mod,
                                           const char *name, anvil_type_t *ret_type,
                                           anvil_type_t **param_types, size_t num_params)
{
    anvil_type_t *func_type = anvil_type_func(ctx, ret_type, param_types, num_params, false);
    return anvil_module_add_extern(mod, name, func_type);
}

int main(int argc, char **argv)
{
    anvil_ctx_t *ctx;
    arch_config_t config;
    
    /* Parse architecture arguments */
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
    
    fprintf(stderr, "Generating dynamic array library for: %s\n", config.arch_name);
    
    anvil_module_t *mod = anvil_module_create(ctx, "dynarray_lib");
    if (!mod) {
        fprintf(stderr, "Failed to create module\n");
        anvil_ctx_destroy(ctx);
        return 1;
    }
    
    /* Common types */
    anvil_type_t *i32 = anvil_type_i32(ctx);
    anvil_type_t *i64 = anvil_type_i64(ctx);
    anvil_type_t *void_type = anvil_type_void(ctx);
    anvil_type_t *ptr_i32 = anvil_type_ptr(ctx, i32);
    anvil_type_t *ptr_void = anvil_type_ptr(ctx, void_type);
    
    /* Determine size type based on architecture */
    const anvil_arch_info_t *arch_info = anvil_ctx_get_arch_info(ctx);
    anvil_type_t *size_type = (arch_info->ptr_size == 8) ? i64 : i32;
    
    /* Declare external C functions */
    anvil_type_t *malloc_params[] = { size_type };
    anvil_value_t *malloc_func = declare_extern_func(ctx, mod, "malloc", ptr_void, malloc_params, 1);
    
    anvil_type_t *free_params[] = { ptr_void };
    anvil_value_t *free_func = declare_extern_func(ctx, mod, "free", void_type, free_params, 1);
    
    anvil_type_t *memcpy_params[] = { ptr_void, ptr_void, size_type };
    anvil_value_t *memcpy_func = declare_extern_func(ctx, mod, "memcpy", ptr_void, memcpy_params, 3);
    
    /* ================================================================
     * array_create: int* array_create(int capacity)
     * Allocates memory for 'capacity' integers
     * ================================================================ */
    {
        anvil_type_t *params[] = { i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, ptr_i32, params, 1, false);
        anvil_func_t *func = anvil_func_create(mod, "array_create", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_set_insert_point(ctx, entry);
        
        anvil_value_t *capacity = anvil_func_get_param(func, 0);
        
        /* size = capacity * sizeof(int) = capacity * 4 */
        anvil_value_t *four = anvil_const_i32(ctx, 4);
        anvil_value_t *size = anvil_build_mul(ctx, capacity, four, "size");
        
        /* Convert to size_t if needed */
        anvil_value_t *size_arg;
        if (arch_info->ptr_size == 8) {
            size_arg = anvil_build_zext(ctx, size, size_type, "size_ext");
        } else {
            size_arg = size;
        }
        
        /* Call malloc */
        anvil_value_t *args[] = { size_arg };
        anvil_type_t *malloc_ret_type = ptr_void;
        anvil_value_t *ptr = anvil_build_call(ctx, malloc_ret_type, malloc_func, args, 1, "ptr");
        
        /* Bitcast void* to int* */
        anvil_value_t *result = anvil_build_bitcast(ctx, ptr, ptr_i32, "result");
        anvil_build_ret(ctx, result);
    }
    
    /* ================================================================
     * array_destroy: void array_destroy(int* arr)
     * Frees the array memory
     * ================================================================ */
    {
        anvil_type_t *params[] = { ptr_i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, void_type, params, 1, false);
        anvil_func_t *func = anvil_func_create(mod, "array_destroy", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_set_insert_point(ctx, entry);
        
        anvil_value_t *arr = anvil_func_get_param(func, 0);
        
        /* Bitcast int* to void* */
        anvil_value_t *ptr = anvil_build_bitcast(ctx, arr, ptr_void, "ptr");
        
        /* Call free */
        anvil_value_t *args[] = { ptr };
        anvil_build_call(ctx, void_type, free_func, args, 1, NULL);
        
        anvil_build_ret_void(ctx);
    }
    
    /* ================================================================
     * array_copy: int* array_copy(int* src, int count)
     * Creates a copy of the array
     * ================================================================ */
    {
        anvil_type_t *params[] = { ptr_i32, i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, ptr_i32, params, 2, false);
        anvil_func_t *func = anvil_func_create(mod, "array_copy", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_set_insert_point(ctx, entry);
        
        anvil_value_t *src = anvil_func_get_param(func, 0);
        anvil_value_t *count = anvil_func_get_param(func, 1);
        
        /* size = count * 4 */
        anvil_value_t *four = anvil_const_i32(ctx, 4);
        anvil_value_t *size = anvil_build_mul(ctx, count, four, "size");
        
        /* Convert to size_t if needed */
        anvil_value_t *size_arg;
        if (arch_info->ptr_size == 8) {
            size_arg = anvil_build_zext(ctx, size, size_type, "size_ext");
        } else {
            size_arg = size;
        }
        
        /* Allocate destination */
        anvil_value_t *malloc_args[] = { size_arg };
        anvil_value_t *dest_void = anvil_build_call(ctx, ptr_void, malloc_func, malloc_args, 1, "dest_void");
        
        /* Bitcast for memcpy */
        anvil_value_t *src_void = anvil_build_bitcast(ctx, src, ptr_void, "src_void");
        
        /* Call memcpy(dest, src, size) */
        anvil_value_t *memcpy_args[] = { dest_void, src_void, size_arg };
        anvil_build_call(ctx, ptr_void, memcpy_func, memcpy_args, 3, NULL);
        
        /* Bitcast result to int* */
        anvil_value_t *result = anvil_build_bitcast(ctx, dest_void, ptr_i32, "result");
        anvil_build_ret(ctx, result);
    }
    
    /* ================================================================
     * array_sum: int array_sum(int* arr, int count)
     * Returns sum of all elements
     * ================================================================ */
    {
        anvil_type_t *params[] = { ptr_i32, i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
        anvil_func_t *func = anvil_func_create(mod, "array_sum", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_block_t *loop_cond = anvil_block_create(func, "loop_cond");
        anvil_block_t *loop_body = anvil_block_create(func, "loop_body");
        anvil_block_t *loop_end = anvil_block_create(func, "loop_end");
        
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *arr = anvil_func_get_param(func, 0);
        anvil_value_t *count = anvil_func_get_param(func, 1);
        
        /* Allocate locals */
        anvil_value_t *sum_ptr = anvil_build_alloca(ctx, i32, "sum");
        anvil_value_t *i_ptr = anvil_build_alloca(ctx, i32, "i");
        
        /* Initialize: sum = 0, i = 0 */
        anvil_value_t *zero = anvil_const_i32(ctx, 0);
        anvil_build_store(ctx, zero, sum_ptr);
        anvil_build_store(ctx, zero, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        /* Loop condition: while (i < count) */
        anvil_set_insert_point(ctx, loop_cond);
        anvil_value_t *i_val = anvil_build_load(ctx, i32, i_ptr, "i_val");
        anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_val, count, "cmp");
        anvil_build_br_cond(ctx, cmp, loop_body, loop_end);
        
        /* Loop body: sum += arr[i]; i++ */
        anvil_set_insert_point(ctx, loop_body);
        anvil_value_t *i_val2 = anvil_build_load(ctx, i32, i_ptr, "i_val2");
        anvil_value_t *indices[] = { i_val2 };
        anvil_value_t *elem_ptr = anvil_build_gep(ctx, i32, arr, indices, 1, "elem_ptr");
        anvil_value_t *elem = anvil_build_load(ctx, i32, elem_ptr, "elem");
        anvil_value_t *sum_val = anvil_build_load(ctx, i32, sum_ptr, "sum_val");
        anvil_value_t *new_sum = anvil_build_add(ctx, sum_val, elem, "new_sum");
        anvil_build_store(ctx, new_sum, sum_ptr);
        
        anvil_value_t *one = anvil_const_i32(ctx, 1);
        anvil_value_t *new_i = anvil_build_add(ctx, i_val2, one, "new_i");
        anvil_build_store(ctx, new_i, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        /* Return sum */
        anvil_set_insert_point(ctx, loop_end);
        anvil_value_t *result = anvil_build_load(ctx, i32, sum_ptr, "result");
        anvil_build_ret(ctx, result);
    }
    
    /* ================================================================
     * array_max: int array_max(int* arr, int count)
     * Returns maximum element (assumes count > 0)
     * ================================================================ */
    {
        anvil_type_t *params[] = { ptr_i32, i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
        anvil_func_t *func = anvil_func_create(mod, "array_max", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_block_t *loop_cond = anvil_block_create(func, "loop_cond");
        anvil_block_t *loop_body = anvil_block_create(func, "loop_body");
        anvil_block_t *update_max = anvil_block_create(func, "update_max");
        anvil_block_t *loop_inc = anvil_block_create(func, "loop_inc");
        anvil_block_t *loop_end = anvil_block_create(func, "loop_end");
        
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *arr = anvil_func_get_param(func, 0);
        anvil_value_t *count = anvil_func_get_param(func, 1);
        
        /* Allocate locals */
        anvil_value_t *max_ptr = anvil_build_alloca(ctx, i32, "max");
        anvil_value_t *i_ptr = anvil_build_alloca(ctx, i32, "i");
        
        /* Initialize: max = arr[0], i = 1 */
        anvil_value_t *zero = anvil_const_i32(ctx, 0);
        anvil_value_t *one = anvil_const_i32(ctx, 1);
        anvil_value_t *idx0[] = { zero };
        anvil_value_t *first_ptr = anvil_build_gep(ctx, i32, arr, idx0, 1, "first_ptr");
        anvil_value_t *first = anvil_build_load(ctx, i32, first_ptr, "first");
        anvil_build_store(ctx, first, max_ptr);
        anvil_build_store(ctx, one, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        /* Loop condition */
        anvil_set_insert_point(ctx, loop_cond);
        anvil_value_t *i_val = anvil_build_load(ctx, i32, i_ptr, "i_val");
        anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_val, count, "cmp");
        anvil_build_br_cond(ctx, cmp, loop_body, loop_end);
        
        /* Loop body: check if arr[i] > max */
        anvil_set_insert_point(ctx, loop_body);
        anvil_value_t *i_val2 = anvil_build_load(ctx, i32, i_ptr, "i_val2");
        anvil_value_t *indices[] = { i_val2 };
        anvil_value_t *elem_ptr = anvil_build_gep(ctx, i32, arr, indices, 1, "elem_ptr");
        anvil_value_t *elem = anvil_build_load(ctx, i32, elem_ptr, "elem");
        anvil_value_t *max_val = anvil_build_load(ctx, i32, max_ptr, "max_val");
        anvil_value_t *is_greater = anvil_build_cmp_gt(ctx, elem, max_val, "is_greater");
        anvil_build_br_cond(ctx, is_greater, update_max, loop_inc);
        
        /* Update max */
        anvil_set_insert_point(ctx, update_max);
        anvil_value_t *elem2 = anvil_build_load(ctx, i32, elem_ptr, "elem2");
        anvil_build_store(ctx, elem2, max_ptr);
        anvil_build_br(ctx, loop_inc);
        
        /* Increment i */
        anvil_set_insert_point(ctx, loop_inc);
        anvil_value_t *i_val3 = anvil_build_load(ctx, i32, i_ptr, "i_val3");
        anvil_value_t *new_i = anvil_build_add(ctx, i_val3, one, "new_i");
        anvil_build_store(ctx, new_i, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        /* Return max */
        anvil_set_insert_point(ctx, loop_end);
        anvil_value_t *result = anvil_build_load(ctx, i32, max_ptr, "result");
        anvil_build_ret(ctx, result);
    }
    
    /* ================================================================
     * array_min: int array_min(int* arr, int count)
     * Returns minimum element (assumes count > 0)
     * ================================================================ */
    {
        anvil_type_t *params[] = { ptr_i32, i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 2, false);
        anvil_func_t *func = anvil_func_create(mod, "array_min", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_block_t *loop_cond = anvil_block_create(func, "loop_cond");
        anvil_block_t *loop_body = anvil_block_create(func, "loop_body");
        anvil_block_t *update_min = anvil_block_create(func, "update_min");
        anvil_block_t *loop_inc = anvil_block_create(func, "loop_inc");
        anvil_block_t *loop_end = anvil_block_create(func, "loop_end");
        
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *arr = anvil_func_get_param(func, 0);
        anvil_value_t *count = anvil_func_get_param(func, 1);
        
        anvil_value_t *min_ptr = anvil_build_alloca(ctx, i32, "min");
        anvil_value_t *i_ptr = anvil_build_alloca(ctx, i32, "i");
        
        anvil_value_t *zero = anvil_const_i32(ctx, 0);
        anvil_value_t *one = anvil_const_i32(ctx, 1);
        anvil_value_t *idx0[] = { zero };
        anvil_value_t *first_ptr = anvil_build_gep(ctx, i32, arr, idx0, 1, "first_ptr");
        anvil_value_t *first = anvil_build_load(ctx, i32, first_ptr, "first");
        anvil_build_store(ctx, first, min_ptr);
        anvil_build_store(ctx, one, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        anvil_set_insert_point(ctx, loop_cond);
        anvil_value_t *i_val = anvil_build_load(ctx, i32, i_ptr, "i_val");
        anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_val, count, "cmp");
        anvil_build_br_cond(ctx, cmp, loop_body, loop_end);
        
        anvil_set_insert_point(ctx, loop_body);
        anvil_value_t *i_val2 = anvil_build_load(ctx, i32, i_ptr, "i_val2");
        anvil_value_t *indices[] = { i_val2 };
        anvil_value_t *elem_ptr = anvil_build_gep(ctx, i32, arr, indices, 1, "elem_ptr");
        anvil_value_t *elem = anvil_build_load(ctx, i32, elem_ptr, "elem");
        anvil_value_t *min_val = anvil_build_load(ctx, i32, min_ptr, "min_val");
        anvil_value_t *is_less = anvil_build_cmp_lt(ctx, elem, min_val, "is_less");
        anvil_build_br_cond(ctx, is_less, update_min, loop_inc);
        
        anvil_set_insert_point(ctx, update_min);
        anvil_value_t *elem2 = anvil_build_load(ctx, i32, elem_ptr, "elem2");
        anvil_build_store(ctx, elem2, min_ptr);
        anvil_build_br(ctx, loop_inc);
        
        anvil_set_insert_point(ctx, loop_inc);
        anvil_value_t *i_val3 = anvil_build_load(ctx, i32, i_ptr, "i_val3");
        anvil_value_t *new_i = anvil_build_add(ctx, i_val3, one, "new_i");
        anvil_build_store(ctx, new_i, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        anvil_set_insert_point(ctx, loop_end);
        anvil_value_t *result = anvil_build_load(ctx, i32, min_ptr, "result");
        anvil_build_ret(ctx, result);
    }
    
    /* ================================================================
     * array_count_if: int array_count_if(int* arr, int n, int threshold)
     * Counts elements greater than threshold
     * ================================================================ */
    {
        anvil_type_t *params[] = { ptr_i32, i32, i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, i32, params, 3, false);
        anvil_func_t *func = anvil_func_create(mod, "array_count_if", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_block_t *loop_cond = anvil_block_create(func, "loop_cond");
        anvil_block_t *loop_body = anvil_block_create(func, "loop_body");
        anvil_block_t *inc_count = anvil_block_create(func, "inc_count");
        anvil_block_t *loop_inc = anvil_block_create(func, "loop_inc");
        anvil_block_t *loop_end = anvil_block_create(func, "loop_end");
        
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *arr = anvil_func_get_param(func, 0);
        anvil_value_t *n = anvil_func_get_param(func, 1);
        anvil_value_t *threshold = anvil_func_get_param(func, 2);
        
        anvil_value_t *count_ptr = anvil_build_alloca(ctx, i32, "count");
        anvil_value_t *i_ptr = anvil_build_alloca(ctx, i32, "i");
        
        anvil_value_t *zero = anvil_const_i32(ctx, 0);
        anvil_value_t *one = anvil_const_i32(ctx, 1);
        anvil_build_store(ctx, zero, count_ptr);
        anvil_build_store(ctx, zero, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        anvil_set_insert_point(ctx, loop_cond);
        anvil_value_t *i_val = anvil_build_load(ctx, i32, i_ptr, "i_val");
        anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_val, n, "cmp");
        anvil_build_br_cond(ctx, cmp, loop_body, loop_end);
        
        anvil_set_insert_point(ctx, loop_body);
        anvil_value_t *i_val2 = anvil_build_load(ctx, i32, i_ptr, "i_val2");
        anvil_value_t *indices[] = { i_val2 };
        anvil_value_t *elem_ptr = anvil_build_gep(ctx, i32, arr, indices, 1, "elem_ptr");
        anvil_value_t *elem = anvil_build_load(ctx, i32, elem_ptr, "elem");
        anvil_value_t *is_greater = anvil_build_cmp_gt(ctx, elem, threshold, "is_greater");
        anvil_build_br_cond(ctx, is_greater, inc_count, loop_inc);
        
        anvil_set_insert_point(ctx, inc_count);
        anvil_value_t *count_val = anvil_build_load(ctx, i32, count_ptr, "count_val");
        anvil_value_t *new_count = anvil_build_add(ctx, count_val, one, "new_count");
        anvil_build_store(ctx, new_count, count_ptr);
        anvil_build_br(ctx, loop_inc);
        
        anvil_set_insert_point(ctx, loop_inc);
        anvil_value_t *i_val3 = anvil_build_load(ctx, i32, i_ptr, "i_val3");
        anvil_value_t *new_i = anvil_build_add(ctx, i_val3, one, "new_i");
        anvil_build_store(ctx, new_i, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        anvil_set_insert_point(ctx, loop_end);
        anvil_value_t *result = anvil_build_load(ctx, i32, count_ptr, "result");
        anvil_build_ret(ctx, result);
    }
    
    /* ================================================================
     * array_scale: void array_scale(int* arr, int n, int factor)
     * Multiplies all elements by factor (in-place)
     * ================================================================ */
    {
        anvil_type_t *params[] = { ptr_i32, i32, i32 };
        anvil_type_t *func_type = anvil_type_func(ctx, void_type, params, 3, false);
        anvil_func_t *func = anvil_func_create(mod, "array_scale", func_type, ANVIL_LINK_EXTERNAL);
        
        anvil_block_t *entry = anvil_func_get_entry(func);
        anvil_block_t *loop_cond = anvil_block_create(func, "loop_cond");
        anvil_block_t *loop_body = anvil_block_create(func, "loop_body");
        anvil_block_t *loop_end = anvil_block_create(func, "loop_end");
        
        anvil_set_insert_point(ctx, entry);
        anvil_value_t *arr = anvil_func_get_param(func, 0);
        anvil_value_t *n = anvil_func_get_param(func, 1);
        anvil_value_t *factor = anvil_func_get_param(func, 2);
        
        anvil_value_t *i_ptr = anvil_build_alloca(ctx, i32, "i");
        
        anvil_value_t *zero = anvil_const_i32(ctx, 0);
        anvil_value_t *one = anvil_const_i32(ctx, 1);
        anvil_build_store(ctx, zero, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        anvil_set_insert_point(ctx, loop_cond);
        anvil_value_t *i_val = anvil_build_load(ctx, i32, i_ptr, "i_val");
        anvil_value_t *cmp = anvil_build_cmp_lt(ctx, i_val, n, "cmp");
        anvil_build_br_cond(ctx, cmp, loop_body, loop_end);
        
        anvil_set_insert_point(ctx, loop_body);
        anvil_value_t *i_val2 = anvil_build_load(ctx, i32, i_ptr, "i_val2");
        anvil_value_t *indices[] = { i_val2 };
        anvil_value_t *elem_ptr = anvil_build_gep(ctx, i32, arr, indices, 1, "elem_ptr");
        anvil_value_t *elem = anvil_build_load(ctx, i32, elem_ptr, "elem");
        anvil_value_t *scaled = anvil_build_mul(ctx, elem, factor, "scaled");
        anvil_build_store(ctx, scaled, elem_ptr);
        
        anvil_value_t *new_i = anvil_build_add(ctx, i_val2, one, "new_i");
        anvil_build_store(ctx, new_i, i_ptr);
        anvil_build_br(ctx, loop_cond);
        
        anvil_set_insert_point(ctx, loop_end);
        anvil_build_ret_void(ctx);
    }
    
    /* Generate code */
    char *output = NULL;
    size_t len = 0;
    anvil_error_t err = anvil_module_codegen(mod, &output, &len);
    
    if (err == ANVIL_OK && output) {
        printf("%s", output);
        free(output);
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
