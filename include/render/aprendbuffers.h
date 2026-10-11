
#ifndef APREND_BUFFERS_H
#define APREND_BUFFERS_H

#include "aprendcontext.h"
#include "stdint.h"

/****************************************************
 * Apricot Render Buffers
 *
 * Higher-level buffer management on top of SpudGPU buffers.
 * Provides dynamic resizing, memory hints, and uniform buffer
 * field layout management.
 *
 * Threads: the rule is in aprendcontext.h ("Threads"). A buffer is used by
 * one thread at a time with everything else of its instance. An update call
 * and a submission of a list that uses the buffer are never made from two
 * threads at once: the update writes what the submission reads. A buffer
 * set's update calls also read the instance's frame index. The size and
 * layout functions that take no buffer (aprend_uniform_type_get_size,
 * aprend_buffer_element_type_get_size, aprend_buffer_layout_get_element_index
 * and aprend_buffer_layout_get_total_size) are safe from any thread.
 ****************************************************/

#if __cplusplus
extern "C" {
#endif

/*
 * Rewrite this entire thing
 * Make something like :
 * aprend_static_mesh_buffer_t*
 * aprend_static_mesh_set_data()
 * aprend_dynamic_mesh_buffer_t*
 * aprend_dynamic_mesh_set_vertex_range(data_range, vertices)
 * aprend_dynamic_mesh_set_index_range(data_range, indices)
 */

typedef struct APREND_DEFAULT_VERTEX {
	float position[3];
	float normal[3];
	float uv[2];
	float color[4];
} APREND_DEFAULT_VERTEX;

typedef struct aprend_vertex_buffer_t *aprend_vertex_buffer;
typedef struct aprend_index_buffer_t *aprend_index_buffer;
typedef struct aprend_uniform_buffer_t *aprend_uniform_buffer;
typedef struct aprend_storage_buffer_t *aprend_storage_buffer;
typedef struct aprend_uniform_buffer_set_t *aprend_uniform_buffer_set;
typedef struct aprend_storage_buffer_set_t *aprend_storage_buffer_set;

typedef uint32_t APREND_UNIFORM_TYPE;
enum {
	APREND_UNIFORM_TYPE_BOOL  = 0,
	APREND_UNIFORM_TYPE_INT   = 1,
	APREND_UNIFORM_TYPE_INT2  = 2,
	APREND_UNIFORM_TYPE_INT3  = 3,
	APREND_UNIFORM_TYPE_INT4  = 4,
	APREND_UNIFORM_TYPE_UINT  = 5,
	APREND_UNIFORM_TYPE_UINT2 = 6,
	APREND_UNIFORM_TYPE_UINT3 = 7,
	APREND_UNIFORM_TYPE_UINT4 = 8,
	APREND_UNIFORM_TYPE_FLOAT = 9,
	APREND_UNIFORM_TYPE_VEC2  = 10,
	APREND_UNIFORM_TYPE_VEC3  = 11,
	APREND_UNIFORM_TYPE_VEC4  = 12,
	APREND_UNIFORM_TYPE_MAT4  = 13
};
uint32_t aprend_uniform_type_get_size(APREND_UNIFORM_TYPE type);

typedef struct aprend_uniform {
	const char *name;
	APREND_UNIFORM_TYPE type;
	uint32_t size; /* array element count, NOT a byte size; use 1 (or 0) for a non-array uniform */
	uint32_t offset;
} aprend_uniform;

typedef struct aprend_uniform_layout {
	uint32_t count;
	aprend_uniform *uniforms;
} aprend_uniform_layout;

/* A buffer for one std140 uniform block. Nothing is placed: each uniform's
 * [offset] is the caller's and must be where the shader's block has it. The
 * buffer ends where the furthest uniform does, rounded up to 16, with an
 * array (a [size] above 1) taking 16 bytes an element, or the type's size if
 * that is larger. The block starts as zeroes: a uniform reads 0 until it is
 * written. */
aprend_uniform_buffer aprend_uniform_buffer_create(
    aprend_instance instance,
    const aprend_uniform_layout *layout);
void aprend_uniform_buffer_destroy(aprend_uniform_buffer buffer);
spudgpu_buffer_view aprend_uniform_buffer_get_spudgpu_buffer_view(aprend_uniform_buffer buffer);
bool aprend_uniform_buffer_update(
    aprend_uniform_buffer buffer,
    uint32_t local_offset,
    uint32_t size,
    void *pData);
/* Writes the uniform named [name] (case-sensitive; the first one, if the
 * layout repeats a name) at the offset its layout entry gives. [pData] must
 * hold the whole uniform: the type's size times its element count, tightly
 * packed. An array's elements are written where std140 puts them, 16 bytes
 * apart or the type's size if that is larger. Names are copied at aprend_uniform_buffer_create, so the layout
 * passed there doesn't have to outlive the buffer.
 *
 * Returns false if an argument is NULL, no uniform has that name, the type is
 * unknown, or the uniform doesn't fit inside the buffer. */
bool aprend_uniform_buffer_update_by_name(
    aprend_uniform_buffer buffer,
    const char *name,
    void *pData);

typedef uint32_t APREND_BUFFER_ELEMENT_TYPE;
enum {
	APREND_BUFFER_ELEMENT_TYPE_FLOAT     = 0,
	APREND_BUFFER_ELEMENT_TYPE_VEC2      = 1,
	APREND_BUFFER_ELEMENT_TYPE_VEC3      = 2,
	APREND_BUFFER_ELEMENT_TYPE_VEC4      = 3,
	APREND_BUFFER_ELEMENT_TYPE_INT       = 4,
	APREND_BUFFER_ELEMENT_TYPE_INT2      = 5,
	APREND_BUFFER_ELEMENT_TYPE_INT3      = 6,
	APREND_BUFFER_ELEMENT_TYPE_INT4      = 7,
};
/* Size of one element of [type] in bytes; 0 for a value that isn't an
 * APREND_BUFFER_ELEMENT_TYPE. */
uint32_t aprend_buffer_element_type_get_size(APREND_BUFFER_ELEMENT_TYPE type);

typedef struct aprend_buffer_element {
	const char *name;
	APREND_BUFFER_ELEMENT_TYPE type;
	uint32_t size, offset;
} aprend_buffer_element;

typedef struct aprend_buffer_layout {
	uint32_t count;
	aprend_buffer_element *elements;
} aprend_buffer_layout;
/* Index of the first element of [layout] named [name] (case-sensitive), or
 * UINT32_MAX if there is none, or if [layout], its elements or [name] is NULL. */
uint32_t aprend_buffer_layout_get_element_index(
    const aprend_buffer_layout *layout,
    const char *name);
uint32_t aprend_buffer_layout_get_total_size(const aprend_buffer_layout *layout);

/* Where a vertex, index or storage buffer is kept follows the device's memory
 * (SPUDGPU_DEVICE_PROPERTIES::unified_memory and ::device_mappable_memory,
 * read when the instance is created), and the calls behave the same either
 * way.
 *
 * On a device whose memory is the system's, the buffer is mapped and a write
 * goes straight into it; a storage buffer needs the device to have
 * SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE for that. On a device with memory of
 * its own, or for a storage buffer without that kind, the buffer is in the
 * device's memory and a write goes into a staging copy Aprend keeps beside
 * it; the next aprend_command_list_submit of a list that uses the buffer
 * copies what was written across, ahead of the list's own commands.
 *
 * The rule for the caller is one rule: a write (at creation or by an update
 * call) is seen by any submission made after it, and must not be made while
 * submitted work that uses the buffer has yet to finish. A buffer written and
 * never used by a submitted list is never copied.
 *
 * A uniform buffer is mapped and written directly on every device.
 *
 * A vertex buffer of [vertex_count] vertices of [vertex_layout], holding a
 * copy of the data at [pData] or left unwritten if [pData] is NULL. NULL if
 * [instance] or [vertex_layout] is NULL, the layout has no elements or a
 * stride of 0, [vertex_count] is 0, or the device refuses the buffer. */
aprend_vertex_buffer aprend_vertex_buffer_create(
    aprend_instance instance,
    const aprend_buffer_layout *vertex_layout,
    uint32_t vertex_count,
    void *pData);
void aprend_vertex_buffer_destroy(aprend_vertex_buffer buffer);
spudgpu_buffer_view aprend_vertex_buffer_get_spudgpu_buffer_view(aprend_vertex_buffer buffer);
/* Writes [vertex_count] vertices from [pData], starting at vertex
 * [vertex_offset]. False, with nothing written, if [buffer] or [pData] is
 * NULL, [vertex_count] is 0, or the range runs past the end of the buffer. */
bool aprend_vertex_buffer_update(
    aprend_vertex_buffer buffer,
    uint32_t vertex_offset,
    uint32_t vertex_count,
    void *pData);
void aprend_vertex_buffer_get_layout(
    aprend_vertex_buffer buffer,
    aprend_buffer_layout *out_layout,
    uint32_t *out_vertex_count);

typedef enum APREND_INDEX_STRIDE { APREND_INDEX_STRIDE_NONE = 0, APREND_INDEX_STRIDE_UINT16 = 2, APREND_INDEX_STRIDE_UINT32 = 4 } APREND_INDEX_STRIDE;

/* An index buffer of [index_count] indices of [stride] bytes, holding a copy
 * of the data at [pData] or left unwritten if [pData] is NULL. NULL if
 * [instance] is NULL, [stride] is not UINT16 or UINT32, [index_count] is 0,
 * or the device refuses the buffer. */
aprend_index_buffer aprend_index_buffer_create(
    aprend_instance instance,
    APREND_INDEX_STRIDE stride,
    uint32_t index_count,
    void *pData);
void aprend_index_buffer_destroy(aprend_index_buffer buffer);
spudgpu_buffer_view aprend_index_buffer_get_spudgpu_buffer_view(aprend_index_buffer buffer);
/* Writes [index_count] indices from [pData], starting at index
 * [index_offset]. False, with nothing written, if [buffer] or [pData] is
 * NULL, [index_count] is 0, or the range runs past the end of the buffer. */
bool aprend_index_buffer_update(
    aprend_index_buffer buffer,
    uint32_t index_offset,
    uint32_t index_count,
    void *pData);
APREND_INDEX_STRIDE aprend_index_buffer_get_stride(aprend_index_buffer buffer);
uint32_t aprend_index_buffer_get_index_count(aprend_index_buffer buffer);

/* What a storage buffer is used for beyond a storage slot of a binding set,
 * which every storage buffer can be put in. */
typedef uint32_t APREND_STORAGE_BUFFER_USAGE;
enum {
	APREND_STORAGE_BUFFER_USAGE_NONE = 0,
	/* The buffer can also be the argument buffer of
	 * APREND_COMMAND_DRAW_INDIRECT and APREND_COMMAND_DRAW_INDEXED_INDIRECT
	 * (aprendcommands.h): the same buffer a compute shader fills through a
	 * storage slot is then read by the draws, with no trip through the CPU.
	 * It can equally be filled from the CPU with aprend_storage_buffer_update.
	 * A command list moves such a buffer between the two uses itself. */
	APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS = 1
};

/* A storage buffer of [size] bytes, holding a copy of the [size] bytes at
 * [pData], or left unwritten if [pData] is NULL. NULL if [instance] is NULL,
 * [size] is 0, [usage] has a bit that is not an APREND_STORAGE_BUFFER_USAGE,
 * or the device refuses the buffer. Where it is kept and when a write is
 * seen are as for a vertex buffer, above. */
aprend_storage_buffer aprend_storage_buffer_create(
    aprend_instance instance,
    uint64_t size,
    APREND_STORAGE_BUFFER_USAGE usage,
    void *pData);
/* The usage [buffer] was created with; APREND_STORAGE_BUFFER_USAGE_NONE if
 * it is NULL. */
APREND_STORAGE_BUFFER_USAGE aprend_storage_buffer_get_usage(aprend_storage_buffer buffer);
/* The buffer's size in bytes; 0 if [buffer] is NULL. */
uint64_t aprend_storage_buffer_get_size(aprend_storage_buffer buffer);
void aprend_storage_buffer_destroy(aprend_storage_buffer buffer);
/* Copies [size] bytes from [pData] to [local_offset] bytes into [buffer].
 * False, with nothing written, if [buffer] or [pData] is NULL, [size] is 0, or
 * [local_offset] + [size] runs past the end of the buffer. */
bool aprend_storage_buffer_update(
    aprend_storage_buffer buffer,
    uint64_t local_offset,
    uint64_t size,
    void *pData);

/****************************************************
 * Buffer sets: one buffer for each frame in flight
 *
 * A single uniform or storage buffer that is rewritten every frame can only
 * be written once the GPU has finished the last frame that reads it. A set
 * holds aprend_instance_desc::frames_in_flight copies instead. The update
 * calls write the copy of the current frame (aprend_instance_next_frame,
 * aprendcontext.h), and a binding set that holds the buffer set makes a
 * command list read the copy of the frame it is compiled in. So the frame
 * being written and the frames still on the GPU never share a copy.
 *
 * Each copy keeps what was last written to it, which was frames_in_flight
 * frames ago, not last frame: an update is not carried from one copy to the
 * next. Write, each frame, everything that frame reads.
 *
 * A set is put in a binding set through the _set member of an entry's
 * _uniform or _storage (aprendpipeline.h). With frames_in_flight 1 a set is
 * one buffer and behaves as a single buffer does.
 ****************************************************/

/* A set of uniform buffers of [layout], as aprend_uniform_buffer_create makes
 * each. NULL if [instance] or [layout] is NULL or a buffer can't be created. */
aprend_uniform_buffer_set aprend_uniform_buffer_set_create(
    aprend_instance instance,
    const aprend_uniform_layout *layout);
/* Destroys the set and its buffers. NULL is accepted. Every binding set
 * holding it goes first, and no later submission may read it; one already
 * made is safe (aprendcontext.h, "Destroying"). */
void aprend_uniform_buffer_set_destroy(aprend_uniform_buffer_set set);
/* The copy for frame [frame_index], owned by the set: not to be destroyed.
 * NULL if [set] is NULL or [frame_index] is not below frames_in_flight. */
aprend_uniform_buffer aprend_uniform_buffer_set_get_buffer(
    aprend_uniform_buffer_set set,
    uint32_t frame_index);
/* aprend_uniform_buffer_update on the current frame's copy. */
bool aprend_uniform_buffer_set_update(
    aprend_uniform_buffer_set set,
    uint32_t local_offset,
    uint32_t size,
    void *pData);
/* aprend_uniform_buffer_update_by_name on the current frame's copy. */
bool aprend_uniform_buffer_set_update_by_name(
    aprend_uniform_buffer_set set,
    const char *name,
    void *pData);

/* A set of storage buffers of [size] bytes, as aprend_storage_buffer_create
 * makes each. Every copy starts with the [size] bytes at [pData], or
 * unwritten if [pData] is NULL. NULL if [instance] is NULL, [size] is 0, or a
 * buffer can't be created. [usage] is every copy's. */
aprend_storage_buffer_set aprend_storage_buffer_set_create(
    aprend_instance instance,
    uint64_t size,
    APREND_STORAGE_BUFFER_USAGE usage,
    void *pData);
/* As aprend_uniform_buffer_set_destroy. */
void aprend_storage_buffer_set_destroy(aprend_storage_buffer_set set);
/* As aprend_uniform_buffer_set_get_buffer. */
aprend_storage_buffer aprend_storage_buffer_set_get_buffer(
    aprend_storage_buffer_set set,
    uint32_t frame_index);
/* aprend_storage_buffer_update on the current frame's copy. */
bool aprend_storage_buffer_set_update(
    aprend_storage_buffer_set set,
    uint64_t local_offset,
    uint64_t size,
    void *pData);

#if __cplusplus
}
#endif

#endif // APREND_BUFFERS_H
