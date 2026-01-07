#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../../src/ir/types_internal.h"
#include "../../src/core/arena.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) void test_##name(void)
#define RUN_TEST(name) do { \
    printf("Running %s... ", #name); \
    test_##name(); \
    printf("PASSED\n"); \
    tests_passed++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

TEST(basic_types) {
    AnvilType* void_t = anvil_type_void();
    ASSERT(void_t != NULL);
    ASSERT(void_t->kind == ANVIL_TYPE_VOID);
    ASSERT(anvil_type_is_void(void_t));
    
    AnvilType* bool_t = anvil_type_bool();
    ASSERT(bool_t != NULL);
    ASSERT(bool_t->kind == ANVIL_TYPE_BOOL);
    ASSERT(bool_t->size == 1);
    
    AnvilType* i32_t = anvil_type_i32();
    ASSERT(i32_t != NULL);
    ASSERT(i32_t->kind == ANVIL_TYPE_I32);
    ASSERT(i32_t->size == 4);
    ASSERT(anvil_type_is_integer(i32_t));
    ASSERT(anvil_type_is_signed(i32_t));
    
    AnvilType* u64_t = anvil_type_u64();
    ASSERT(u64_t != NULL);
    ASSERT(u64_t->kind == ANVIL_TYPE_U64);
    ASSERT(u64_t->size == 8);
    ASSERT(anvil_type_is_integer(u64_t));
    ASSERT(anvil_type_is_unsigned(u64_t));
    
    AnvilType* f32_t = anvil_type_f32();
    ASSERT(f32_t != NULL);
    ASSERT(f32_t->kind == ANVIL_TYPE_F32);
    ASSERT(f32_t->size == 4);
    ASSERT(anvil_type_is_float(f32_t));
    
    AnvilType* f64_t = anvil_type_f64();
    ASSERT(f64_t != NULL);
    ASSERT(f64_t->kind == ANVIL_TYPE_F64);
    ASSERT(f64_t->size == 8);
    ASSERT(anvil_type_is_float(f64_t));
}

TEST(pointer_types) {
    AnvilArena* arena = anvil_arena_new();
    
    AnvilType* i32_t = anvil_type_i32();
    AnvilType* ptr_t = anvil_type_ptr_create(arena, i32_t);
    
    ASSERT(ptr_t != NULL);
    ASSERT(ptr_t->kind == ANVIL_TYPE_PTR);
    ASSERT(ptr_t->size == 8);
    ASSERT(anvil_type_is_ptr(ptr_t));
    ASSERT(ptr_t->ptr.pointee == i32_t);
    
    anvil_arena_free(arena);
}

TEST(array_types) {
    AnvilArena* arena = anvil_arena_new();
    
    AnvilType* i32_t = anvil_type_i32();
    AnvilType* arr_t = anvil_type_array_create(arena, i32_t, 10);
    
    ASSERT(arr_t != NULL);
    ASSERT(arr_t->kind == ANVIL_TYPE_ARRAY);
    ASSERT(arr_t->size == 40);
    ASSERT(arr_t->array.elem == i32_t);
    ASSERT(arr_t->array.count == 10);
    ASSERT(anvil_type_is_aggregate(arr_t));
    
    anvil_arena_free(arena);
}

TEST(struct_types) {
    AnvilArena* arena = anvil_arena_new();
    
    AnvilType* struct_t = anvil_type_struct_create(arena, "Point");
    ASSERT(struct_t != NULL);
    ASSERT(struct_t->kind == ANVIL_TYPE_STRUCT);
    ASSERT(strcmp(struct_t->struct_type.name, "Point") == 0);
    
    anvil_type_struct_add_field(struct_t, arena, "x", anvil_type_i32());
    anvil_type_struct_add_field(struct_t, arena, "y", anvil_type_i32());
    
    ASSERT(anvil_vec_len(&struct_t->struct_type.fields) == 2);
    ASSERT(struct_t->size == 8);
    ASSERT(anvil_type_is_aggregate(struct_t));
    
    anvil_arena_free(arena);
}

TEST(type_equality) {
    AnvilArena* arena = anvil_arena_new();
    
    ASSERT(anvil_type_eq(anvil_type_i32(), anvil_type_i32()));
    ASSERT(!anvil_type_eq(anvil_type_i32(), anvil_type_i64()));
    ASSERT(!anvil_type_eq(anvil_type_i32(), anvil_type_f32()));
    
    AnvilType* ptr1 = anvil_type_ptr_create(arena, anvil_type_i32());
    AnvilType* ptr2 = anvil_type_ptr_create(arena, anvil_type_i32());
    AnvilType* ptr3 = anvil_type_ptr_create(arena, anvil_type_i64());
    
    ASSERT(anvil_type_eq(ptr1, ptr2));
    ASSERT(!anvil_type_eq(ptr1, ptr3));
    
    anvil_arena_free(arena);
}

int main(void) {
    printf("=== Type System Tests ===\n");
    
    RUN_TEST(basic_types);
    RUN_TEST(pointer_types);
    RUN_TEST(array_types);
    RUN_TEST(struct_types);
    RUN_TEST(type_equality);
    
    printf("\n=== Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
