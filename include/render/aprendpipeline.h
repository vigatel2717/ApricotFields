
#ifndef APREND_PIPELINE_H
#define APREND_PIPELINE_H

#include "aprendbuffers.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_graphics_pipeline_layout_t aprend_graphics_pipeline_layout;




typedef struct aprend_shader_t *aprend_shader;

aprend_shader aprend_shader_read_from_file_spirv(
    aprend_instance instance,
    const char *filename,
    SPUDGPU_SHADER_STAGE shader_stage);
void aprend_shader_destroy(aprend_shader shader);

typedef struct aprend_graphics_pipeline_desc {
#ifdef _DEBUG
	const char *_debug_name;
#endif
    aprend_buffer_layout _vertex_layout;
    SPUDGPU_PRIMITIVE_TOPOLOGY _topology;
	bool _backface_culling;
	bool _depth_test;
	bool _depth_write;
	bool _wireframe;
	float _line_width;
	aprend_shader vertex_shader;
	aprend_shader fragment_shader;

	/// Pixel format of the color render target this pipeline will write to.
	SPUDGPU_FORMAT color_attachment_format;
	/// Pixel format of the depth attachment. Set to SPUDGPU_FORMAT_UNKNOWN for no depth.
	SPUDGPU_FORMAT depth_format;
} aprend_graphics_pipeline_desc;

typedef struct aprend_graphics_pipeline_t *aprend_graphics_pipeline;

aprend_graphics_pipeline aprend_graphics_pipeline_create(aprend_instance instance, aprend_graphics_pipeline_desc desc);
void aprend_graphics_pipeline_destroy(aprend_graphics_pipeline p);
aprend_graphics_pipeline_desc aprend_graphics_pipeline_get_desc(aprend_graphics_pipeline p);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_PIPELINE_H
