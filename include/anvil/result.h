#ifndef ANVIL_RESULT_H
#define ANVIL_RESULT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnvilCompileResult {
    char* code;
    size_t length;
    char* errors;
    int num_errors;
} AnvilCompileResult;

void anvil_result_free(AnvilCompileResult* result);

#ifdef __cplusplus
}
#endif

#endif
