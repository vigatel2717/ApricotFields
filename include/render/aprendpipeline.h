
#ifndef APREND_PIPELINE_H
#define APREND_PIPELINE_H

/**
 * @file aprendpipeline.h
 * @brief Shaders, graphics pipelines, and the binding layouts and sets that
 * give a pipeline's shaders their buffers, textures and samplers.
 */

#include "aprendbuffers.h"
#include "aprendimages.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_graphics_pipeline_layout_t aprend_graphics_pipeline_layout;




/**
 * @brief Opaque handle to one compiled shader stage.
 */
typedef struct aprend_shader_t *aprend_shader;

/**
 * @brief Creates a shader from a SPIR-V file.
 *
 * The file is read whole during the call and not kept open.
 *
 * @param[in] instance     Instance whose device the shader is created on.
 * @param[in] filename     Path of the SPIR-V file.
 * @param[in] shader_stage The one stage the code is for. Only
 *                         SPUDGPU_SHADER_STAGE_VERTEX and
 *                         SPUDGPU_SHADER_STAGE_FRAGMENT are accepted.
 *
 * @return The shader, or NULL if @p instance or @p filename is NULL,
 *         @p shader_stage is not accepted, the file doesn't exist or can't be
 *         read, or the shader module can't be created.
 *
 * @see aprend_shader_create_spirv()
 * @see aprend_shader_destroy()
 */
aprend_shader aprend_shader_read_from_file_spirv(
    aprend_instance instance,
    const char *filename,
    SPUDGPU_SHADER_STAGE shader_stage);
/**
 * @brief Creates a shader from SPIR-V already in memory, such as code embedded
 * in the caller's binary.
 *
 * The code is copied, so @p spirv_code only has to live for this call.
 *
 * @param[in] instance     Instance whose device the shader is created on.
 * @param[in] spirv_code   The SPIR-V words.
 * @param[in] spirv_size   Size of @p spirv_code in bytes. A multiple of 4, and
 *                         not 0.
 * @param[in] shader_stage The one stage the code is for. Only
 *                         SPUDGPU_SHADER_STAGE_VERTEX and
 *                         SPUDGPU_SHADER_STAGE_FRAGMENT are accepted.
 *
 * @return The shader, or NULL if @p instance or @p spirv_code is NULL,
 *         @p spirv_size is 0 or not a multiple of 4, @p shader_stage is not
 *         accepted, or the shader module can't be created.
 *
 * @see aprend_shader_destroy()
 */
aprend_shader aprend_shader_create_spirv(
    aprend_instance instance,
    const void *spirv_code,
    uint64_t spirv_size,
    SPUDGPU_SHADER_STAGE shader_stage);

/**
 * @brief Destroys a shader.
 *
 * @param[in] shader Shader to destroy. NULL is accepted and does nothing.
 */
void aprend_shader_destroy(aprend_shader shader);

/**
 * @brief Upper bound on the bindings one aprend_binding_layout can declare.
 */
#define APREND_MAX_BINDINGS_PER_LAYOUT SPUDGPU_MAX_DESCRIPTOR_BINDINGS_PER_SET

/**
 * @brief Upper bound on the set slots one pipeline can declare.
 */
#define APREND_MAX_BINDING_LAYOUTS SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS

/**
 * @brief One slot shaders read or write: `layout(set = N, binding = _binding)`
 * in GLSL.
 *
 * N is not stated here. It is the index a pipeline puts the layout at, in
 * aprend_graphics_pipeline_desc::_binding_layouts.
 */
typedef struct aprend_binding_desc {
	/** Slot index, `binding = ...` in GLSL. Unique within a layout. */
	uint32_t _binding;
	/** What the slot holds. */
	SPUDGPU_DESCRIPTOR_TYPE _type;
	/**
	 * 1 for a single resource, N for an array of N (`... name[N]` in GLSL).
	 * Never 0.
	 */
	uint32_t _count;
	/**
	 * Bitmask of the shader stages that use the slot. Never
	 * SPUDGPU_SHADER_STAGE_NONE.
	 * @see SPUDGPU_SHADER_STAGE
	 */
	SPUDGPU_SHADER_STAGE _stages;
} aprend_binding_desc;

/**
 * @brief Opaque handle to the slots of one descriptor set.
 *
 * Shared by every pipeline that declares it and every aprend_binding_set
 * created from it.
 */
typedef struct aprend_binding_layout_t *aprend_binding_layout;

/**
 * @brief Creates a binding layout.
 *
 * @p bindings is copied, so it only has to live for this call.
 *
 * @param[in] instance      Instance whose device the layout is created on.
 * @param[in] bindings      The slots, in any order.
 * @param[in] binding_count Number of elements in @p bindings. From 1 to
 *                          APREND_MAX_BINDINGS_PER_LAYOUT.
 *
 * @return The layout, or NULL if @p instance or @p bindings is NULL,
 *         @p binding_count is 0 or above APREND_MAX_BINDINGS_PER_LAYOUT, two
 *         entries share a `_binding`, an entry has a `_count` of 0, no
 *         `_stages`, or a `_type` that is not a SPUDGPU_DESCRIPTOR_TYPE, or
 *         the layout can't be created.
 *
 * @see aprend_binding_layout_destroy()
 * @see aprend_binding_set_create()
 */
aprend_binding_layout aprend_binding_layout_create(
    aprend_instance instance,
    const aprend_binding_desc *bindings,
    uint32_t binding_count);

/**
 * @brief Destroys a binding layout and the descriptor pools it owns.
 *
 * @warning Every pipeline declaring @p layout and every aprend_binding_set
 * created from it must be destroyed first.
 *
 * @param[in] layout Layout to destroy. NULL is accepted and does nothing.
 */
void aprend_binding_layout_destroy(aprend_binding_layout layout);

/**
 * @brief Configuration descriptor for a graphics pipeline.
 *
 * No field has a default. The shader entry points are not fields: both are
 * `main`.
 */
typedef struct aprend_graphics_pipeline_desc {
#ifdef _DEBUG
	/** A name for diagnostics. May be NULL. */
	const char *_debug_name;
#endif
	/**
	 * The vertex attributes, read from one vertex buffer, per vertex.
	 * Element N is `layout(location = N)` in the vertex shader. At most
	 * SPUDGPU_MAX_VERTEX_ATTRIBUTES elements; a count of 0 is a pipeline with
	 * no vertex input. The `elements` array is not copied: it must outlive
	 * the pipeline for aprend_graphics_pipeline_get_desc() to return it.
	 */
    aprend_buffer_layout _vertex_layout;
	/** What the vertices are assembled into. */
    SPUDGPU_PRIMITIVE_TOPOLOGY _topology;
	/**
	 * Which faces are discarded before shading: SPUDGPU_CULL_MODE_NONE,
	 * SPUDGPU_CULL_MODE_FRONT or SPUDGPU_CULL_MODE_BACK.
	 */
	SPUDGPU_CULL_MODE _cull_mode;
	/**
	 * Which winding is a front face, judged as the triangle lands in the
	 * target: true for counter-clockwise, false for clockwise. It decides
	 * what #_cull_mode discards and what the fragment shader sees as front
	 * facing. A projection that flips an axis reverses the winding of
	 * everything drawn through it; accounting for that is the caller's.
	 */
	bool _front_face_ccw;
	/** Whether fragments are tested against the depth target. */
	bool _depth_test;
	/** Whether fragments that pass write their depth. */
	bool _depth_write;
	/**
	 * Depth test comparison, required when #_depth_test is set:
	 * SPUDGPU_COMPARE_OP_LESS for standard depth, SPUDGPU_COMPARE_OP_GREATER
	 * for reverse-Z. SPUDGPU_COMPARE_OP_NEVER with #_depth_test set is
	 * refused.
	 */
	SPUDGPU_COMPARE_OP _depth_compare_op;
	/** Draws triangles as their edges instead of filled. */
	bool _wireframe;
	/** Not read yet: lines are as wide as the backend rasterizes them. */
	float _line_width;
	/**
	 * How this pipeline's output combines with what the color target
	 * already holds. Zeroed (`blend_enable` false) writes it as is. The
	 * presets fill it for the usual cases. A blended pipeline normally
	 * clears #_depth_write and keeps #_depth_test, and its draws are the
	 * caller's to order: back to front, or after everything opaque.
	 * @see aprend_blend_premultiplied()
	 * @see aprend_blend_alpha()
	 * @see aprend_blend_additive()
	 */
	spudgpu_blend_attachment_desc _blend;
	/** The vertex stage. Required. */
	aprend_shader vertex_shader;
	/** The fragment stage. Required. */
	aprend_shader fragment_shader;

	/** Pixel format of the color render target this pipeline writes to. */
	SPUDGPU_FORMAT color_attachment_format;
	/**
	 * Pixel format of the depth attachment. SPUDGPU_FORMAT_UNKNOWN for no
	 * depth.
	 */
	SPUDGPU_FORMAT depth_format;

	/**
	 * The descriptor sets the shaders use: `_binding_layouts[N]` describes
	 * `set = N`. The first #_binding_layout_count entries must all be set;
	 * the rest are ignored. Draws with this pipeline need an
	 * aprend_binding_set of the same layout at each of those slots first
	 * (APREND_COMMAND_SET_BINDING_SET). The layouts must outlive the
	 * pipeline.
	 */
	aprend_binding_layout _binding_layouts[APREND_MAX_BINDING_LAYOUTS];
	/** How many set slots are declared. At most APREND_MAX_BINDING_LAYOUTS. */
	uint32_t _binding_layout_count;
} aprend_graphics_pipeline_desc;

/**
 * @brief Opaque handle to a graphics pipeline.
 */
typedef struct aprend_graphics_pipeline_t *aprend_graphics_pipeline;

/**
 * @name Blend presets
 * For aprend_graphics_pipeline_desc::_blend.
 *
 * Premultiplied is the convention to prefer: the fragment shader outputs
 * color already multiplied by its alpha, which composites correctly over
 * anything and filters without dark fringes. Use aprend_blend_alpha() for
 * shaders that output straight (unmultiplied) color; it leaves the target's
 * alpha as coverage, so the target can itself be composited afterwards.
 * @{
 */

/**
 * @brief Premultiplied alpha blending.
 * @return `out = src + dst * (1 - src.a)`, color and alpha alike.
 */
spudgpu_blend_attachment_desc aprend_blend_premultiplied(void);

/**
 * @brief Straight (unmultiplied) alpha blending.
 * @return `out.rgb = src.rgb * src.a + dst.rgb * (1 - src.a)`;
 *         `out.a = src.a + dst.a * (1 - src.a)`.
 */
spudgpu_blend_attachment_desc aprend_blend_alpha(void);

/**
 * @brief Additive blending: light adding to light.
 * @return `out = src + dst`, color and alpha alike.
 */
spudgpu_blend_attachment_desc aprend_blend_additive(void);

/** @} */

/**
 * @brief Creates a graphics pipeline.
 *
 * @param[in] instance Instance whose device the pipeline is created on.
 * @param[in] desc     Pipeline configuration, taken by value.
 *
 * @return The pipeline, or NULL if @p instance is NULL, either shader is NULL,
 *         the vertex layout has more than SPUDGPU_MAX_VERTEX_ATTRIBUTES
 *         elements, `_cull_mode` is not a SPUDGPU_CULL_MODE, `_depth_test` is
 *         set with `_depth_compare_op` SPUDGPU_COMPARE_OP_NEVER,
 *         `_binding_layout_count` is above APREND_MAX_BINDING_LAYOUTS, a
 *         counted entry of `_binding_layouts` is NULL or belongs to another
 *         instance, or the pipeline can't be created.
 *
 * @see aprend_graphics_pipeline_destroy()
 */
aprend_graphics_pipeline aprend_graphics_pipeline_create(aprend_instance instance, aprend_graphics_pipeline_desc desc);

/**
 * @brief Destroys a graphics pipeline.
 *
 * Its shaders and binding layouts are the caller's and are not destroyed.
 *
 * @warning Every submission that draws with @p p must have finished.
 *
 * @param[in] p Pipeline to destroy. NULL is accepted and does nothing.
 */
void aprend_graphics_pipeline_destroy(aprend_graphics_pipeline p);

/**
 * @brief Reads the descriptor a pipeline was created with.
 *
 * @param[in] p Pipeline to read.
 *
 * @return The descriptor as it was passed to
 *         aprend_graphics_pipeline_create(), pointers included; zeroed if
 *         @p p is NULL.
 */
aprend_graphics_pipeline_desc aprend_graphics_pipeline_get_desc(aprend_graphics_pipeline p);

/**
 * @brief Opaque handle to the resources filling one aprend_binding_layout's
 * slots.
 *
 * Put at a set slot in a command list with APREND_COMMAND_SET_BINDING_SET.
 * Which resource sits in which slot is fixed at creation; the contents of the
 * buffers and textures can still change. Usable with any pipeline that
 * declares the layout it was created from, at any set slot where the pipeline
 * declares it.
 */
typedef struct aprend_binding_set_t *aprend_binding_set;

/**
 * @brief One resource for one slot of an aprend_binding_set.
 *
 * `_type` says which member of `_resource` is read:
 *
 * | `_type`                                        | Read                               |
 * |------------------------------------------------|------------------------------------|
 * | SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER         | `_uniform`                         |
 * | SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER         | `_storage`                         |
 * | SPUDGPU_DESCRIPTOR_TYPE_SAMPLED_IMAGE          | `_image._view`                     |
 * | SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE          | `_image._view`                     |
 * | SPUDGPU_DESCRIPTOR_TYPE_SAMPLER                | `_image._sampler`                  |
 * | SPUDGPU_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER | `_image._view`, `_image._sampler`  |
 *
 * A member of `_image` the type doesn't read is ignored.
 */
typedef struct aprend_binding_set_entry {
	/** The slot, as aprend_binding_desc::_binding. */
	uint32_t _binding;
	/** Index into the binding's array; 0 where its `_count` is 1. */
	uint32_t _array_element;
	/** Must be the `_type` the layout declares at #_binding. */
	SPUDGPU_DESCRIPTOR_TYPE _type;
	/** The resource. Only the member #_type names is read. */
	union {
		/** For SPUDGPU_DESCRIPTOR_TYPE_UNIFORM_BUFFER. */
		struct {
			/** The buffer. Required. */
			aprend_uniform_buffer _buffer;
			/** First byte of `_buffer` the slot sees. Inside the buffer. */
			uint64_t _offset;
			/**
			 * How many bytes the slot sees, from `_offset`. 0 is to the end
			 * of the buffer.
			 */
			uint64_t _range;
		} _uniform;
		/** For SPUDGPU_DESCRIPTOR_TYPE_STORAGE_BUFFER. */
		struct {
			/** The buffer. Required. */
			aprend_storage_buffer _buffer;
			/** First byte of `_buffer` the slot sees. Inside the buffer. */
			uint64_t _offset;
			/**
			 * How many bytes the slot sees, from `_offset`. 0 is to the end
			 * of the buffer.
			 */
			uint64_t _range;
		} _storage;
		/** For the image and sampler types. */
		struct {
			/**
			 * The texture view. Of type
			 * APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE for a sampled image or
			 * combined image sampler slot,
			 * APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS for a storage image
			 * slot.
			 */
			aprend_texture_view _view;
			/** The sampler. */
			aprend_sampler _sampler;
		} _image;
	} _resource;
} aprend_binding_set_entry;

/**
 * @brief Creates a binding set from a layout.
 *
 * @p entries is only read during the call. The resources it names must
 * outlive the set, and the set must outlive every submission that binds it.
 *
 * A layout's sets share descriptor pools the layout owns, and a destroyed
 * set's place is reused by the next one created.
 *
 * @note Creating and destroying sets of the same layout from two threads at
 * once is not safe. Sets of different layouts are independent.
 *
 * @param[in] layout      Layout whose slots the set fills.
 * @param[in] entries     One entry for every array element of every binding
 *                        of @p layout, each exactly once, and nothing else.
 * @param[in] entry_count Number of elements in @p entries.
 *
 * @return The set, or NULL if @p layout or @p entries is NULL, the entries
 *         don't fill the layout exactly, an entry's `_type` is not the
 *         layout's, a handle its `_type` reads is NULL, a view is of the wrong
 *         type for its slot, a buffer's `_offset` and `_range` run past its
 *         end, or the set can't be allocated.
 *
 * @see aprend_binding_set_destroy()
 */
aprend_binding_set aprend_binding_set_create(
    aprend_binding_layout layout,
    const aprend_binding_set_entry *entries,
    uint32_t entry_count);

/**
 * @brief Destroys a binding set.
 *
 * The resources it holds are the caller's and are not destroyed.
 *
 * @warning Every submission that binds @p set must have finished.
 *
 * @param[in] set Set to destroy. NULL is accepted and does nothing.
 */
void aprend_binding_set_destroy(aprend_binding_set set);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_PIPELINE_H
