#include "st_artifact_bundle.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    st_artifact_allocator_t allocator;
} bundle_impl_t;

typedef struct {
    uint32_t state[8];
    uint64_t byte_count;
    uint8_t block[64];
    size_t block_size;
} sha256_t;

typedef struct {
    st_artifact_allocator_t allocator;
    char *bytes;
    size_t length;
    size_t capacity;
    bool overflow;
} text_builder_t;

typedef struct {
    const char *bytes;
    size_t length;
} bundle_symbol_t;

static int bundle_symbol_compare(const void *left, const void *right)
{
    const bundle_symbol_t *a = left;
    const bundle_symbol_t *b = right;
    size_t common = a->length < b->length ? a->length : b->length;
    int order = common != 0u ? memcmp(a->bytes, b->bytes, common) : 0;
    if (order != 0) return order;
    return a->length < b->length ? -1 : a->length != b->length;
}

static void *default_allocate(void *user, size_t size)
{
    (void)user;
    return malloc(size);
}

static void default_deallocate(void *user, void *pointer)
{
    (void)user;
    free(pointer);
}

static void release(st_artifact_allocator_t allocator, void *pointer)
{
    if (pointer != NULL) allocator.deallocate(allocator.user, pointer);
}

static bool add_size(size_t left, size_t right, size_t *out)
{
    if (left > SIZE_MAX - right) return false;
    *out = left + right;
    return true;
}

static void *allocate_array(st_artifact_allocator_t allocator, size_t count,
                            size_t element_size)
{
    void *memory;
    if (count == 0u) return NULL;
    if (element_size == 0u || count > SIZE_MAX / element_size) return NULL;
    memory = allocator.allocate(allocator.user, count * element_size);
    if (memory != NULL) memset(memory, 0, count * element_size);
    return memory;
}

static uint32_t rotate_right(uint32_t value, unsigned amount)
{
    return (value >> amount) | (value << (32u - amount));
}

static uint32_t load_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24u) | ((uint32_t)bytes[1] << 16u)
        | ((uint32_t)bytes[2] << 8u) | (uint32_t)bytes[3];
}

static void store_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24u);
    bytes[1] = (uint8_t)(value >> 16u);
    bytes[2] = (uint8_t)(value >> 8u);
    bytes[3] = (uint8_t)value;
}

static void sha256_compress(sha256_t *hash, const uint8_t block[64])
{
    static const uint32_t constants[64] = {
        UINT32_C(0x428a2f98), UINT32_C(0x71374491), UINT32_C(0xb5c0fbcf),
        UINT32_C(0xe9b5dba5), UINT32_C(0x3956c25b), UINT32_C(0x59f111f1),
        UINT32_C(0x923f82a4), UINT32_C(0xab1c5ed5), UINT32_C(0xd807aa98),
        UINT32_C(0x12835b01), UINT32_C(0x243185be), UINT32_C(0x550c7dc3),
        UINT32_C(0x72be5d74), UINT32_C(0x80deb1fe), UINT32_C(0x9bdc06a7),
        UINT32_C(0xc19bf174), UINT32_C(0xe49b69c1), UINT32_C(0xefbe4786),
        UINT32_C(0x0fc19dc6), UINT32_C(0x240ca1cc), UINT32_C(0x2de92c6f),
        UINT32_C(0x4a7484aa), UINT32_C(0x5cb0a9dc), UINT32_C(0x76f988da),
        UINT32_C(0x983e5152), UINT32_C(0xa831c66d), UINT32_C(0xb00327c8),
        UINT32_C(0xbf597fc7), UINT32_C(0xc6e00bf3), UINT32_C(0xd5a79147),
        UINT32_C(0x06ca6351), UINT32_C(0x14292967), UINT32_C(0x27b70a85),
        UINT32_C(0x2e1b2138), UINT32_C(0x4d2c6dfc), UINT32_C(0x53380d13),
        UINT32_C(0x650a7354), UINT32_C(0x766a0abb), UINT32_C(0x81c2c92e),
        UINT32_C(0x92722c85), UINT32_C(0xa2bfe8a1), UINT32_C(0xa81a664b),
        UINT32_C(0xc24b8b70), UINT32_C(0xc76c51a3), UINT32_C(0xd192e819),
        UINT32_C(0xd6990624), UINT32_C(0xf40e3585), UINT32_C(0x106aa070),
        UINT32_C(0x19a4c116), UINT32_C(0x1e376c08), UINT32_C(0x2748774c),
        UINT32_C(0x34b0bcb5), UINT32_C(0x391c0cb3), UINT32_C(0x4ed8aa4a),
        UINT32_C(0x5b9cca4f), UINT32_C(0x682e6ff3), UINT32_C(0x748f82ee),
        UINT32_C(0x78a5636f), UINT32_C(0x84c87814), UINT32_C(0x8cc70208),
        UINT32_C(0x90befffa), UINT32_C(0xa4506ceb), UINT32_C(0xbef9a3f7),
        UINT32_C(0xc67178f2)
    };
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    size_t index;
    for (index = 0u; index < 16u; index++)
        words[index] = load_be32(block + index * 4u);
    for (; index < 64u; index++) {
        uint32_t x = words[index - 15u];
        uint32_t y = words[index - 2u];
        uint32_t s0 = rotate_right(x, 7u) ^ rotate_right(x, 18u) ^ (x >> 3u);
        uint32_t s1 = rotate_right(y, 17u) ^ rotate_right(y, 19u) ^ (y >> 10u);
        words[index] = words[index - 16u] + s0 + words[index - 7u] + s1;
    }
    a = hash->state[0]; b = hash->state[1]; c = hash->state[2];
    d = hash->state[3]; e = hash->state[4]; f = hash->state[5];
    g = hash->state[6]; h = hash->state[7];
    for (index = 0u; index < 64u; index++) {
        uint32_t sum1 = rotate_right(e, 6u) ^ rotate_right(e, 11u)
            ^ rotate_right(e, 25u);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temporary1 = h + sum1 + choice + constants[index]
            + words[index];
        uint32_t sum0 = rotate_right(a, 2u) ^ rotate_right(a, 13u)
            ^ rotate_right(a, 22u);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temporary1;
        d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    hash->state[0] += a; hash->state[1] += b;
    hash->state[2] += c; hash->state[3] += d;
    hash->state[4] += e; hash->state[5] += f;
    hash->state[6] += g; hash->state[7] += h;
}

static void sha256_init(sha256_t *hash)
{
    static const uint32_t initial[8] = {
        UINT32_C(0x6a09e667), UINT32_C(0xbb67ae85),
        UINT32_C(0x3c6ef372), UINT32_C(0xa54ff53a),
        UINT32_C(0x510e527f), UINT32_C(0x9b05688c),
        UINT32_C(0x1f83d9ab), UINT32_C(0x5be0cd19)
    };
    memset(hash, 0, sizeof(*hash));
    memcpy(hash->state, initial, sizeof(initial));
}

static void sha256_update(sha256_t *hash, const void *input, size_t length)
{
    const uint8_t *bytes = input;
    while (length != 0u) {
        size_t available = sizeof(hash->block) - hash->block_size;
        size_t take = length < available ? length : available;
        memcpy(hash->block + hash->block_size, bytes, take);
        hash->block_size += take;
        hash->byte_count += (uint64_t)take;
        bytes += take;
        length -= take;
        if (hash->block_size == sizeof(hash->block)) {
            sha256_compress(hash, hash->block);
            hash->block_size = 0u;
        }
    }
}

static void sha256_finish(sha256_t *hash, uint8_t output[32])
{
    uint64_t bit_count = hash->byte_count * UINT64_C(8);
    size_t index;
    hash->block[hash->block_size++] = UINT8_C(0x80);
    if (hash->block_size > 56u) {
        memset(hash->block + hash->block_size, 0,
               sizeof(hash->block) - hash->block_size);
        sha256_compress(hash, hash->block);
        hash->block_size = 0u;
    }
    memset(hash->block + hash->block_size, 0, 56u - hash->block_size);
    for (index = 0u; index < 8u; index++)
        hash->block[63u - index] = (uint8_t)(bit_count >> (index * 8u));
    sha256_compress(hash, hash->block);
    for (index = 0u; index < 8u; index++)
        store_be32(output + index * 4u, hash->state[index]);
}

bool st_artifact_sha256(const void *bytes, size_t length, uint8_t output[32])
{
    sha256_t hash;
    if (output == NULL || (bytes == NULL && length != 0u)
            || length > UINT64_MAX / UINT64_C(8))
        return false;
    sha256_init(&hash);
    sha256_update(&hash, bytes, length);
    sha256_finish(&hash, output);
    return true;
}

static void hash_u64(sha256_t *hash, uint64_t value)
{
    uint8_t bytes[8];
    size_t index;
    for (index = 0u; index < sizeof(bytes); index++)
        bytes[index] = (uint8_t)(value >> (56u - index * 8u));
    sha256_update(hash, bytes, sizeof(bytes));
}

static bool portable_symbol(const char *symbol, size_t length)
{
    size_t index;
    unsigned char byte;
    if (symbol == NULL || length == 0u || symbol[length] != '\0') return false;
    byte = (unsigned char)symbol[0];
    if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')
            || byte == '_')) return false;
    for (index = 1u; index < length; index++) {
        byte = (unsigned char)symbol[index];
        if (!((byte >= 'A' && byte <= 'Z')
                || (byte >= 'a' && byte <= 'z')
                || (byte >= '0' && byte <= '9') || byte == '_'))
            return false;
    }
    return true;
}

static bool target_supported(anvil_arch_t target)
{
    return target == ANVIL_ARCH_X86_64 || target == ANVIL_ARCH_ARM64
        || target == ANVIL_ARCH_PPC64 || target == ANVIL_ARCH_PPC64LE
        || target == ANVIL_ARCH_ZARCH;
}

static bool syntax_supported(anvil_arch_t target, anvil_syntax_t syntax)
{
    if ((unsigned)syntax > (unsigned)ANVIL_SYNTAX_GAS) return false;
    if (target == ANVIL_ARCH_ZARCH) return syntax != ANVIL_SYNTAX_GAS;
    return syntax != ANVIL_SYNTAX_HLASM;
}

static bool abi_supported(anvil_arch_t target, anvil_abi_t abi)
{
    switch (target) {
    case ANVIL_ARCH_X86_64:
        return abi == ANVIL_ABI_DEFAULT || abi == ANVIL_ABI_SYSV
            || abi == ANVIL_ABI_DARWIN || abi == ANVIL_ABI_WIN64;
    case ANVIL_ARCH_ARM64:
        return abi == ANVIL_ABI_SYSV || abi == ANVIL_ABI_DARWIN;
    case ANVIL_ARCH_PPC64:
    case ANVIL_ARCH_PPC64LE:
        return abi == ANVIL_ABI_DEFAULT || abi == ANVIL_ABI_SYSV;
    case ANVIL_ARCH_ZARCH:
        return abi == ANVIL_ABI_DEFAULT || abi == ANVIL_ABI_MVS;
    default:
        return false;
    }
}

static const char *target_name(anvil_arch_t target)
{
    switch (target) {
    case ANVIL_ARCH_X86_64: return "x86_64";
    case ANVIL_ARCH_ARM64: return "arm64";
    case ANVIL_ARCH_PPC64: return "ppc64";
    case ANVIL_ARCH_PPC64LE: return "ppc64le";
    case ANVIL_ARCH_ZARCH: return "zarch";
    default: return "unsupported";
    }
}

static const char *abi_name(anvil_abi_t abi)
{
    switch (abi) {
    case ANVIL_ABI_DEFAULT: return "default";
    case ANVIL_ABI_SYSV: return "sysv";
    case ANVIL_ABI_DARWIN: return "darwin";
    case ANVIL_ABI_WIN64: return "win64";
    case ANVIL_ABI_MVS: return "mvs";
    }
    return "invalid";
}

static const char *syntax_name(anvil_syntax_t syntax)
{
    switch (syntax) {
    case ANVIL_SYNTAX_DEFAULT: return "default";
    case ANVIL_SYNTAX_HLASM: return "hlasm";
    case ANVIL_SYNTAX_GAS: return "gas";
    }
    return "invalid";
}

static const char *optimization_name(anvil_opt_level_t optimization)
{
    switch (optimization) {
    case ANVIL_OPT_NONE: return "O0";
    case ANVIL_OPT_DEBUG: return "Og";
    case ANVIL_OPT_BASIC: return "O1";
    case ANVIL_OPT_STANDARD: return "O2";
    case ANVIL_OPT_AGGRESSIVE: return "O3";
    }
    return "invalid";
}

static bool collision_free(const st_aot_compile_result_t *compiled,
                           st_artifact_allocator_t allocator,
                           st_artifact_bundle_status_t *status)
{
    bundle_symbol_t *symbols;
    size_t count = compiled->method_count, cursor = 0u, index;
    for (index = 0u; index < compiled->method_count; index++) {
        size_t blocks = compiled->methods[index].artifact.block_count;
        if (blocks > (SIZE_MAX - count) / 3u) {
            *status = ST_ARTIFACT_BUNDLE_ERR_OVERFLOW;
            return false;
        }
        count += blocks * 3u;
    }
    symbols = allocate_array(allocator, count, sizeof(*symbols));
    if (count != 0u && symbols == NULL) {
        *status = ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
        return false;
    }
    for (index = 0u; index < compiled->method_count; index++) {
        const st_aot_method_result_t *method = &compiled->methods[index];
        size_t block_index;
        symbols[cursor++] = (bundle_symbol_t) {
            method->symbol, method->symbol_length
        };
        for (block_index = 0u; block_index < method->artifact.block_count;
             block_index++) {
            const st_image_aot_block_artifact_t *block =
                &method->artifact.blocks[block_index];
            symbols[cursor++] = (bundle_symbol_t) {
                block->code_symbol, block->code_symbol_length
            };
            symbols[cursor++] = (bundle_symbol_t) {
                block->descriptor_symbol, block->descriptor_symbol_length
            };
            symbols[cursor++] = (bundle_symbol_t) {
                block->method_descriptor_symbol,
                block->method_descriptor_symbol_length
            };
        }
    }
    if (cursor != count) {
        release(allocator, symbols);
        *status = ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
        return false;
    }
    qsort(symbols, count, sizeof(*symbols), bundle_symbol_compare);
    for (index = 1u; index < count; index++)
        if (symbols[index - 1u].length == symbols[index].length
                && memcmp(symbols[index - 1u].bytes, symbols[index].bytes,
                          symbols[index].length) == 0) {
            release(allocator, symbols);
            *status = ST_ARTIFACT_BUNDLE_ERR_COLLISION;
            return false;
        }
    release(allocator, symbols);
    return true;
}

static bool result_is_empty(const st_artifact_bundle_t *bundle)
{
    return bundle != NULL && bundle->status == ST_ARTIFACT_BUNDLE_OK
        && bundle->artifacts == NULL && bundle->artifact_count == 0u
        && bundle->block_count == 0u
        && bundle->manifest == NULL && bundle->manifest_length == 0u
        && bundle->implementation == NULL;
}

void st_artifact_bundle_init(st_artifact_bundle_t *bundle)
{
    if (bundle == NULL) return;
    memset(bundle, 0, sizeof(*bundle));
    bundle->target = ANVIL_ARCH_NONE;
}

void st_artifact_bundle_destroy(st_artifact_bundle_t *bundle)
{
    st_artifact_allocator_t allocator = {
        default_allocate, default_deallocate, NULL
    };
    bundle_impl_t *implementation;
    size_t index;
    if (bundle == NULL) return;
    implementation = bundle->implementation;
    if (implementation != NULL) allocator = implementation->allocator;
    for (index = 0u; index < bundle->artifact_count; index++) {
        release(allocator, bundle->artifacts[index].bytes);
        release(allocator, bundle->artifacts[index].symbol);
        release(allocator, bundle->artifacts[index].name);
    }
    release(allocator, bundle->artifacts);
    release(allocator, bundle->manifest);
    release(allocator, implementation);
    st_artifact_bundle_init(bundle);
}

static bool copy_bytes(st_artifact_allocator_t allocator, const void *bytes,
                       size_t length, bool nul_terminate, void **out)
{
    unsigned char *copy;
    size_t allocation_size = length;
    *out = NULL;
    if (bytes == NULL && length != 0u) return false;
    if (nul_terminate && !add_size(allocation_size, 1u, &allocation_size))
        return false;
    if (allocation_size == 0u) return true;
    copy = allocator.allocate(allocator.user, allocation_size);
    if (copy == NULL) return false;
    if (length != 0u) memcpy(copy, bytes, length);
    if (nul_terminate) copy[length] = '\0';
    *out = copy;
    return true;
}

static st_artifact_bundle_status_t make_name(
    st_artifact_allocator_t allocator, st_artifact_blob_t *artifact,
    const char *symbol, size_t symbol_length, anvil_syntax_t syntax)
{
    const char *extension = syntax == ANVIL_SYNTAX_HLASM ? ".asm" : ".s";
    size_t extension_length = strlen(extension);
    int written;
    size_t allocation_size, length, prefix_length;
    char *name;
    if (artifact->kind == ST_ARTIFACT_METADATA_ASSEMBLY) {
        const char *metadata = syntax == ANVIL_SYNTAX_HLASM
            ? "metadata.asm" : "metadata.s";
        size_t metadata_length = strlen(metadata);
        if (!copy_bytes(allocator, metadata, metadata_length,
                        true, (void **)&artifact->name))
            return ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
        artifact->name_length = metadata_length;
        return ST_ARTIFACT_BUNDLE_OK;
    }
    if (artifact->kind == ST_ARTIFACT_LAUNCH_ASSEMBLY) {
        const char *launch = syntax == ANVIL_SYNTAX_HLASM
            ? "launch.asm" : "launch.s";
        size_t launch_length = strlen(launch);
        if (!copy_bytes(allocator, launch, launch_length,
                        true, (void **)&artifact->name))
            return ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
        artifact->name_length = launch_length;
        return ST_ARTIFACT_BUNDLE_OK;
    }
    written = snprintf(NULL, 0, "%08" PRIu32 "-", artifact->method_id);
    if (written < 0) return ST_ARTIFACT_BUNDLE_ERR_OVERFLOW;
    prefix_length = (size_t)written;
    if (!add_size(prefix_length, symbol_length, &length)
            || !add_size(length, extension_length, &length)
            || !add_size(length, 1u, &allocation_size))
        return ST_ARTIFACT_BUNDLE_ERR_OVERFLOW;
    name = allocator.allocate(allocator.user, allocation_size);
    if (name == NULL) return ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
    written = snprintf(name, prefix_length + 1u, "%08" PRIu32 "-",
                       artifact->method_id);
    if (written < 0 || (size_t)written != prefix_length) {
        release(allocator, name);
        return ST_ARTIFACT_BUNDLE_ERR_OVERFLOW;
    }
    memcpy(name + prefix_length, symbol, symbol_length);
    memcpy(name + prefix_length + symbol_length, extension,
           extension_length + 1u);
    artifact->name = name;
    artifact->name_length = length;
    return ST_ARTIFACT_BUNDLE_OK;
}

static st_artifact_bundle_status_t render_blob(
    st_artifact_allocator_t allocator, st_artifact_blob_t *artifact,
    anvil_module_t *module, const char *symbol, size_t symbol_length,
    anvil_syntax_t syntax)
{
    char *generated = NULL;
    size_t generated_length = 0u;
    anvil_error_t error;
    st_artifact_bundle_status_t status;
    status = make_name(allocator, artifact, symbol, symbol_length, syntax);
    if (status != ST_ARTIFACT_BUNDLE_OK) return status;
    if (!copy_bytes(allocator, symbol, symbol_length, true,
                    (void **)&artifact->symbol))
        return ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
    artifact->symbol_length = symbol_length;
    error = anvil_module_codegen(module, &generated, &generated_length);
    if (error != ANVIL_OK || generated == NULL || generated_length == 0u) {
        free(generated);
        return ST_ARTIFACT_BUNDLE_ERR_CODEGEN;
    }
    if (!copy_bytes(allocator, generated, generated_length, false,
                    (void **)&artifact->bytes)) {
        free(generated);
        return ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
    }
    free(generated);
    artifact->size = generated_length;
    if (artifact->size > UINT64_MAX / UINT64_C(8))
        return ST_ARTIFACT_BUNDLE_ERR_OVERFLOW;
    if (!st_artifact_sha256(artifact->bytes, artifact->size,
                            artifact->sha256))
        return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
    return ST_ARTIFACT_BUNDLE_OK;
}

static bool text_reserve(text_builder_t *builder, size_t extra)
{
    char *replacement;
    size_t required, capacity;
    if (!add_size(builder->length, extra, &required)
            || !add_size(required, 1u, &required)) {
        builder->overflow = true;
        return false;
    }
    if (required <= builder->capacity) return true;
    capacity = builder->capacity == 0u ? 512u : builder->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2u) {
            builder->overflow = true;
            return false;
        }
        capacity *= 2u;
    }
    replacement = builder->allocator.allocate(builder->allocator.user,
                                               capacity);
    if (replacement == NULL) return false;
    if (builder->length != 0u)
        memcpy(replacement, builder->bytes, builder->length);
    release(builder->allocator, builder->bytes);
    builder->bytes = replacement;
    builder->capacity = capacity;
    return true;
}

static bool text_appendf(text_builder_t *builder, const char *format, ...)
{
    va_list arguments, copy;
    int count;
    va_start(arguments, format);
    va_copy(copy, arguments);
    count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (count < 0 || !text_reserve(builder, (size_t)count)) {
        va_end(arguments);
        return false;
    }
    if (vsnprintf(builder->bytes + builder->length,
                  builder->capacity - builder->length,
                  format, arguments) != count) {
        va_end(arguments);
        builder->overflow = true;
        return false;
    }
    va_end(arguments);
    builder->length += (size_t)count;
    return true;
}

static void hash_hex(const uint8_t hash[32], char output[65])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;
    for (index = 0u; index < 32u; index++) {
        output[index * 2u] = digits[hash[index] >> 4u];
        output[index * 2u + 1u] = digits[hash[index] & UINT8_C(0x0f)];
    }
    output[64] = '\0';
}

static void hash_block_record(sha256_t *hash, uint32_t method_id,
                              const st_image_aot_block_artifact_t *block)
{
    size_t index, word;
    hash_u64(hash, method_id);
    hash_u64(hash, block->lexical_ordinal);
    hash_u64(hash, block->arity);
    hash_u64(hash, block->flags);
    hash_u64(hash, block->method_flags);
    hash_u64(hash, block->frame_root_capacity);
    hash_u64(hash, block->code_symbol_length);
    sha256_update(hash, block->code_symbol, block->code_symbol_length);
    hash_u64(hash, block->descriptor_symbol_length);
    sha256_update(hash, block->descriptor_symbol,
                  block->descriptor_symbol_length);
    hash_u64(hash, block->method_descriptor_symbol_length);
    sha256_update(hash, block->method_descriptor_symbol,
                  block->method_descriptor_symbol_length);
    hash_u64(hash, block->capture_count);
    for (index = 0u; index < block->capture_count; index++) {
        hash_u64(hash, block->captures[index].binding_id);
        hash_u64(hash, block->captures[index].kind);
    }
    hash_u64(hash, block->root_map_count);
    for (index = 0u; index < block->root_map_count; index++) {
        const st_image_root_map_metadata_t *map = &block->root_maps[index];
        hash_u64(hash, map->safepoint_id);
        hash_u64(hash, map->root_count);
        hash_u64(hash, map->bitmap_word_count);
        for (word = 0u; word < map->bitmap_word_count; word++)
            hash_u64(hash, map->live_root_bitmap[word]);
    }
}

static void compute_bundle_hash(st_artifact_bundle_t *bundle,
                                const st_aot_compile_result_t *compiled)
{
    static const char domain[] = "anvil-smalltalk-artifact-bundle-v2\0";
    sha256_t hash;
    size_t index;
    sha256_init(&hash);
    /* Include exactly one separator NUL (the literal has an explicit one and
     * C adds its own terminator). */
    sha256_update(&hash, domain, sizeof(domain) - 1u);
    hash_u64(&hash, (uint64_t)bundle->target);
    hash_u64(&hash, (uint64_t)bundle->abi);
    hash_u64(&hash, (uint64_t)bundle->syntax);
    hash_u64(&hash, (uint64_t)bundle->optimization);
    hash_u64(&hash, (uint64_t)ST_IMAGE_METADATA_ABI_VERSION);
    hash_u64(&hash, (uint64_t)bundle->artifact_count);
    hash_u64(&hash, (uint64_t)bundle->block_count);
    for (index = 0u; index < bundle->artifact_count; index++) {
        const st_artifact_blob_t *artifact = &bundle->artifacts[index];
        hash_u64(&hash, (uint64_t)artifact->kind);
        hash_u64(&hash, (uint64_t)artifact->method_id);
        hash_u64(&hash, (uint64_t)artifact->name_length);
        sha256_update(&hash, artifact->name, artifact->name_length);
        hash_u64(&hash, (uint64_t)artifact->symbol_length);
        sha256_update(&hash, artifact->symbol, artifact->symbol_length);
        hash_u64(&hash, (uint64_t)artifact->size);
        sha256_update(&hash, artifact->sha256, sizeof(artifact->sha256));
    }
    for (index = 0u; index < compiled->method_count; index++) {
        const st_aot_method_result_t *method = &compiled->methods[index];
        size_t block_index;
        for (block_index = 0u; block_index < method->artifact.block_count;
             block_index++)
            hash_block_record(&hash, method->method_id,
                              &method->artifact.blocks[block_index]);
    }
    sha256_finish(&hash, bundle->bundle_sha256);
}

static st_artifact_bundle_status_t build_manifest(
    st_artifact_bundle_t *bundle, const st_aot_compile_result_t *compiled,
    st_artifact_allocator_t allocator)
{
    text_builder_t text;
    char bundle_hex[65];
    size_t index;
    bool launch_present = false;
    memset(&text, 0, sizeof(text));
    text.allocator = allocator;
    hash_hex(bundle->bundle_sha256, bundle_hex);
    for (index = 0u; index < bundle->artifact_count; index++) {
        if (bundle->artifacts[index].kind == ST_ARTIFACT_LAUNCH_ASSEMBLY) {
            launch_present = true;
        }
    }
    if (!text_appendf(&text,
            "anvil-smalltalk-artifact-bundle-v%" PRIu32 "\n"
            "target=%s\nabi=%s\nsyntax=%s\noptimization=%s\n"
            "metadata-abi=%" PRIu32 "\nlaunch=%s\n"
            "artifact-count=%zu\nblock-count=%zu\n"
            "bundle-sha256=%s\n",
            ST_ARTIFACT_BUNDLE_FORMAT_VERSION, target_name(bundle->target),
            abi_name(bundle->abi), syntax_name(bundle->syntax),
            optimization_name(bundle->optimization),
            ST_IMAGE_METADATA_ABI_VERSION,
            launch_present ? "present" : "absent",
            bundle->artifact_count,
            bundle->block_count,
            bundle_hex)) goto failure;
    for (index = 0u; index < bundle->artifact_count; index++) {
        const st_artifact_blob_t *artifact = &bundle->artifacts[index];
        char artifact_hex[65];
        hash_hex(artifact->sha256, artifact_hex);
        if (!text_appendf(&text,
                "artifact=%s|%08" PRIu32 "|%s|%s|%zu|%s\n",
                artifact->kind == ST_ARTIFACT_METHOD_ASSEMBLY
                    ? "method"
                    : artifact->kind == ST_ARTIFACT_METADATA_ASSEMBLY
                        ? "metadata" : "launch",
                artifact->method_id, artifact->name, artifact->symbol,
                artifact->size, artifact_hex)) goto failure;
    }
    for (index = 0u; index < compiled->method_count; index++) {
        const st_aot_method_result_t *method = &compiled->methods[index];
        size_t block_index;
        for (block_index = 0u; block_index < method->artifact.block_count;
             block_index++) {
            const st_image_aot_block_artifact_t *block =
                &method->artifact.blocks[block_index];
            if (!text_appendf(&text,
                    "block=%08" PRIu32 "|%08" PRIu32
                    "|%s|%s|%s|%" PRIu32 "|%" PRIu32 "|%" PRIu32
                    "|%" PRIu32 "|%zu|%zu\n",
                    method->method_id, block->lexical_ordinal,
                    block->code_symbol, block->descriptor_symbol,
                    block->method_descriptor_symbol, block->arity,
                    block->flags, block->method_flags,
                    block->frame_root_capacity, block->capture_count,
                    block->root_map_count)) goto failure;
        }
    }
    bundle->manifest = text.bytes;
    bundle->manifest_length = text.length;
    return ST_ARTIFACT_BUNDLE_OK;
failure:
    release(allocator, text.bytes);
    return text.overflow ? ST_ARTIFACT_BUNDLE_ERR_OVERFLOW
                         : ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
}

static st_artifact_bundle_status_t validate(
    const st_aot_compile_result_t *compiled,
    const st_artifact_bundle_options_t *options,
    anvil_arch_t *target_out, anvil_abi_t *abi_out,
    anvil_opt_level_t *optimization_out)
{
    const anvil_arch_info_t *arch;
    const st_aot_compile_provenance_t *provenance;
    anvil_arch_t target;
    size_t index, expected_blocks = 0u, expected_captures = 0u;
    size_t expected_parent_maps = 0u, expected_block_maps = 0u;
    if (compiled == NULL || options == NULL
            || ((options->allocator.allocate == NULL)
                != (options->allocator.deallocate == NULL))
            || ((options->launch_module == NULL)
                != (options->launch_symbol == NULL))
            || (options->launch_module == NULL
                && options->launch_symbol_length != 0u)
            || (options->launch_module != NULL
                && (!portable_symbol(options->launch_symbol,
                                     options->launch_symbol_length)
                    || anvil_module_lookup_symbol(options->launch_module,
                                                  options->launch_symbol)
                        == NULL)))
        return ST_ARTIFACT_BUNDLE_ERR_INVALID_ARGUMENT;
    provenance = &compiled->provenance;
    if (compiled->context == NULL || compiled->status != ST_AOT_COMPILE_OK
            || compiled->implementation == NULL
            || ((compiled->methods == NULL) != (compiled->method_count == 0u))
            || compiled->metadata.module == NULL
            || (compiled->method_count != 0u
                && !compiled->metadata.has_method_code)
            || compiled->metadata.method_count != compiled->method_count
            || provenance->symbol_prefix_length > ST_AOT_SYMBOL_PREFIX_MAX
            || !portable_symbol(provenance->symbol_prefix,
                                provenance->symbol_prefix_length))
        return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
    target = provenance->target;
    if (!target_supported(target)) return ST_ARTIFACT_BUNDLE_ERR_UNSUPPORTED_TARGET;
    if (!syntax_supported(target, provenance->syntax))
        return ST_ARTIFACT_BUNDLE_ERR_UNSUPPORTED_TARGET;
    arch = anvil_ctx_get_arch_info(compiled->context);
    if (!anvil_ctx_has_target(compiled->context) || arch == NULL
            || arch->ptr_size != 8
            || anvil_ctx_get_target(compiled->context) != target)
        return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
    *abi_out = anvil_ctx_get_abi(compiled->context);
    *optimization_out = anvil_ctx_get_opt_level(compiled->context);
    if ((unsigned)*abi_out > (unsigned)ANVIL_ABI_MVS
            || (unsigned)*optimization_out > (unsigned)ANVIL_OPT_AGGRESSIVE)
        return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
    if (!abi_supported(target, *abi_out))
        return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
    if (provenance->abi != *abi_out
            || provenance->optimization != *optimization_out)
        return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
    for (index = 0u; index < compiled->method_count; index++) {
        const st_aot_method_result_t *method = &compiled->methods[index];
        const st_lower_result_t *lowering = &method->lowering;
        const st_image_aot_method_artifact_t *artifact = &method->artifact;
        uint32_t expected_flags;
        size_t map_index, block_index, block_map_cursor = 0u;
        size_t block_capture_cursor = 0u;
        if (index >= UINT32_MAX || method->method_id != (uint32_t)(index + 1u)
                || method->owner == ST_CLASS_GRAPH_INVALID_ID
                || lowering->status != ST_LOWER_OK
                || lowering->module == NULL || lowering->function == NULL
                || method->selector == NULL || method->selector_length == 0u
                || method->selector[method->selector_length] != '\0'
                || !portable_symbol(method->symbol, method->symbol_length)
                || method->symbol_length
                    <= provenance->symbol_prefix_length + 2u
                || memcmp(method->symbol, provenance->symbol_prefix,
                          provenance->symbol_prefix_length) != 0
                || method->symbol[provenance->symbol_prefix_length] != '_'
                || method->symbol[provenance->symbol_prefix_length + 1u] != 'm'
                || ((method->root_maps == NULL)
                    != (lowering->root_map_count == 0u))
                || ((lowering->root_maps == NULL)
                    != (lowering->root_map_count == 0u))
                || artifact->method_id != method->method_id
                || artifact->owner != method->owner
                || artifact->arity != method->arity
                || artifact->selector != method->selector
                || artifact->selector_length != method->selector_length
                || artifact->symbol != method->symbol
                || artifact->symbol_length != method->symbol_length
                || artifact->frame_root_capacity
                    != lowering->required_root_capacity
                || artifact->root_maps != method->root_maps
                || artifact->root_map_count != lowering->root_map_count
                || artifact->blocks != method->block_artifacts
                || artifact->block_count != lowering->block_count
                || ((lowering->blocks == NULL)
                    != (lowering->block_count == 0u))
                || ((method->block_artifacts == NULL)
                    != (lowering->block_count == 0u)))
            return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
        if (lowering->root_map_count > SIZE_MAX - expected_parent_maps
                || lowering->block_count > SIZE_MAX - expected_blocks)
            return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
        expected_parent_maps += lowering->root_map_count;
        expected_blocks += lowering->block_count;
        expected_flags = lowering->method_flags
            | (lowering->has_primitive ? ST_METHOD_PRIMITIVE : 0u);
        if (artifact->flags != expected_flags
                || (expected_flags & ~(uint32_t)ST_METHOD_FLAGS_MASK) != 0u)
            return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
        for (map_index = 0u; map_index < lowering->root_map_count;
             map_index++) {
            const st_image_root_map_metadata_t *adapter =
                &method->root_maps[map_index];
            const st_lower_root_map_t *canonical =
                &lowering->root_maps[map_index];
            if (adapter->safepoint_id != canonical->safepoint_id
                    || adapter->root_count != canonical->root_count
                    || adapter->bitmap_word_count
                        != canonical->bitmap_word_count
                    || adapter->live_root_bitmap
                        != canonical->live_root_bitmap)
                return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
        }
        for (block_index = 0u; block_index < lowering->block_count;
             block_index++) {
            const st_lower_block_artifact_t *canonical =
                &lowering->blocks[block_index];
            const st_image_aot_block_artifact_t *adapter =
                &method->block_artifacts[block_index];
            bool saw_cell = false;
            if (!canonical->function
                    || adapter->lexical_ordinal != canonical->lexical_ordinal
                    || adapter->arity != canonical->arity
                    || adapter->flags != canonical->flags
                    || adapter->method_flags != canonical->method_flags
                    || adapter->frame_root_capacity
                        != canonical->required_root_capacity
                    || adapter->code_symbol != canonical->code_symbol.bytes
                    || adapter->code_symbol_length
                        != canonical->code_symbol.length
                    || adapter->descriptor_symbol
                        != canonical->descriptor_symbol.bytes
                    || adapter->descriptor_symbol_length
                        != canonical->descriptor_symbol.length
                    || adapter->method_descriptor_symbol
                        != canonical->method_descriptor_symbol.bytes
                    || adapter->method_descriptor_symbol_length
                        != canonical->method_descriptor_symbol.length
                    || adapter->capture_count != canonical->capture_count
                    || adapter->root_map_count != canonical->root_map_count
                    || (adapter->flags & ~(uint32_t)ST_AOT_BLOCK_FLAGS_MASK)
                        != 0u)
                return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
            if (canonical->capture_count != 0u
                    && (adapter->captures
                        != &method->block_captures[block_capture_cursor]
                        || memcmp(adapter->captures, canonical->captures,
                            canonical->capture_count
                                * sizeof(*canonical->captures)) != 0))
                return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
            if (!portable_symbol(adapter->code_symbol,
                                 adapter->code_symbol_length)
                    || !portable_symbol(adapter->descriptor_symbol,
                                        adapter->descriptor_symbol_length)
                    || !portable_symbol(adapter->method_descriptor_symbol,
                        adapter->method_descriptor_symbol_length)
                    || canonical->capture_count > SIZE_MAX - expected_captures
                    || canonical->root_map_count
                        > SIZE_MAX - expected_block_maps
                    || canonical->root_map_count
                        > SIZE_MAX - block_map_cursor)
                return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
            for (map_index = 0u; map_index < canonical->capture_count;
                 map_index++) {
                if (canonical->captures[map_index].kind
                        == ST_AOT_CAPTURE_CELL) {
                    saw_cell = true;
                }
            }
            if (saw_cell
                    != ((adapter->flags & ST_AOT_BLOCK_HAS_CELLS) != 0u)) {
                return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
            }
            expected_captures += canonical->capture_count;
            expected_block_maps += canonical->root_map_count;
            if (canonical->root_map_count != 0u
                    && adapter->root_maps
                        != &method->block_root_maps[block_map_cursor])
                return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
            for (map_index = 0u; map_index < canonical->root_map_count;
                 map_index++) {
                const st_lower_root_map_t *source =
                    &canonical->root_maps[map_index];
                const st_image_root_map_metadata_t *copy =
                    &adapter->root_maps[map_index];
                size_t bytes;
                if (source->bitmap_word_count > SIZE_MAX / sizeof(uint64_t))
                    return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
                bytes = source->bitmap_word_count * sizeof(uint64_t);
                if (copy->safepoint_id != source->safepoint_id
                        || copy->root_count != source->root_count
                        || copy->bitmap_word_count != source->bitmap_word_count
                        || copy->live_root_bitmap != source->live_root_bitmap
                        || (bytes != 0u && memcmp(copy->live_root_bitmap,
                            source->live_root_bitmap, bytes) != 0))
                    return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
            }
            block_map_cursor += canonical->root_map_count;
            block_capture_cursor += canonical->capture_count;
        }
        if (block_map_cursor != method->block_root_map_count
                || block_capture_cursor != method->block_capture_count)
            return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
    }
    if (expected_parent_maps > SIZE_MAX - expected_block_maps
            || compiled->metadata.block_count != expected_blocks
            || compiled->metadata.block_capture_count != expected_captures
            || compiled->metadata.block_root_map_count != expected_block_maps
            || compiled->metadata.root_map_count
                != expected_parent_maps + expected_block_maps)
        return ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT;
    *target_out = target;
    return ST_ARTIFACT_BUNDLE_OK;
}

static void builder_destroy(st_artifact_bundle_t *builder,
                            st_artifact_allocator_t allocator)
{
    size_t index;
    for (index = 0u; index < builder->artifact_count; index++) {
        release(allocator, builder->artifacts[index].bytes);
        release(allocator, builder->artifacts[index].symbol);
        release(allocator, builder->artifacts[index].name);
    }
    release(allocator, builder->artifacts);
    release(allocator, builder->manifest);
    builder->artifacts = NULL;
    builder->artifact_count = 0u;
    builder->manifest = NULL;
    builder->manifest_length = 0u;
}

st_artifact_bundle_status_t st_artifact_bundle_render(
    st_artifact_bundle_t *bundle,
    const st_aot_compile_result_t *compiled,
    const st_artifact_bundle_options_t *options)
{
    st_artifact_allocator_t allocator = {
        default_allocate, default_deallocate, NULL
    };
    st_artifact_bundle_t builder;
    bundle_impl_t *implementation = NULL;
    st_artifact_bundle_status_t status;
    anvil_arch_t target = ANVIL_ARCH_NONE;
    anvil_abi_t abi = ANVIL_ABI_DEFAULT;
    anvil_opt_level_t optimization = ANVIL_OPT_NONE;
    size_t count, index;
    char *metadata_symbol = NULL;
    size_t metadata_symbol_length;
    if (!result_is_empty(bundle)) {
        if (bundle != NULL)
            bundle->status = ST_ARTIFACT_BUNDLE_ERR_INVALID_ARGUMENT;
        return ST_ARTIFACT_BUNDLE_ERR_INVALID_ARGUMENT;
    }
    status = validate(compiled, options, &target, &abi, &optimization);
    if (status != ST_ARTIFACT_BUNDLE_OK) {
        bundle->status = status;
        return status;
    }
    if (options->allocator.allocate != NULL) allocator = options->allocator;
    if (!collision_free(compiled, allocator, &status)) {
        bundle->status = status;
        return status;
    }
    if (!add_size(compiled->method_count,
                  options->launch_module != NULL ? 2u : 1u, &count)
            || count > SIZE_MAX / sizeof(st_artifact_blob_t)) {
        bundle->status = ST_ARTIFACT_BUNDLE_ERR_OVERFLOW;
        return bundle->status;
    }
    st_artifact_bundle_init(&builder);
    builder.target = target;
    builder.abi = abi;
    builder.syntax = compiled->provenance.syntax;
    builder.optimization = optimization;
    builder.artifacts = allocate_array(allocator, count,
                                       sizeof(*builder.artifacts));
    if (builder.artifacts == NULL) {
        bundle->status = ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
        return bundle->status;
    }
    builder.artifact_count = count;
    for (index = 0u; index < compiled->method_count; index++) {
        st_artifact_blob_t *artifact = &builder.artifacts[index];
        const st_aot_method_result_t *method = &compiled->methods[index];
        artifact->kind = ST_ARTIFACT_METHOD_ASSEMBLY;
        artifact->method_id = method->method_id;
        status = render_blob(allocator, artifact, method->lowering.module,
                             method->symbol, method->symbol_length,
                             builder.syntax);
        if (status != ST_ARTIFACT_BUNDLE_OK) {
            if (status == ST_ARTIFACT_BUNDLE_ERR_CODEGEN) {
                builder.failed_method_id = method->method_id;
                builder.codegen_error = anvil_ctx_get_last_error(
                    compiled->context);
            }
            goto failure;
        }
    }
    if (!add_size(compiled->provenance.symbol_prefix_length,
                  sizeof("_descriptor") - 1u, &metadata_symbol_length)
            || !add_size(metadata_symbol_length, 1u, &count)) {
        status = ST_ARTIFACT_BUNDLE_ERR_OVERFLOW;
        goto failure;
    }
    metadata_symbol = allocator.allocate(allocator.user, count);
    if (metadata_symbol == NULL) {
        status = ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
        goto failure;
    }
    memcpy(metadata_symbol, compiled->provenance.symbol_prefix,
           compiled->provenance.symbol_prefix_length);
    memcpy(metadata_symbol + compiled->provenance.symbol_prefix_length,
           "_descriptor", sizeof("_descriptor"));
    builder.artifacts[compiled->method_count].kind =
        ST_ARTIFACT_METADATA_ASSEMBLY;
    status = render_blob(allocator, &builder.artifacts[compiled->method_count],
                         compiled->metadata.module, metadata_symbol,
                         metadata_symbol_length, builder.syntax);
    release(allocator, metadata_symbol);
    metadata_symbol = NULL;
    if (status != ST_ARTIFACT_BUNDLE_OK) {
        if (status == ST_ARTIFACT_BUNDLE_ERR_CODEGEN) {
            builder.failed_method_id = ST_CLASS_GRAPH_INVALID_ID;
            builder.codegen_error = anvil_ctx_get_last_error(compiled->context);
        }
        goto failure;
    }
    if (options->launch_module != NULL) {
        st_artifact_blob_t *launch =
            &builder.artifacts[compiled->method_count + 1u];
        launch->kind = ST_ARTIFACT_LAUNCH_ASSEMBLY;
        status = render_blob(
            allocator, launch, options->launch_module,
            options->launch_symbol, options->launch_symbol_length,
            builder.syntax);
        if (status != ST_ARTIFACT_BUNDLE_OK) {
            if (status == ST_ARTIFACT_BUNDLE_ERR_CODEGEN) {
                builder.failed_method_id = ST_CLASS_GRAPH_INVALID_ID;
                builder.codegen_error = anvil_ctx_get_last_error(
                    compiled->context);
            }
            goto failure;
        }
    }
    builder.block_count = compiled->metadata.block_count;
    compute_bundle_hash(&builder, compiled);
    status = build_manifest(&builder, compiled, allocator);
    if (status != ST_ARTIFACT_BUNDLE_OK) goto failure;
    implementation = allocator.allocate(allocator.user,
                                         sizeof(*implementation));
    if (implementation == NULL) {
        status = ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY;
        goto failure;
    }
    implementation->allocator = allocator;
    builder.status = ST_ARTIFACT_BUNDLE_OK;
    builder.implementation = implementation;
    *bundle = builder;
    return ST_ARTIFACT_BUNDLE_OK;
failure:
    release(allocator, metadata_symbol);
    bundle->failed_method_id = builder.failed_method_id;
    bundle->codegen_error = builder.codegen_error;
    builder_destroy(&builder, allocator);
    bundle->status = status;
    return status;
}

const char *st_artifact_bundle_status_string(
    st_artifact_bundle_status_t status)
{
    switch (status) {
    case ST_ARTIFACT_BUNDLE_OK: return "ok";
    case ST_ARTIFACT_BUNDLE_ERR_INVALID_ARGUMENT: return "invalid argument";
    case ST_ARTIFACT_BUNDLE_ERR_INVALID_COMPILE_RESULT:
        return "invalid AOT compile result";
    case ST_ARTIFACT_BUNDLE_ERR_UNSUPPORTED_TARGET:
        return "unsupported Smalltalk tagged64 target";
    case ST_ARTIFACT_BUNDLE_ERR_COLLISION: return "artifact name collision";
    case ST_ARTIFACT_BUNDLE_ERR_OUT_OF_MEMORY: return "out of memory";
    case ST_ARTIFACT_BUNDLE_ERR_OVERFLOW: return "size overflow";
    case ST_ARTIFACT_BUNDLE_ERR_CODEGEN: return "assembly code generation failed";
    }
    return "unknown artifact bundle status";
}
