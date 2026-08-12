#ifndef ANVIL_SMALLTALK_RUNTIME_H
#define ANVIL_SMALLTALK_RUNTIME_H

#include "st_dispatch.h"
#include "st_value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST_METHOD_ABI_VERSION UINT32_C(2)

typedef st_value_t (*st_method_code_t)(StFrame *frame);

typedef enum {
    ST_RUNTIME_OK = 0,
    ST_RUNTIME_ERR_INVALID_ARGUMENT,
    ST_RUNTIME_ERR_INVALID_DESCRIPTOR,
    ST_RUNTIME_ERR_ID_OUT_OF_RANGE,
    ST_RUNTIME_ERR_OVERFLOW,
    ST_RUNTIME_ERR_OUT_OF_MEMORY,
    ST_RUNTIME_ERR_BAD_ALIGNMENT,
    ST_RUNTIME_ERR_BAD_EXTENT,
    ST_RUNTIME_ERR_BAD_OBJECT,
    ST_RUNTIME_ERR_IMMUTABLE,
    ST_RUNTIME_ERR_INCOMPATIBLE_SHAPE,
    ST_RUNTIME_ERR_CONFLICT,
    ST_RUNTIME_ERR_VISITOR_ABORTED
} st_runtime_status_t;

typedef enum {
    ST_INDEXED_NONE = 0,
    ST_INDEXED_VALUES,
    ST_INDEXED_UINT8,
    ST_INDEXED_UINT16,
    ST_INDEXED_UINT32,
    ST_INDEXED_UINT64
} st_indexed_format_t;

typedef struct {
    uint32_t safepoint_id;
    uint32_t root_count;
    size_t bitmap_word_count;
    const uint64_t *live_root_bitmap;
} st_root_map_t;

enum {
    ST_UNWIND_CATCH = UINT32_C(1),
    ST_UNWIND_ENSURE = UINT32_C(2),
    ST_UNWIND_NON_LOCAL_RETURN = UINT32_C(3)
};

typedef struct {
    uint32_t start_pc_offset;
    uint32_t end_pc_offset;
    uint32_t landing_pad_pc_offset;
    uint32_t kind;
    uint32_t catch_class_id;
} st_unwind_region_t;

enum {
    ST_METHOD_PRIMITIVE = UINT32_C(1) << 0,
    ST_METHOD_CAN_UNWIND = UINT32_C(1) << 1,
    ST_METHOD_HAS_NON_LOCAL_RETURN = UINT32_C(1) << 2,
    ST_METHOD_FLAGS_MASK = ST_METHOD_PRIMITIVE | ST_METHOD_CAN_UNWIND |
                           ST_METHOD_HAS_NON_LOCAL_RETURN
};

/* Immutable metadata emitted by the AOT compiler.  All pointer targets and
 * their storage must outlive every frame and MethodEntry which references the
 * descriptor. ABI v2 permits pre-link descriptors with code_size == 0 only
 * when no PC-relative unwind regions are present. Executable code remains in
 * StMethodBinding; cooperative NLR metadata does not require a PC region. */
struct StMethodDescriptor {
    uint32_t abi_version;
    uint32_t selector_id;
    uint32_t owner_class_id;
    uint32_t arity;
    uint32_t frame_root_capacity;
    uint32_t flags;
    uint32_t code_size;
    const char *source_name;
    size_t source_name_length;
    size_t source_start_offset;
    size_t source_end_offset;
    const st_root_map_t *root_maps;
    size_t root_map_count;
    const st_unwind_region_t *unwind_regions;
    size_t unwind_region_count;
};

typedef struct StMethodBinding {
    const StMethodDescriptor *descriptor;
    st_method_code_t code;
    uint64_t version;
} StMethodBinding;

/* A MethodEntry has a stable address. Publication changes only its pointer to
 * immutable data; it never creates, patches, or makes executable memory.  A
 * published binding must remain alive until the runtime has proved that no
 * reader can retain it (image-lifetime storage is the baseline policy). */
typedef struct StMethodEntry {
    uint32_t selector_id;
    uint32_t owner_class_id;
    _Atomic(const StMethodBinding *) binding;
} StMethodEntry;

typedef struct {
    uint32_t selector_id;
    StMethodEntry *entry;
} st_method_slot_t;

enum {
    ST_CLASS_METACLASS = UINT32_C(1) << 0,
    ST_CLASS_ABSTRACT = UINT32_C(1) << 1,
    ST_CLASS_FLAGS_MASK = ST_CLASS_METACLASS | ST_CLASS_ABSTRACT
};

typedef struct StClassDescriptor {
    uint32_t class_id;
    uint32_t superclass_id;
    uint32_t metaclass_id;
    uint32_t default_shape_id;
    uint32_t flags;
    const char *name;
    size_t name_length;
    const st_method_slot_t *method_slots;
    size_t method_slot_count;
} StClassDescriptor;

typedef struct StShapeDescriptor {
    uint32_t shape_id;
    uint32_t class_id;
    size_t allocation_alignment;
    size_t minimum_allocation_size;
    size_t fixed_word_count;
    st_indexed_format_t indexed_format;
    const uint64_t *fixed_pointer_bitmap;
    size_t fixed_pointer_bitmap_word_count;
} StShapeDescriptor;

/* Descriptor pointer arrays are dense: element zero describes ID one.  The
 * arrays and every descriptor they name are immutable image-lifetime data. */
typedef struct {
    const StClassDescriptor *const *classes;
    size_t class_count;
    const StShapeDescriptor *const *shapes;
    size_t shape_count;
} st_runtime_descriptors_t;

/* Every managed allocation begins with this prefix. `indexed_length` is
 * immutable after construction; capacity and total allocation extent live in
 * the allocation extent. */
typedef struct {
    st_object_header_t header;
    size_t indexed_length;
    size_t indexed_capacity;
    unsigned char payload[];
} st_heap_object_t;

typedef struct {
    void *base;
    size_t byte_size;
    size_t allocation_alignment;
} st_object_extent_t;

typedef struct {
    st_heap_object_t *object;
    const StClassDescriptor *class_descriptor;
    const StShapeDescriptor *shape_descriptor;
    size_t indexed_length;
    size_t indexed_capacity;
    void *fixed_words;
    void *indexed_elements;
} st_object_view_t;

typedef void *(*st_runtime_allocate_fn)(void *user, size_t alignment,
                                        size_t size);
typedef void (*st_runtime_deallocate_fn)(void *user, void *pointer,
                                         size_t alignment, size_t size);

typedef struct {
    st_runtime_allocate_fn allocate;
    st_runtime_deallocate_fn deallocate;
    void *user;
} st_runtime_allocator_t;

/* Only flags with complete semantics in the current non-moving allocation
 * layer may be requested by mutators. REMEMBERED is collector-owned;
 * FINALIZABLE and WEAK remain rejected until their queue/scan protocols are
 * implemented. */
#define ST_RUNTIME_ALLOCATION_FLAGS \
    ((st_header_flags_t)(ST_HEADER_IMMUTABLE | ST_HEADER_PINNED))

bool st_method_descriptor_is_valid(const StMethodDescriptor *descriptor);
bool st_method_binding_is_valid(const StMethodBinding *binding);
bool st_method_entry_init(StMethodEntry *entry,
                          const StMethodBinding *initial_binding);
const StMethodBinding *st_method_entry_load(const StMethodEntry *entry);
bool st_method_entry_publish(StMethodEntry *entry,
                             const StMethodBinding *new_binding,
                             const StMethodBinding **old_binding_out);
bool st_method_entry_compare_exchange(
    StMethodEntry *entry, const StMethodBinding **expected_in_out,
    const StMethodBinding *new_binding);

bool st_shape_descriptor_extent(const StShapeDescriptor *shape,
                                size_t indexed_capacity, size_t *extent_out);
bool st_shape_descriptor_is_valid(const StShapeDescriptor *shape);
bool st_class_descriptor_is_valid(const StClassDescriptor *descriptor);
st_runtime_status_t st_runtime_descriptors_validate(
    const st_runtime_descriptors_t *descriptors);
const StClassDescriptor *st_runtime_class(
    const st_runtime_descriptors_t *descriptors, uint32_t class_id);
const StShapeDescriptor *st_runtime_shape(
    const st_runtime_descriptors_t *descriptors, uint32_t shape_id);

st_runtime_status_t st_object_allocate(
    const st_runtime_descriptors_t *descriptors, uint32_t class_id,
    uint32_t shape_id, size_t indexed_length, size_t indexed_capacity,
    st_header_flags_t flags, st_runtime_allocator_t allocator,
    st_object_extent_t *extent_out, st_value_t *value_out);
void st_object_deallocate(st_runtime_allocator_t allocator,
                          st_object_extent_t extent);

/* Extent validation is the authority to dereference an encoded heap value.
 * It checks allocation base/alignment/size, header IDs, shape ownership,
 * indexed length and descriptor layout before returning borrowed pointers. */
st_runtime_status_t st_object_validate(
    const st_runtime_descriptors_t *descriptors, st_value_t value,
    st_object_extent_t extent, st_object_view_t *view_out);

typedef bool (*st_reference_visitor_fn)(void *user, st_value_t *slot);
st_runtime_status_t st_object_visit_references(
    const st_runtime_descriptors_t *descriptors, st_value_t value,
    st_object_extent_t extent, st_reference_visitor_fn visitor, void *user,
    size_t *visited_out);

/* A transition is permitted only within the same class and physical layout,
 * including extent and indexed capacity. Immutable instances reject it. */
st_runtime_status_t st_object_transition_shape(
    const st_runtime_descriptors_t *descriptors, st_value_t value,
    st_object_extent_t extent, uint32_t expected_shape_id,
    uint32_t target_shape_id);

const char *st_runtime_status_string(st_runtime_status_t status);

#ifdef __cplusplus
}
#endif

#endif
