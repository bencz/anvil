#include <stdio.h>
#include <string.h>
#include <assert.h>
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

TEST(arena_create) {
    AnvilArena* arena = anvil_arena_new();
    ASSERT(arena != NULL);
    ASSERT(arena->first != NULL);
    ASSERT(arena->current != NULL);
    anvil_arena_free(arena);
}

TEST(arena_alloc) {
    AnvilArena* arena = anvil_arena_new();
    
    void* p1 = anvil_arena_alloc(arena, 100);
    ASSERT(p1 != NULL);
    
    void* p2 = anvil_arena_alloc(arena, 200);
    ASSERT(p2 != NULL);
    ASSERT(p2 != p1);
    
    void* p3 = anvil_arena_alloc(arena, 50);
    ASSERT(p3 != NULL);
    
    anvil_arena_free(arena);
}

TEST(arena_strdup) {
    AnvilArena* arena = anvil_arena_new();
    
    const char* original = "Hello, World!";
    char* copy = anvil_arena_strdup(arena, original);
    
    ASSERT(copy != NULL);
    ASSERT(copy != original);
    ASSERT(strcmp(copy, original) == 0);
    
    anvil_arena_free(arena);
}

TEST(arena_strndup) {
    AnvilArena* arena = anvil_arena_new();
    
    const char* original = "Hello, World!";
    char* copy = anvil_arena_strndup(arena, original, 5);
    
    ASSERT(copy != NULL);
    ASSERT(strcmp(copy, "Hello") == 0);
    
    anvil_arena_free(arena);
}

TEST(arena_memdup) {
    AnvilArena* arena = anvil_arena_new();
    
    int data[] = {1, 2, 3, 4, 5};
    int* copy = anvil_arena_memdup(arena, data, sizeof(data));
    
    ASSERT(copy != NULL);
    ASSERT(copy != data);
    ASSERT(memcmp(copy, data, sizeof(data)) == 0);
    
    anvil_arena_free(arena);
}

TEST(arena_large_alloc) {
    AnvilArena* arena = anvil_arena_new();
    
    void* large = anvil_arena_alloc(arena, 1024 * 1024);
    ASSERT(large != NULL);
    
    anvil_arena_free(arena);
}

TEST(arena_reset) {
    AnvilArena* arena = anvil_arena_new();
    
    anvil_arena_alloc(arena, 1000);
    anvil_arena_alloc(arena, 2000);
    
    size_t before = arena->total_allocated;
    ASSERT(before > 0);
    
    anvil_arena_reset(arena);
    ASSERT(arena->total_allocated == 0);
    
    anvil_arena_free(arena);
}

int main(void) {
    printf("=== Arena Tests ===\n");
    
    RUN_TEST(arena_create);
    RUN_TEST(arena_alloc);
    RUN_TEST(arena_strdup);
    RUN_TEST(arena_strndup);
    RUN_TEST(arena_memdup);
    RUN_TEST(arena_large_alloc);
    RUN_TEST(arena_reset);
    
    printf("\n=== Results ===\n");
    printf("Passed: %d\n", tests_passed);
    printf("Failed: %d\n", tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
