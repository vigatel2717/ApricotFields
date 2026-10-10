
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
 * packed. Names are copied at aprend_uniform_buffer_create, so the layout
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

aprend_vertex_buffer aprend_vertex_buffer_create(
    aprend_instance instance,
    const aprend_buffer_layout *vertex_layout,
    uint32_t vertex_count,
    void *pData);
void aprend_vertex_buffer_destroy(aprend_vertex_buffer buffer);
spudgpu_buffer_view aprend_vertex_buffer_get_spudgpu_buffer_view(aprend_vertex_buffer buffer);
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

aprend_index_buffer aprend_index_buffer_create(
    aprend_instance instance,
    APREND_INDEX_STRIDE stride,
    uint32_t index_count,
    void *pData);
void aprend_index_buffer_destroy(aprend_index_buffer buffer);
spudgpu_buffer_view aprend_index_buffer_get_spudgpu_buffer_view(aprend_index_buffer buffer);
bool aprend_index_buffer_update(
    aprend_index_buffer buffer,
    uint32_t index_offset,
    uint32_t index_count,
    void *pData);
APREND_INDEX_STRIDE aprend_index_buffer_get_stride(aprend_index_buffer buffer);
uint32_t aprend_index_buffer_get_index_count(aprend_index_buffer buffer);

/* A storage buffer of [size] bytes, holding a copy of the [size] bytes at
 * [pData], or left unwritten if [pData] is NULL. NULL if [instance] is NULL,
 * [size] is 0, or the device refuses the buffer.
 *
 * The buffer is in host-visible memory and is written directly, so a write is
 * seen by anything the GPU has not finished running: the caller makes sure no
 * submitted work still reads the buffer before updating it. */
aprend_storage_buffer aprend_storage_buffer_create(
    aprend_instance instance,
    uint64_t size,
    void *pData);
void aprend_storage_buffer_destroy(aprend_storage_buffer buffer);
/* Copies [size] bytes from [pData] to [local_offset] bytes into [buffer].
 * False, with nothing written, if [buffer] or [pData] is NULL, [size] is 0, or
 * [local_offset] + [size] runs past the end of the buffer. */
bool aprend_storage_buffer_update(
    aprend_storage_buffer buffer,
    uint64_t local_offset,
    uint64_t size,
    void *pData);

#if __cplusplus
}
#endif

#endif // APREND_BUFFERS_H
