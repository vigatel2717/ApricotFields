
#ifndef APREND_BUFFERS_H
#define APREND_BUFFERS_H

#include "aprendbase.h"
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

typedef struct aprend_buffer_context_t *aprend_buffer_context;
typedef struct aprend_vertex_buffer_t *aprend_vertex_buffer;
typedef struct aprend_index_buffer_t *aprend_index_buffer;
typedef struct aprend_uniform_buffer_t *aprend_uniform_buffer;
typedef struct aprend_storage_buffer_t *aprend_storage_buffer;
typedef struct aprend_shader_t *aprend_shader;
typedef struct aprend_shader_program_t *aprend_shader_program;

typedef uint32_t APREND_BUFFER_CONTEXT_FLAGS;
enum {
	APREND_BUFFER_CONTEXT_FLAG_DYNAMIC = 1, /* buffer memory is host-visible and coherent; updated frequently from the CPU */
	APREND_BUFFER_CONTEXT_FLAG_STATIC  = 2  /* buffer memory is device-local; updated rarely or never from the CPU */
};
aprend_buffer_context aprend_buffer_context_create(
    aprend_instance instance,
    uint64_t initial_capacity,
    SPUDGPU_BUFFER_USAGE usage,
    SPUDGPU_MEMORY_FLAGS flags);
uint64_t aprend_buffer_context_get_current_capacity(aprend_buffer_context context);
uint64_t aprend_buffer_context_shrink_to_fit(aprend_buffer_context context);
void aprend_buffer_context_destroy(aprend_buffer_context context);

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
	uint32_t size, offset;
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
aprend_buffer_context aprend_uniform_buffer_get_buffer_context(aprend_uniform_buffer buffer);
bool aprend_uniform_buffer_update(
    aprend_uniform_buffer buffer,
    uint32_t local_offset,
    uint32_t size,
    void *pData);
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
	APREND_BUFFER_ELEMENT_TYPE_UNIQUE_ID = 8 /* special type for picking; treated as uint32 behind the scenes, but semantically distinct from a regular uint */
};
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
uint32_t aprend_index_buffer_get_stride(aprend_index_buffer buffer);

aprend_storage_buffer aprend_storage_buffer_create(
    aprend_instance instance,
    uint64_t size,
    void *pData);
void aprend_storage_buffer_destroy(aprend_storage_buffer buffer);
bool aprend_storage_buffer_update(
    aprend_storage_buffer buffer,
    uint64_t local_offset,
    uint64_t size,
    void *pData);

#if __cplusplus
}
#endif

#endif // APREND_BUFFERS_H
