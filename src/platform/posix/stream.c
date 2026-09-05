#include "../stream.h"

#include <stdlib.h>

static char *capture_stream(anvil_dump_stream_fn write, void *object)
{
    char *result = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&result, &size);
    if (!stream)
        return NULL;

    write(stream, object);
    int failed = ferror(stream);
    if (fclose(stream) != 0 || failed)
    {
        free(result);
        return NULL;
    }

    return result;
}

const anvil_stream_platform_ops_t anvil_stream_platform = {
    .capture = capture_stream,
};
