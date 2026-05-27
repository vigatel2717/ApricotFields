#ifndef APRICOTFIELDS_H
#define APRICOTFIELDS_H

#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

typedef struct apricot_universe_t *apricot_universe;
typedef struct apricot_vertex_buffer_t *apricot_vertex_buffer;
typedef struct apricot_index_buffer_t *apricot_index_buffer;
typedef struct apricot_uniform_buffer_t *apricot_uniform_buffer;
typedef struct apricot_shader_t *apricot_shader;
typedef struct apricot_shader_program_t *apricot_shader_program;
typedef struct apricot_mesh_t *apricot_mesh;
typedef struct apricot_camera_t *apricot_camera;

apricot_universe apricot_create_universe();

typedef struct apricot_vertex_layout {
	uint32_t _stride;
} apricot_vertex_layout;
apricot_vertex_buffer apricot_create_vertex_buffer(apricot_vertex_layout layout, uint32_t vertexCount, void *pData);
void apricot_destroy_vertex_buffer(apricot_vertex_buffer buffer);
void apricot_update_vertex_buffer(apricot_vertex_buffer buffer, void *pData);

typedef uint16_t APRICOT_INDEX_DATA_TYPE;
enum {
	APRICOT_INDEX_DATA_TYPE_UINT16 = 0,
	APRICOT_INDEX_DATA_TYPE_UINT32 = 1,
	APRICOT_INDEX_DATA_TYPE_UINT64 = 2,
};
apricot_index_buffer apricot_create_index_buffer(uint16_t indexType, uint32_t indexCount, void *pData);
void apricot_destroy_index_buffer(apricot_index_buffer buffer);
void apricot_update_index_buffer(apricot_index_buffer buffer, void *pData);


apricot_uniform_buffer apricot_create_uniform_buffer(uint32_t bufferSize, void *pData);
void apricot_destroy_uniform_buffer(apricot_uniform_buffer buffer);
void apricot_update_uniform_buffer(apricot_uniform_buffer buffer, void *pData);

apricot_mesh apricot_create_mesh();




void hello();

#if __cplusplus
}
#endif

#endif // APRICOTFIELDS_H
