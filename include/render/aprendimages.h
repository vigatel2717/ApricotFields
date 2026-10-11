
#ifndef APREND_RENDER_IMAGES_H
#define APREND_RENDER_IMAGES_H

#include "aprendcontext.h"

/**
 * @file aprendimages.h
 * @brief Textures, the views a pass or a binding set uses them through, and
 * samplers, over SpudGPU images.
 *
 * @par Threads
 * The rule is in aprendcontext.h ("Threads"). A texture, a view and a sampler
 * are each used by one thread at a time with everything else of their
 * instance. A texture's update, readback and resize calls submit work and
 * write the image layout Aprend tracks for it, which
 * aprend_command_list_compile() reads and aprend_command_list_submit()
 * writes, so none of them runs while another thread compiles or submits a
 * list that uses the texture. A readback blocks its thread.
 */

#if __cplusplus
extern "C" {
#endif

/**
 * @brief Bitmask of what a texture is used for.
 *
 * Passed through to SpudGPU as the image's usage. Every texture can also be
 * copied to and from, whatever is set here.
 */
typedef uint32_t APREND_TEXTURE_USAGE_BITS;
enum {
	APREND_TEXTURE_USAGE_BIT_NONE            = 0,
	/** Sampled by a shader. */
	APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE = 1,
	/** A color target of a pass. */
	APREND_TEXTURE_USAGE_BIT_RENDER_TARGET   = 2,
	/** The depth target of a pass. */
	APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL   = 4,
	/** Read and written by a shader as a storage image. */
	APREND_TEXTURE_USAGE_BIT_STORAGE         = 8
};

/**
 * @brief Configuration descriptor for a 2D texture or 2D texture array.
 */
typedef struct aprend_texture2d_desc {
	/** Width of mip 0 in texels. Never 0. */
	uint32_t width;
	/** Height of mip 0 in texels. Never 0. */
	uint32_t height;
	/** Texel format. Never SPUDGPU_FORMAT_UNKNOWN. */
	SPUDGPU_FORMAT format;

	/** @see APREND_TEXTURE_USAGE_BITS */
	APREND_TEXTURE_USAGE_BITS usage;

	/**
	 * 0 for a full chain, 1 for no mips, N for N levels. Every level starts
	 * undefined and nothing generates one from another: each level a
	 * sampler can reach is the caller's to write.
	 */
	uint32_t mip_levels;
	/** 1 for a plain 2D texture, more for an array. 0 is taken as 1. */
	uint32_t array_layers;
	/** 1 for no MSAA. More than 1 is refused: MSAA is not supported yet. */
	uint32_t sample_count;

#if _DEBUG
	/** A name for diagnostics. May be NULL. */
	const char *debug_name;
#endif
} aprend_texture2d_desc;

/**
 * @brief Opaque handle to a 2D texture or 2D texture array.
 */
typedef struct aprend_texture2d_t *aprend_texture2d;

/**
 * @brief Creates a 2D texture. Its contents are undefined until written:
 * there is no initial data in the descriptor, so fill it with
 * aprend_texture2d_update().
 *
 * @param[in] instance Instance whose device the texture is created on.
 * @param[in] desc     Texture configuration. Only read during the call.
 *
 * @return The texture, or NULL if @p instance or @p desc is NULL, `width`,
 *         `height` or `format` is 0, `sample_count` is above 1, or the image
 *         can't be created.
 *
 * @see aprend_texture2d_destroy()
 */
aprend_texture2d aprend_texture2d_create(
	aprend_instance instance,
	const aprend_texture2d_desc *desc);

/**
 * @brief Destroys a 2D texture.
 *
 * @warning Every aprend_texture_view made from @p texture must be destroyed
 * first, and no later submission may use it. A submission
 * already made that uses it is safe: the GPU's side of it is released when
 * that work has finished (aprendcontext.h, "Destroying").
 *
 * @param[in] texture Texture to destroy. NULL is accepted and does nothing.
 */
void aprend_texture2d_destroy(aprend_texture2d texture);

/**
 * @brief Reads a 2D texture's descriptor.
 *
 * @param[in] texture Texture to read.
 *
 * @return The descriptor it was created with, except that `mip_levels` and
 *         `array_layers` are the counts actually allocated, and `width` and
 *         `height` follow aprend_texture2d_resize(). Zeroed if @p texture is
 *         NULL.
 */
aprend_texture2d_desc aprend_texture2d_get_desc(aprend_texture2d texture);

/**
 * @brief Gets the SpudGPU image behind a 2D texture.
 *
 * @param[in] texture Texture to read.
 *
 * @return The image, or NULL if @p texture is NULL. No longer valid after
 *         aprend_texture2d_resize().
 */
spudgpu_image aprend_texture2d_get_spudgpu_image(aprend_texture2d texture);

/**
 * @brief Writes a region of one mip level of one array layer of a 2D
 * texture.
 *
 * Submitted at once, on @p queue, and not waited for: the call returns when
 * the copy is submitted, and the copy runs after everything submitted before
 * it and ahead of everything submitted after. @p ppData is read during the
 * call and may be released when it returns. It is one of the instance's
 * submissions, so @p queue is the instance's one queue (aprendcontext.h): the
 * call fails if the instance has already submitted on another.
 *
 * The texture is left in the image layout it was found in, so a command list
 * already compiled against it can still be submitted. The exception is a
 * texture nothing has written or rendered to yet: it has no layout to go back
 * to, so a list compiled against it before the call must be recompiled
 * (aprend_command_list_submit() refuses it).
 *
 * @param[in] texture  Texture to write.
 * @param[in] queue    Queue the copy is submitted on. Never NULL. It must be
 *                     able to run copies between a buffer and an image.
 * @param[in] mip_level Mip level, 0 the largest. Below the texture's mip count.
 * @param[in] array_layer Array layer. Below the texture's layer count.
 * @param[in] x_offset Left edge of the region, in texels of the level.
 * @param[in] y_offset Top edge of the region, in texels.
 * @param[in] width    Width of the region. Never 0.
 * @param[in] height   Height of the region. Never 0.
 * @param[in] ppData   Address of a pointer to the texels: @p height rows of
 *                     @p width texels, tightly packed, in the texture's
 *                     format.
 *
 * @return true if the region was written. false if an argument or `*ppData`
 *         is NULL or 0, the level or layer doesn't exist, the region runs
 *         outside the level, or the copy
 *         couldn't be made or submitted.
 */
bool aprend_texture2d_update(
	aprend_texture2d texture,
	spudgpu_command_queue queue,
	uint32_t mip_level,
	uint32_t array_layer,
	uint32_t x_offset,
	uint32_t y_offset,
	uint32_t width,
	uint32_t height,
	void **ppData);

/**
 * @brief Reads back a region of one mip level of one array layer of a 2D
 * texture.
 *
 * Submitted at once on @p queue, as aprend_texture2d_update(), and unlike it
 * waited for: the call blocks until the copy has run and everything
 * submitted before it has finished, since the caller reads the result. It
 * leaves the texture's image layout the same way.
 *
 * @param[in]  texture  Texture to read.
 * @param[in]  queue    Queue the copy is submitted on. Never NULL. It must be
 *                      able to run copies between a buffer and an image.
 * @param[in]  mip_level Mip level, 0 the largest. Below the texture's mip count.
 * @param[in]  array_layer Array layer. Below the texture's layer count.
 * @param[in]  x_offset Left edge of the region, in texels of the level.
 * @param[in]  y_offset Top edge of the region, in texels.
 * @param[in]  width    Width of the region. Never 0.
 * @param[in]  height   Height of the region. Never 0.
 * @param[out] ppData   Address of a pointer to the caller's memory, which
 *                      receives @p height rows of @p width texels, tightly
 *                      packed, in the texture's format.
 *
 * @return true if the region was read. false if an argument or `*ppData` is
 *         NULL or 0, the level or layer doesn't exist, the region runs
 *         outside the level, or the copy
 *         couldn't be made or submitted.
 */
bool aprend_texture2d_get_data(
	aprend_texture2d texture,
	spudgpu_command_queue queue,
	uint32_t mip_level,
	uint32_t array_layer,
	uint32_t x_offset,
	uint32_t y_offset,
	uint32_t width,
	uint32_t height,
	void **ppData);

/**
 * @brief Replaces a 2D texture's image with a new one of the given size, the
 * same in every other respect.
 *
 * The contents are not carried over: the new image starts empty. A
 * `mip_levels` of 0 at creation gives a full chain at the new size; an
 * explicit count is kept, and the resize fails if the new size can't hold it.
 * The handle stays valid, and aprend_texture2d_get_desc() reports the new
 * size and mip count.
 *
 * @warning Before calling, destroy every aprend_texture_view made from
 * @p texture. Afterwards, recompile any command list that used it, and make
 * the views again. A submission already made that uses the old image is
 * safe: the old image is released when that work has finished.
 *
 * @param[in] texture    Texture to resize.
 * @param[in] new_width  New width of mip 0. Never 0.
 * @param[in] new_height New height of mip 0. Never 0.
 *
 * @return true if the texture has the new size, including when it already
 *         had it. false on failure, with the texture left exactly as it was.
 */
bool aprend_texture2d_resize(
	aprend_texture2d texture,
	uint32_t new_width,
	uint32_t new_height);

/**
 * @brief Configuration descriptor for a 3D texture.
 *
 * A 3D texture has neither array layers nor MSAA.
 */
typedef struct aprend_texture3d_desc {
	/** Width of mip 0 in texels. Never 0. */
	uint32_t width;
	/** Height of mip 0 in texels. Never 0. */
	uint32_t height;
	/** Depth of mip 0 in texels. Never 0. */
	uint32_t depth;
	/** Texel format. Never SPUDGPU_FORMAT_UNKNOWN. */
	SPUDGPU_FORMAT format;

	/**
	 * Typically SHADER_RESOURCE | STORAGE. DEPTH_STENCIL is not valid for a
	 * 3D texture.
	 * @see APREND_TEXTURE_USAGE_BITS
	 */
	APREND_TEXTURE_USAGE_BITS usage;

	/**
	 * 0 for a full chain, 1 for no mips, N for N levels. Every level starts
	 * undefined and nothing generates one from another: each level a
	 * sampler can reach is the caller's to write.
	 */
	uint32_t mip_levels;

#if _DEBUG
	/** A name for diagnostics. May be NULL. */
	const char *debug_name;
#endif
} aprend_texture3d_desc;

/**
 * @brief Opaque handle to a 3D texture.
 */
typedef struct aprend_texture3d_t *aprend_texture3d;

/**
 * @brief Creates a 3D texture, and a view of the whole of it that the texture
 * owns. Its contents are undefined until written.
 *
 * @param[in] instance Instance whose device the texture is created on.
 * @param[in] desc     Texture configuration. Only read during the call.
 *
 * @return The texture, or NULL if @p instance or @p desc is NULL, `width`,
 *         `height`, `depth` or `format` is 0, or the image or its view can't
 *         be created.
 *
 * @see aprend_texture3d_destroy()
 */
aprend_texture3d aprend_texture3d_create(
	aprend_instance instance,
	const aprend_texture3d_desc *desc);

/**
 * @brief Destroys a 3D texture and the view it owns.
 *
 * @warning Every aprend_texture_view made from @p texture must be destroyed
 * first, and no later submission may use it. A submission
 * already made that uses it is safe: the GPU's side of it is released when
 * that work has finished (aprendcontext.h, "Destroying").
 *
 * @param[in] texture Texture to destroy. NULL is accepted and does nothing.
 */
void aprend_texture3d_destroy(aprend_texture3d texture);

/**
 * @brief Reads a 3D texture's descriptor.
 *
 * @param[in] texture Texture to read.
 *
 * @return The descriptor it was created with, with its size and mip count
 *         following aprend_texture3d_resize(). Zeroed if @p texture is NULL.
 */
aprend_texture3d_desc aprend_texture3d_get_desc(aprend_texture3d texture);

/**
 * @brief Gets the SpudGPU view a 3D texture owns, covering the whole of it.
 *
 * @param[in] texture Texture to read.
 *
 * @return The view, or NULL if @p texture is NULL. No longer valid after
 *         aprend_texture3d_resize().
 */
spudgpu_image_view aprend_texture3d_get_spudgpu_image_view(aprend_texture3d texture);

/**
 * @brief Gets the SpudGPU image behind a 3D texture.
 *
 * @param[in] texture Texture to read.
 *
 * @return The image, or NULL if @p texture is NULL. No longer valid after
 *         aprend_texture3d_resize().
 */
spudgpu_image aprend_texture3d_get_spudgpu_image(aprend_texture3d texture);

/**
 * @brief Writes a region of one mip level of a 3D texture.
 *
 * Submitted, not waited for, and leaving the texture's image layout as
 * aprend_texture2d_update().
 *
 * @param[in] texture  Texture to write.
 * @param[in] queue    Queue the copy is submitted on. Never NULL. It must be
 *                     able to run copies between a buffer and an image.
 * @param[in] mip_level Mip level, 0 the largest. Below the texture's mip count.
 * @param[in] x_offset Left edge of the region, in texels of the level.
 * @param[in] y_offset Top edge of the region, in texels.
 * @param[in] z_offset First depth slice of the region.
 * @param[in] width    Width of the region. Never 0.
 * @param[in] height   Height of the region. Never 0.
 * @param[in] depth    Number of depth slices. Never 0.
 * @param[in] ppData   Address of a pointer to the texels: @p depth slices of
 *                     @p height rows of @p width texels, tightly packed, in
 *                     the texture's format.
 *
 * @return true if the region was written. false if an argument or `*ppData`
 *         is NULL or 0, the level or layer doesn't exist, the region runs
 *         outside the level, or the copy
 *         couldn't be made or submitted.
 */
bool aprend_texture3d_update(
	aprend_texture3d texture,
	spudgpu_command_queue queue,
	uint32_t mip_level,
	uint32_t x_offset,
	uint32_t y_offset,
	uint32_t z_offset,
	uint32_t width,
	uint32_t height,
	uint32_t depth,
	void **ppData);

/**
 * @brief Reads back a region of one mip level of a 3D texture.
 *
 * Submitted, waited for, and leaving the texture's image layout as
 * aprend_texture2d_get_data().
 *
 * @param[in]  texture  Texture to read.
 * @param[in]  queue    Queue the copy is submitted on. Never NULL. It must be
 *                      able to run copies between a buffer and an image.
 * @param[in]  mip_level Mip level, 0 the largest. Below the texture's mip count.
 * @param[in]  x_offset Left edge of the region, in texels of the level.
 * @param[in]  y_offset Top edge of the region, in texels.
 * @param[in]  z_offset First depth slice of the region.
 * @param[in]  width    Width of the region. Never 0.
 * @param[in]  height   Height of the region. Never 0.
 * @param[in]  depth    Number of depth slices. Never 0.
 * @param[out] ppData   Address of a pointer to the caller's memory, which
 *                      receives @p depth slices of @p height rows of @p width
 *                      texels, tightly packed, in the texture's format.
 *
 * @return true if the region was read, false otherwise.
 */
bool aprend_texture3d_get_data(
	aprend_texture3d texture,
	spudgpu_command_queue queue,
	uint32_t mip_level,
	uint32_t x_offset,
	uint32_t y_offset,
	uint32_t z_offset,
	uint32_t width,
	uint32_t height,
	uint32_t depth,
	void **ppData);

/**
 * @brief Replaces a 3D texture's image with a new one of the given size, as
 * aprend_texture2d_resize().
 *
 * The texture's own view is replaced with it, so a spudgpu_image_view or
 * spudgpu_image taken from @p texture before the call must be fetched again.
 *
 * @warning As aprend_texture2d_resize(): views destroyed before, command
 * lists recompiled and views made again after.
 *
 * @param[in] texture    Texture to resize.
 * @param[in] new_width  New width of mip 0. Never 0.
 * @param[in] new_height New height of mip 0. Never 0.
 * @param[in] new_depth  New depth of mip 0. Never 0.
 *
 * @return true if the texture has the new size, including when it already
 *         had it. false on failure, with the texture left exactly as it was.
 */
bool aprend_texture3d_resize(
	aprend_texture3d texture,
	uint32_t new_width,
	uint32_t new_height,
	uint32_t new_depth);

/**
 * @brief The shape of what a texture view looks at.
 *
 * Only APREND_TEXTURE_DIMENSION_2D and APREND_TEXTURE_DIMENSION_3D views can
 * be created today.
 */
typedef uint32_t APREND_TEXTURE_DIMENSION;
enum {
	APREND_TEXTURE_DIMENSION_1D       = 0,
	APREND_TEXTURE_DIMENSION_2D       = 1,
	APREND_TEXTURE_DIMENSION_3D       = 2,
	APREND_TEXTURE_DIMENSION_CUBE     = 3,
	APREND_TEXTURE_DIMENSION_1D_ARRAY = 4,
	APREND_TEXTURE_DIMENSION_2D_ARRAY = 5,
	APREND_TEXTURE_DIMENSION_3D_ARRAY = 6
};

/**
 * @brief What a texture view is used as.
 *
 * View creation requires the texture to have been created with the usage bit
 * the type needs, named on each value. An aprend_binding_set checks the type
 * against the slot the view is put in.
 */
typedef uint32_t APREND_TEXTURE_VIEW_TYPE;
enum {
	/** Not a view type: view creation refuses it. */
	APREND_TEXTURE_VIEW_TYPE_NONE             = 0,
	/**
	 * Read and written by a shader through a storage image slot. Needs
	 * APREND_TEXTURE_USAGE_BIT_STORAGE.
	 */
	APREND_TEXTURE_VIEW_TYPE_UNORDERED_ACCESS = 1,
	/** A color target of a pass. Needs APREND_TEXTURE_USAGE_BIT_RENDER_TARGET. */
	APREND_TEXTURE_VIEW_TYPE_RENDER_TAGET     = 2,
	/** The depth target of a pass. Needs APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL. */
	APREND_TEXTURE_VIEW_TYPE_DEPTH_STENCIL    = 3,
	/**
	 * Read by a shader through a sampled image or combined image sampler
	 * slot. Needs APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE. Of a texture
	 * whose format has both depth and stencil, the view is of the depth
	 * alone: a sampled view reads one or the other, never both.
	 */
	APREND_TEXTURE_VIEW_TYPE_SHADER_RESOURCE  = 4
};

/**
 * @brief Opaque handle to a view of a texture: what a pass renders to and a
 * binding set reads through.
 */
typedef struct aprend_texture_view_t *aprend_texture_view;

/**
 * @brief Creates a view of a 2D texture, covering every mip and array layer.
 *
 * @param[in] texture Texture to view. It must outlive the view.
 * @param[in] type    What the view is used as. Not
 *                    APREND_TEXTURE_VIEW_TYPE_NONE.
 *
 * @return The view, or NULL if @p texture is NULL, @p type is
 *         APREND_TEXTURE_VIEW_TYPE_NONE or not an APREND_TEXTURE_VIEW_TYPE,
 *         @p texture was created without the usage bit @p type needs, or the
 *         view can't be created.
 *
 * @see aprend_destroy_texture_view()
 */
aprend_texture_view aprend_texture_view_create_2d(
	aprend_texture2d texture,
	APREND_TEXTURE_VIEW_TYPE type);

/**
 * @brief Creates a view of a 3D texture, covering every mip.
 *
 * @param[in] texture Texture to view. It must outlive the view.
 * @param[in] type    What the view is used as. Not
 *                    APREND_TEXTURE_VIEW_TYPE_NONE.
 *
 * @return The view, or NULL if @p texture is NULL, @p type is
 *         APREND_TEXTURE_VIEW_TYPE_NONE or not an APREND_TEXTURE_VIEW_TYPE,
 *         @p texture was created without the usage bit @p type needs, or the
 *         view can't be created.
 *
 * @see aprend_destroy_texture_view()
 */
aprend_texture_view aprend_texture_view_create_3d(
	aprend_texture3d texture,
	APREND_TEXTURE_VIEW_TYPE type);

/**
 * @brief Destroys a texture view. Its texture is not destroyed.
 *
 * @warning Every aprend_binding_set holding @p view must be destroyed first,
 * and no later submission may use it. A submission
 * already made that uses it is safe: the GPU's side of it is released when
 * that work has finished (aprendcontext.h, "Destroying").
 *
 * @param[in] view View to destroy. NULL is accepted and does nothing.
 */
void aprend_destroy_texture_view(aprend_texture_view view);

/**
 * @brief Gets the type a view was created with.
 *
 * @param[in] view View to read.
 *
 * @return The type, or APREND_TEXTURE_VIEW_TYPE_NONE if @p view is NULL.
 */
APREND_TEXTURE_VIEW_TYPE aprend_texture_view_get_type(aprend_texture_view view);

/**
 * @brief Gets the shape of what a view looks at.
 *
 * @param[in] view View to read.
 *
 * @return APREND_TEXTURE_DIMENSION_2D or APREND_TEXTURE_DIMENSION_3D, or
 *         APREND_TEXTURE_DIMENSION_1D if @p view is NULL.
 */
APREND_TEXTURE_DIMENSION aprend_texture_view_get_dimension(aprend_texture_view view);

/**
 * @brief Gets the SpudGPU image view behind a view.
 *
 * @param[in] view View to read.
 *
 * @return The image view, or NULL if @p view is NULL.
 */
spudgpu_image_view aprend_texture_view_get_spudgpu_image_view(aprend_texture_view view);

/**
 * @brief Gets the 2D texture a view was made from.
 *
 * @param[in] view View to read.
 *
 * @return The texture, or NULL if @p view is NULL or is not a view of a 2D
 *         texture.
 */
aprend_texture2d aprend_texture_view_get_texture2d(aprend_texture_view view);

/**
 * @brief Configuration descriptor for a sampler: how a shader reads a texture
 * between texels and mip levels and outside [0, 1].
 *
 * Every field is the caller's to set; none has a default.
 */
typedef struct aprend_sampler_desc {
	/** Filter when a texel covers more than one pixel. */
	SPUDGPU_FILTER mag_filter;
	/** Filter when a pixel covers more than one texel. */
	SPUDGPU_FILTER min_filter;
	/** Filter between mip levels. */
	SPUDGPU_FILTER mipmap_filter;

	/** What a U coordinate outside [0, 1] reads. */
	SPUDGPU_ADDRESS_MODE address_mode_u;
	/** What a V coordinate outside [0, 1] reads. */
	SPUDGPU_ADDRESS_MODE address_mode_v;
	/** What a W coordinate outside [0, 1] reads. */
	SPUDGPU_ADDRESS_MODE address_mode_w;

	/** Added to the mip level the shader would otherwise read. */
	float mip_lod_bias;
	/** The most detailed mip level read. */
	float min_lod;
	/**
	 * The least detailed mip level read. 0 reads mip 0 only, which is what a
	 * texture whose other levels were never filled needs.
	 */
	float max_lod;

	/** 1.0 for no anisotropic filtering. */
	float max_anisotropy;

#if _DEBUG
	/** A name for diagnostics. May be NULL. */
	const char *debug_name;
#endif
} aprend_sampler_desc;

/**
 * @brief Opaque handle to a sampler.
 *
 * A sampler belongs to no texture: one can sit beside any number of them in
 * an aprend_binding_set (aprendpipeline.h).
 */
typedef struct aprend_sampler_t *aprend_sampler;

/**
 * @brief Creates a sampler.
 *
 * @param[in] instance Instance whose device the sampler is created on.
 * @param[in] desc     Sampler configuration. Only read during the call.
 *
 * @return The sampler, or NULL if @p instance or @p desc is NULL, or the
 *         sampler can't be created.
 *
 * @see aprend_sampler_destroy()
 */
aprend_sampler aprend_sampler_create(
	aprend_instance instance,
	const aprend_sampler_desc *desc);

/**
 * @brief Destroys a sampler.
 *
 * @warning Every aprend_binding_set holding @p sampler must be destroyed
 * first.
 *
 * @param[in] sampler Sampler to destroy. NULL is accepted and does nothing.
 */
void aprend_sampler_destroy(aprend_sampler sampler);

/**
 * @brief Reads the descriptor a sampler was created with.
 *
 * @param[in] sampler Sampler to read.
 *
 * @return The descriptor, or a zeroed one if @p sampler is NULL.
 */
aprend_sampler_desc aprend_sampler_get_desc(aprend_sampler sampler);

/**
 * @brief Gets the SpudGPU sampler behind a sampler.
 *
 * @param[in] sampler Sampler to read.
 *
 * @return The SpudGPU sampler, or NULL if @p sampler is NULL.
 */
spudgpu_sampler aprend_sampler_get_spudgpu_sampler(aprend_sampler sampler);

/**
 * @brief How one pass uses a color target.
 *
 * Zeroed ops mean load and store.
 */
typedef struct aprend_color_target {
	/** The view rendered to. A view of a 2D texture. */
	aprend_texture_view view;
	/** Leave NULL: a resolve target is refused until SpudGPU has MSAA. */
	aprend_texture_view resolve_view;
	/** What the pass starts with: the target's contents, a clear, or neither. */
	SPUDGPU_LOAD_OP load_op;
	/** Whether what the pass draws is kept. */
	SPUDGPU_STORE_OP store_op;
	/** RGBA the target is cleared to when #load_op clears. */
	float clear_color[4];
} aprend_color_target;

/**
 * @brief How one pass uses its depth target.
 *
 * Zeroed ops mean load and store.
 */
typedef struct aprend_depth_target {
	/** The view rendered to, a view of a 2D texture. NULL for no depth. */
	aprend_texture_view view;
	/** What the pass starts with, for depth and for stencil. */
	SPUDGPU_LOAD_OP depth_load_op, stencil_load_op;
	/** Whether what the pass writes is kept, for depth and for stencil. */
	SPUDGPU_STORE_OP depth_store_op, stencil_store_op;
	/** Depth the target is cleared to when #depth_load_op clears. */
	float clear_depth;
	/** Stencil the target is cleared to when #stencil_load_op clears. */
	uint32_t clear_stencil;
} aprend_depth_target;

#if __cplusplus
}
#endif

#endif // APREND_RENDER_IMAGES_H