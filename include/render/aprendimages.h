
#ifndef APREND_RENDER_IMAGES_H
#define APREND_RENDER_IMAGES_H

#include "aprendbase.h"

/****************************************************
 * Apricot Render Images
 *
 * Higher-level image management on top of SpudGPU images.
 * Provides dynamic resizing, memory hints, and image view
 * management.
 ****************************************************/

#if __cplusplus
extern "C" {
#endif

typedef uint32_t APREND_TEXTURE_USAGE_BITS;
enum {
	APREND_TEXTURE_USAGE_BIT_NONE            = 0,
	APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE = 1,
	APREND_TEXTURE_USAGE_BIT_RENDER_TARGET   = 2,
	APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL   = 4,
	APREND_TEXTURE_USAGE_BIT_STORAGE         = 8
};
typedef struct aprend_texture2d_desc {
	uint32_t width;
	uint32_t height;
	SPUDGPU_FORMAT format;

	APREND_TEXTURE_USAGE_BITS usage; // bitmask: SHADER_RESOURCE | RENDER_TARGET | DEPTH_STENCIL | STORAGE

	uint32_t mip_levels;   // 0 = generate full chain, 1 = no mips, N = explicit levels
	uint32_t array_layers; // 1 = plain 2D texture, >1 = texture2d array
	uint32_t sample_count; // 1 = no MSAA

	SPUDGPU_MEMORY_FLAGS memory_flags;
	bool store_locally; // keep a CPU-side copy for aprend_texture2d_get_data

	const void *initial_data;    // optional; upload at creation instead of a separate _update call
	uint32_t initial_data_pitch; // row pitch in bytes, required if initial_data is set

#if _DEBUG
	const char *debug_name; // optional; forwarded to SpudGPU for RenderDoc/PIX labeling
#endif
} aprend_texture2d_desc;

typedef struct aprend_texture2d_t *aprend_texture2d;
aprend_texture2d aprend_texture2d_create(
    aprend_instance instance,
    const aprend_texture2d_desc *desc);
void aprend_texture2d_destroy(aprend_texture2d texture);
bool aprend_texture2d_get_desc(
    aprend_texture2d texture,
    aprend_texture2d_desc *out_desc);
spudgpu_image_view aprend_texture2d_get_spudgpu_image_view(aprend_texture2d texture);
spudgpu_image aprend_texture2d_get_spudgpu_image(aprend_texture2d texture);
bool aprend_texture2d_update(
    aprend_texture2d texture,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t width,
    uint32_t height,
    void **ppData);
bool aprend_texture2d_get_data(
    aprend_texture2d texture,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t width,
    uint32_t height,
    void **ppData);
bool aprend_texture2d_resize(
    aprend_texture2d texture,
    uint32_t new_width,
    uint32_t new_height);

typedef struct aprend_texture3d_desc {
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	SPUDGPU_FORMAT format;

	APREND_TEXTURE_USAGE_BITS usage; // typically SHADER_RESOURCE | STORAGE; DEPTH_STENCIL is not valid for a 3D texture

	uint32_t mip_levels; // 0 = generate full chain, 1 = no mips, N = explicit levels
	// no array_layers or sample_count: 3D textures support neither array layers nor MSAA

	SPUDGPU_MEMORY_FLAGS memory_flags;
	bool store_locally; // keep a CPU-side copy for aprend_texture3d_get_data

	const void *initial_data;          // optional; uploaded at creation instead of a separate aprend_texture3d_update call
	uint32_t initial_data_row_pitch;   // bytes between rows, required if initial_data is set
	uint32_t initial_data_slice_pitch; // bytes between depth slices, required if initial_data is set

#if _DEBUG
	const char *debug_name; // optional; forwarded to SpudGPU for RenderDoc/PIX labeling
#endif
} aprend_texture3d_desc;

typedef struct aprend_texture3d_t *aprend_texture3d;
aprend_texture3d aprend_texture3d_create(
    aprend_instance instance,
    const aprend_texture3d_desc *desc);
void aprend_texture3d_destroy(aprend_texture3d texture);
bool aprend_texture3d_get_desc(
    aprend_texture3d texture,
    aprend_texture3d_desc *out_desc);
spudgpu_image_view aprend_texture3d_get_spudgpu_image_view(aprend_texture3d texture);
spudgpu_image aprend_texture3d_get_spudgpu_image(aprend_texture3d texture);
bool aprend_texture3d_update(
    aprend_texture3d texture,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t z_offset,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    void **ppData);
bool aprend_texture3d_get_data(
    aprend_texture3d texture,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t z_offset,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    void **ppData);
bool aprend_texture3d_resize(
    aprend_texture3d texture,
    uint32_t new_width,
    uint32_t new_height,
    uint32_t new_depth);

#if __cplusplus
}
#endif

#endif // APREND_RENDER_IMAGES_H