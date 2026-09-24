
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
/* From SPIR-V already in memory (e.g. embedded in the caller's binary).
 * [spirv_size] is in bytes and must be a multiple of 4; the code is copied,
 * so it only has to live for this call. */
aprend_shader aprend_shader_create_spirv(
    aprend_instance instance,
    const void *spirv_code,
    uint64_t spirv_size,
    SPUDGPU_SHADER_STAGE shader_stage);
void aprend_shader_destroy(aprend_shader shader);

/* Upper bound on uniform buffer slots one pipeline can declare. */
#define APREND_MAX_UNIFORM_BINDINGS SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET

/* One uniform buffer slot a pipeline's shaders read: `layout(set = 0,
 * binding = _binding) uniform ...` in GLSL. */
typedef struct aprend_uniform_binding_desc {
	uint32_t _binding;
	/* Bitmask of the shader stages that read it (SPUDGPU_SHADER_STAGE_*). */
	SPUDGPU_SHADER_STAGE _stages;
} aprend_uniform_binding_desc;

typedef struct aprend_graphics_pipeline_desc {
#ifdef _DEBUG
	const char *_debug_name;
#endif
    aprend_buffer_layout _vertex_layout;
    SPUDGPU_PRIMITIVE_TOPOLOGY _topology;
	bool _backface_culling;
	bool _depth_test;
	bool _depth_write;
	/// Depth test comparison, required when _depth_test is set (e.g.
	/// SPUDGPU_COMPARE_OP_LESS for standard depth, _GREATER for reverse-Z).
	SPUDGPU_COMPARE_OP _depth_compare_op;
	bool _wireframe;
	float _line_width;
	aprend_shader vertex_shader;
	aprend_shader fragment_shader;

	/// Pixel format of the color render target this pipeline will write to.
	SPUDGPU_FORMAT color_attachment_format;
	/// Pixel format of the depth attachment. Set to SPUDGPU_FORMAT_UNKNOWN for no depth.
	SPUDGPU_FORMAT depth_format;

	/// Uniform buffer slots the shaders read (all in descriptor set 0). Draws
	/// with this pipeline need an aprend_uniform_set filling them bound first
	/// (APREND_COMMAND_SET_UNIFORM_SET). Bindings must be unique.
	aprend_uniform_binding_desc _uniform_bindings[APREND_MAX_UNIFORM_BINDINGS];
	uint32_t _uniform_binding_count;
} aprend_graphics_pipeline_desc;

typedef struct aprend_graphics_pipeline_t *aprend_graphics_pipeline;

aprend_graphics_pipeline aprend_graphics_pipeline_create(aprend_instance instance, aprend_graphics_pipeline_desc desc);
/* Every aprend_uniform_set created for [p] must be destroyed first. */
void aprend_graphics_pipeline_destroy(aprend_graphics_pipeline p);
aprend_graphics_pipeline_desc aprend_graphics_pipeline_get_desc(aprend_graphics_pipeline p);

/* The uniform buffers bound to one pipeline's uniform slots, bound in a
 * command list with APREND_COMMAND_SET_UNIFORM_SET. Which buffer sits in which
 * slot is fixed at creation; the buffers' contents can still change freely
 * with aprend_uniform_buffer_update. Only valid with the pipeline it was
 * created for. */
typedef struct aprend_uniform_set_t *aprend_uniform_set;

typedef struct aprend_uniform_set_entry {
	uint32_t _binding;
	aprend_uniform_buffer _buffer;
} aprend_uniform_set_entry;

/* [entries] must fill every one of [pipeline]'s uniform bindings exactly once
 * (and nothing else); returns NULL otherwise. The buffers must outlive the
 * set, and the set must outlive every submission that binds it. */
aprend_uniform_set aprend_uniform_set_create(
    aprend_graphics_pipeline pipeline,
    const aprend_uniform_set_entry *entries,
    uint32_t entry_count);
void aprend_uniform_set_destroy(aprend_uniform_set set);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_PIPELINE_H
