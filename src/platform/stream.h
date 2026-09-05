#ifndef ANVIL_PLATFORM_STREAM_H
#define ANVIL_PLATFORM_STREAM_H

#include <stdio.h>

typedef void (*anvil_dump_stream_fn)(FILE *stream, void *object);

typedef struct {
    char *(*capture)(anvil_dump_stream_fn write, void *object);
} anvil_stream_platform_ops_t;

extern const anvil_stream_platform_ops_t anvil_stream_platform;

#endif
