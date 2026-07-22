
#ifndef APRENDFRAMES_H
#define APRENDFRAMES_H

#include "aprendimages.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_framebuffer_texture_spec {
	// Texture format.
	SPUDGPU_FORMAT format;
	// True if used for shaders, false otherwise.
	bool shader_resource;
	// True if used for GUI elements, false otherwise.
	bool use_for_gui;
} aprend_framebuffer_texture_spec;

typedef struct aprend_framebuffer_desc {
	// Width of the frame buffer.
	uint32_t width;
	// Height of the frame buffer.
	uint32_t height;
	// Color attachments.
	const aprend_framebuffer_texture_spec *attachments;
	uint32_t attachment_count;
	// Depth/stencil attachment format. Set to SPUDGPU_FORMAT_UNKNOWN for no depth attachment.
	SPUDGPU_FORMAT depth_format;
	// Sample count.
	uint32_t sample_count;
	// True if used for swap chain, false otherwise.
	bool swap_chain_target;
} aprend_framebuffer_desc;

typedef struct aprend_framebuffer_t *aprend_framebuffer;
aprend_framebuffer aprend_framebuffer_create(
    aprend_instance instance,
    const aprend_framebuffer_desc *desc);
void aprend_framebuffer_destroy(aprend_framebuffer framebuffer);
bool aprend_framebuffer_get_desc(
    aprend_framebuffer framebuffer,
    aprend_framebuffer_desc *out_desc);
bool aprend_framebuffer_resize(
    aprend_framebuffer framebuffer,
    uint32_t width,
    uint32_t height);
bool aprend_framebuffer_read_pixel(
    aprend_framebuffer framebuffer,
    uint32_t attachment_index,
    uint32_t x,
    uint32_t y,
    void *out_data,
    uint64_t data_size);
bool aprend_framebuffer_clear_colors(
    aprend_framebuffer framebuffer,
    float r,
    float g,
    float b,
    float a);
bool aprend_framebuffer_clear_depth(
    aprend_framebuffer framebuffer,
    bool clear_depth,
    bool clear_stencil,
    float depth,
    uint32_t stencil);
aprend_texture2d aprend_framebuffer_get_color_attachment_texture(
    aprend_framebuffer framebuffer,
    uint32_t index);
aprend_texture2d aprend_framebuffer_get_depth_attachment_texture(aprend_framebuffer framebuffer);

#if __cplusplus
}
#endif // __cplusplus

#endif // APRENDFRAMES_H
