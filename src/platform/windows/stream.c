#include "../stream.h"

#include <stdint.h>
#include <stdlib.h>

static char *capture_stream(anvil_dump_stream_fn write, void *object)
{
    FILE *stream = tmpfile();
    if (!stream)
        return NULL;

    write(stream, object);
    if (ferror(stream) || fflush(stream) != 0)
    {
        fclose(stream);
        return NULL;
    }

    int64_t size = _ftelli64(stream);
    if (size < 0 || (uint64_t)size >= SIZE_MAX || _fseeki64(stream, 0, SEEK_SET) != 0)
    {
        fclose(stream);
        return NULL;
    }

    char *result = malloc((size_t)size + 1);
    if (!result)
    {
        fclose(stream);
        return NULL;
    }

    size_t read = fread(result, 1, (size_t)size, stream);
    int failed = ferror(stream);
    if (fclose(stream) != 0 || failed || read != (size_t)size)
    {
        free(result);
        return NULL;
    }

    result[read] = '\0';
    return result;
}

const anvil_stream_platform_ops_t anvil_stream_platform = {
    .capture = capture_stream,
};
