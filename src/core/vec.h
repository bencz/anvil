#ifndef ANVIL_VEC_H
#define ANVIL_VEC_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilVec {
    void* data;
    size_t len;
    size_t cap;
    size_t elem_size;
} AnvilVec;

void anvil_vec_init(AnvilVec* vec, size_t elem_size);
void anvil_vec_free(AnvilVec* vec);
void anvil_vec_clear(AnvilVec* vec);
void* anvil_vec_push(AnvilVec* vec);
void anvil_vec_pop(AnvilVec* vec);
void* anvil_vec_get(const AnvilVec* vec, size_t idx);
void* anvil_vec_last(const AnvilVec* vec);
size_t anvil_vec_len(const AnvilVec* vec);
bool anvil_vec_empty(const AnvilVec* vec);
void anvil_vec_reserve(AnvilVec* vec, size_t cap);
void anvil_vec_resize(AnvilVec* vec, size_t len);
void* anvil_vec_insert(AnvilVec* vec, size_t idx);
void anvil_vec_remove(AnvilVec* vec, size_t idx);

#define ANVIL_VEC_FOR_EACH(vec, type, var) \
    for (type* var = (type*)(vec)->data; \
         var < (type*)(vec)->data + (vec)->len; \
         var++)

#define ANVIL_VEC_PUSH_VAL(vec, type, val) \
    do { type* _p = (type*)anvil_vec_push(vec); if (_p) *_p = (val); } while(0)

#define ANVIL_VEC_GET_VAL(vec, type, idx) \
    (*(type*)anvil_vec_get(vec, idx))

#ifdef __cplusplus
}
#endif

#endif
