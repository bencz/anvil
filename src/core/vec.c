#include "vec.h"
#include <stdlib.h>
#include <string.h>

void anvil_vec_init(AnvilVec* vec, size_t elem_size) {
    vec->data = NULL;
    vec->len = 0;
    vec->cap = 0;
    vec->elem_size = elem_size;
}

void anvil_vec_free(AnvilVec* vec) {
    if (vec->data) {
        free(vec->data);
        vec->data = NULL;
    }
    vec->len = 0;
    vec->cap = 0;
}

void anvil_vec_clear(AnvilVec* vec) {
    vec->len = 0;
}

void anvil_vec_reserve(AnvilVec* vec, size_t cap) {
    if (cap <= vec->cap) return;
    void* new_data = realloc(vec->data, cap * vec->elem_size);
    if (new_data) {
        vec->data = new_data;
        vec->cap = cap;
    }
}

void* anvil_vec_push(AnvilVec* vec) {
    if (vec->len >= vec->cap) {
        size_t new_cap = vec->cap ? vec->cap * 2 : 8;
        anvil_vec_reserve(vec, new_cap);
        if (vec->cap < new_cap && vec->cap < vec->len + 1) return NULL;
    }
    void* ptr = (char*)vec->data + vec->len * vec->elem_size;
    memset(ptr, 0, vec->elem_size);
    vec->len++;
    return ptr;
}

void anvil_vec_pop(AnvilVec* vec) {
    if (vec->len > 0) vec->len--;
}

void* anvil_vec_get(const AnvilVec* vec, size_t idx) {
    if (idx >= vec->len) return NULL;
    return (char*)vec->data + idx * vec->elem_size;
}

void* anvil_vec_last(const AnvilVec* vec) {
    if (vec->len == 0) return NULL;
    return (char*)vec->data + (vec->len - 1) * vec->elem_size;
}

size_t anvil_vec_len(const AnvilVec* vec) {
    return vec->len;
}

bool anvil_vec_empty(const AnvilVec* vec) {
    return vec->len == 0;
}

void anvil_vec_resize(AnvilVec* vec, size_t len) {
    if (len > vec->cap) {
        anvil_vec_reserve(vec, len);
    }
    if (len > vec->len) {
        memset((char*)vec->data + vec->len * vec->elem_size, 0, 
               (len - vec->len) * vec->elem_size);
    }
    vec->len = len;
}

void* anvil_vec_insert(AnvilVec* vec, size_t idx) {
    if (idx > vec->len) return NULL;
    if (vec->len >= vec->cap) {
        size_t new_cap = vec->cap ? vec->cap * 2 : 8;
        anvil_vec_reserve(vec, new_cap);
    }
    if (idx < vec->len) {
        memmove((char*)vec->data + (idx + 1) * vec->elem_size,
                (char*)vec->data + idx * vec->elem_size,
                (vec->len - idx) * vec->elem_size);
    }
    void* ptr = (char*)vec->data + idx * vec->elem_size;
    memset(ptr, 0, vec->elem_size);
    vec->len++;
    return ptr;
}

void anvil_vec_remove(AnvilVec* vec, size_t idx) {
    if (idx >= vec->len) return;
    if (idx < vec->len - 1) {
        memmove((char*)vec->data + idx * vec->elem_size,
                (char*)vec->data + (idx + 1) * vec->elem_size,
                (vec->len - idx - 1) * vec->elem_size);
    }
    vec->len--;
}
