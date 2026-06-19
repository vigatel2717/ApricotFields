
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
extern "C"
{
#endif

typedef struct aprend_texture_desc
{
	SPUDGPU_FORMAT format;
	uint32_t width;
	uint32_t height;
    SPUDGPU_IMAGE_TYPE type;
	SPUDGPU_IMAGE_USAGE usage;
    SPUDGPU_MEMORY_FLAGS memory_flags;
    //TextureWrap sampler_wrap = TextureWrap::Repeat;
	//TextureFilter sampler_filter = TextureFilter::Linear;
	
	bool generate_mips;
	bool storage;
	bool store_locally;
} aprend_texture_desc;

typedef struct aprend_texture2d_t *aprend_texture2d;
aprend_texture2d aprend_texture2d_create(
    aprend_instance instance,
    const aprend_texture_desc *desc);
void aprend_texture2d_destroy(
    aprend_texture2d texture);
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

#if __cplusplus
}
#endif

#endif // APREND_RENDER_IMAGES_H