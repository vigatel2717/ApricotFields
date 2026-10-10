
#include "render/aprendimages.h"
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

// Vulkan-aligned aspect bits (COLOR=1, DEPTH=2, STENCIL=4) — SpudGPU doesn't
// expose a named enum for these yet.
// aprend_texture*_create pass desc->usage straight through as
// SPUDGPU_IMAGE_USAGE, which is only right while the bits line up.
static_assert(APREND_TEXTURE_USAGE_BITS(APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE) == SPUDGPU_IMAGE_USAGE(SPUDGPU_IMAGE_USAGE_SAMPLED));
static_assert(APREND_TEXTURE_USAGE_BITS(APREND_TEXTURE_USAGE_BIT_RENDER_TARGET) == SPUDGPU_IMAGE_USAGE(SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT));
static_assert(APREND_TEXTURE_USAGE_BITS(APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL) == SPUDGPU_IMAGE_USAGE(SPUDGPU_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT));
static_assert(APREND_TEXTURE_USAGE_BITS(APREND_TEXTURE_USAGE_BIT_STORAGE) == SPUDGPU_IMAGE_USAGE(SPUDGPU_IMAGE_USAGE_STORAGE));

static uint64_t aprend_texture2d_view_aspect_mask(SPUDGPU_FORMAT format) {
	switch (format) {
	case SPUDGPU_FORMAT_D24_UNORM_S8_UINT:
	case SPUDGPU_FORMAT_D32_FLOAT_S8X24_UINT:
		return 2 | 4; // DEPTH | STENCIL
	case SPUDGPU_FORMAT_D16_UNORM:
	case SPUDGPU_FORMAT_D32_FLOAT:
		return 2; // DEPTH
	default:
		return 1; // COLOR
	}
}

static uint32_t aprend_compute_mip_levels(
    uint32_t width,
    uint32_t height,
    uint32_t depth = 1) {
	uint32_t max_dim = width;
	if (height > max_dim)
		max_dim = height;
	if (depth > max_dim)
		max_dim = depth;
	uint32_t levels = 1;
	while (max_dim > 1) {
		max_dim >>= 1;
		levels++;
	}
	return levels;
}

// aprend_submit_immediate is shared — see aprendimages_internal.hpp.

aprend_texture2d_t::~aprend_texture2d_t() { spudgpu_destroy_image(this->image); }
aprend_texture3d_t::~aprend_texture3d_t() {
	// View first: SpudGPU requires an image's views to be gone before it is.
	spudgpu_destroy_image_view(this->image_view);
	spudgpu_destroy_image(this->image);
}

// The view a 3D texture owns: every mip of [image].
static SPUDRESULT aprend_texture3d_create_image_view(
    spudgpu_image image,
    SPUDGPU_FORMAT format,
    uint32_t mip_levels,
    spudgpu_image_view *out_view) {
	spudgpu_image_view_desc view_desc{};
	view_desc.parent_image                        = image;
	view_desc.type                                = SPUDGPU_IMAGE_VIEW_TYPE_3D;
	view_desc.subresource_range.aspect_mask       = aprend_texture2d_view_aspect_mask(format);
	view_desc.subresource_range.base_mip_level    = 0;
	view_desc.subresource_range.mip_level_count   = mip_levels;
	view_desc.subresource_range.base_array_layer  = 0;
	view_desc.subresource_range.array_layer_count = 1; // 3D textures do not support array layers
	return spudgpu_create_image_view(image, &view_desc, out_view);
}

// What a resize asks SpudGPU for: [texture_image] as it was created, at a new
// size. SpudGPU's own record of the image is the source, so the debug name is
// its copy rather than a pointer the caller handed over at creation.
// [requested_mip_levels] of 0 means a full chain at the new size; an explicit
// count the new size can't hold fails.
static bool aprend_resized_image_desc(
    spudgpu_image texture_image,
    uint32_t requested_mip_levels,
    uint32_t new_width,
    uint32_t new_height,
    uint32_t new_depth,
    spudgpu_image_desc *out_desc) {
	SPUDRESULT sr = spudgpu_get_image_desc(texture_image, out_desc);
	if (SPUDFAIL(sr)) {
		printf("apricot: texture resize couldn't read the image's descriptor: %s\n", spudresult_str(sr));
		return false;
	}
	const uint32_t full_chain = aprend_compute_mip_levels(new_width, new_height, new_depth);
	if (requested_mip_levels > full_chain) {
		printf("apricot: texture resize to %ux%ux%u can't hold the %u mip levels the texture was created with\n", new_width, new_height,
		       new_depth, requested_mip_levels);
		return false;
	}
	out_desc->gpu_address_location = 0;
	out_desc->width                = new_width;
	out_desc->height               = new_height;
	out_desc->depth                = new_depth;
	out_desc->mip_levels           = requested_mip_levels ? requested_mip_levels : full_chain;
	return true;
}

aprend_texture_view_t::~aprend_texture_view_t() { spudgpu_destroy_image_view(this->image_view); }
aprend_sampler_t::~aprend_sampler_t() {
	// A sampler that failed to create has nothing to destroy.
	if (this->sampler)
		spudgpu_destroy_sampler(this->sampler);
}

extern "C" {
aprend_texture2d aprend_texture2d_create(
    aprend_instance instance,
    const aprend_texture2d_desc *desc) {
	/* Implementation-specific texture creation logic goes here. */
	if (!instance || !desc)
		return nullptr;
	if (!(desc->width && desc->height && desc->format))
		return nullptr;
	if (desc->sample_count > 1)
		return nullptr; // MSAA not yet supported by SpudGPU

	aprend_texture2d_t *result = (aprend_texture2d_t *)malloc(sizeof(aprend_texture2d_t));
	if (result)
		result = new (result) aprend_texture2d_t();
	else
		return nullptr;

	result->instance             = instance;
	result->desc                 = *desc;
	result->requested_mip_levels = desc->mip_levels;

	spudgpu_image_desc image_desc{};
	image_desc.usage        = (SPUDGPU_IMAGE_USAGE)desc->usage | SPUDGPU_IMAGE_USAGE_TRANSFER_DST | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC;
	image_desc.type         = SPUDGPU_IMAGE_TYPE_2D;
	image_desc.memory_flags = desc->memory_flags;
	image_desc.format       = desc->format;
	image_desc.width        = desc->width;
	image_desc.height       = desc->height;
	image_desc.depth        = 1;
	image_desc.array_layers = desc->array_layers ? desc->array_layers : 1;
	image_desc.mip_levels   = desc->mip_levels ? desc->mip_levels : aprend_compute_mip_levels(desc->width, desc->height);
#if _DEBUG
	image_desc.debug_name = desc->debug_name;
#endif
	/* TODO: desc->initial_data upload requires aprend_texture2d_update, once mip_level/array_layer targeting is added to it. */

	SPUDRESULT sr = spudgpu_create_image(result->instance->desc.device, &image_desc, &result->image);
	if (sr != SPUD_SUCCESS)
		goto failedattempt;

	// desc->mip_levels/array_layers may have been 0 (meaning "auto"); record
	// what was actually allocated so aprend_texture2d_get_desc and later view
	// creation see the real counts instead of the sentinel.
	result->desc.mip_levels   = image_desc.mip_levels;
	result->desc.array_layers = image_desc.array_layers;

	return result;
failedattempt:
	printf("apricot: aprend_texture2d_create failed (%ux%u): %s\n", desc->width, desc->height, spudresult_str(sr));
	result->~aprend_texture2d_t();
	free(result);
	return nullptr;
}
void aprend_texture2d_destroy(aprend_texture2d texture) {
	if (texture) {
		texture->~aprend_texture2d_t();
		free(texture);
	}
}
aprend_texture2d_desc aprend_texture2d_get_desc(aprend_texture2d texture) { return texture ? texture->desc : aprend_texture2d_desc{}; }
spudgpu_image aprend_texture2d_get_spudgpu_image(aprend_texture2d texture) { return texture ? texture->image : nullptr; }
bool aprend_texture2d_update(
    aprend_texture2d texture,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t width,
    uint32_t height,
    void **ppData) {
	if (!(texture && width && height && ppData))
		return false;
	if (!(*ppData))
		return false;
	if (x_offset + width > texture->desc.width || y_offset + height > texture->desc.height)
		return false;

	uint32_t bytes_per_pixel = spudgpu_format_bit_count(texture->desc.format) / 8;
	uint32_t tight_row_pitch = width * bytes_per_pixel;

	// D3D12 requires the buffer-side row pitch to be 256-byte aligned; Vulkan
	// doesn't care. Query the backend's actual required pitch rather than
	// assuming tightly-packed rows work everywhere.
	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	spudgpu_get_image_buffer_copy_size(texture->image, 0, &aligned_row_pitch, &unused_full_mip_size);
	uint64_t region_size = aligned_row_pitch * height;

	spudgpu_buffer_desc staging_desc{};
	staging_desc.usage        = SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	staging_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT;
	staging_desc.size         = region_size;

	spudgpu_buffer staging_buffer;
	if (spudgpu_create_buffer(texture->instance->desc.device, &staging_desc, &staging_buffer) != SPUD_SUCCESS)
		return false;

	void *pMapped;
	if (spudgpu_map_buffer(staging_buffer, 0, region_size, &pMapped) != SPUD_SUCCESS) {
		spudgpu_destroy_buffer(staging_buffer);
		return false;
	}
	// Re-pack the caller's tightly-packed rows into the backend's (possibly padded) row pitch.
	const uint8_t *src = (const uint8_t *)*ppData;
	uint8_t *dst       = (uint8_t *)pMapped;
	for (uint32_t row = 0; row < height; ++row)
		memcpy(dst + row * aligned_row_pitch, src + row * tight_row_pitch, tight_row_pitch);
	spudgpu_unmap_buffer(staging_buffer);

	uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / bytes_per_pixel);

	bool ok = aprend_immediate_on_texture(texture, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, [&](spudgpu_command_list cmd) {
		spudgpu_image_buffer_copy_desc copy_desc{};
		copy_desc.mip_level         = 0;
		copy_desc.array_layer_count = 1;
		copy_desc.buffer_row_length = buffer_row_length_texels;
		copy_desc.image_x           = x_offset;
		copy_desc.image_y           = y_offset;
		copy_desc.width             = width;
		copy_desc.height            = height;
		copy_desc.depth             = 1;
		spudgpu_cmd_copy_buffer_to_image(cmd, staging_buffer, texture->image, &copy_desc);
	});

	spudgpu_destroy_buffer(staging_buffer);
	return ok;
}
bool aprend_texture2d_get_data(
    aprend_texture2d texture,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t width,
    uint32_t height,
    void **ppData) {
	if (!(texture && width && height && ppData))
		return false;
	if (!(*ppData))
		return false;
	if (x_offset + width > texture->desc.width || y_offset + height > texture->desc.height)
		return false;

	uint32_t bytes_per_pixel = spudgpu_format_bit_count(texture->desc.format) / 8;
	uint32_t tight_row_pitch = width * bytes_per_pixel;

	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	spudgpu_get_image_buffer_copy_size(texture->image, 0, &aligned_row_pitch, &unused_full_mip_size);
	uint64_t region_size = aligned_row_pitch * height;

	spudgpu_buffer_desc staging_desc{};
	staging_desc.usage        = SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
	staging_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_CACHED;
	staging_desc.size         = region_size;

	spudgpu_buffer staging_buffer;
	if (spudgpu_create_buffer(texture->instance->desc.device, &staging_desc, &staging_buffer) != SPUD_SUCCESS)
		return false;

	uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / bytes_per_pixel);

	bool ok = aprend_immediate_on_texture(texture, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC, [&](spudgpu_command_list cmd) {
		spudgpu_image_buffer_copy_desc copy_desc{};
		copy_desc.mip_level         = 0;
		copy_desc.array_layer_count = 1;
		copy_desc.buffer_row_length = buffer_row_length_texels;
		copy_desc.image_x           = x_offset;
		copy_desc.image_y           = y_offset;
		copy_desc.width             = width;
		copy_desc.height            = height;
		copy_desc.depth             = 1;
		spudgpu_cmd_copy_image_to_buffer(cmd, texture->image, staging_buffer, &copy_desc);
	});
	if (!ok) {
		spudgpu_destroy_buffer(staging_buffer);
		return false;
	}

	void *pMapped;
	if (spudgpu_map_buffer(staging_buffer, 0, region_size, &pMapped) != SPUD_SUCCESS) {
		spudgpu_destroy_buffer(staging_buffer);
		return false;
	}
	spudgpu_invalidate_buffer(staging_buffer, 0, region_size);

	// Unpack rows from the backend's (possibly padded) pitch into the caller's tightly-packed destination.
	const uint8_t *src = (const uint8_t *)pMapped;
	uint8_t *dst       = (uint8_t *)*ppData;
	for (uint32_t row = 0; row < height; ++row)
		memcpy(dst + row * tight_row_pitch, src + row * aligned_row_pitch, tight_row_pitch);
	spudgpu_unmap_buffer(staging_buffer);

	spudgpu_destroy_buffer(staging_buffer);
	return true;
}
bool aprend_texture2d_resize(
    aprend_texture2d texture,
    uint32_t new_width,
    uint32_t new_height) {
	if (!texture)
		return false;
	if (!new_width)
		return false;
	if (!new_height)
		return false;
	if (new_width == texture->desc.width && new_height == texture->desc.height)
		return true; // No resize needed

	spudgpu_image_desc image_desc{};
	if (!aprend_resized_image_desc(texture->image, texture->requested_mip_levels, new_width, new_height, 1, &image_desc))
		return false;

	spudgpu_image new_image = nullptr;
	SPUDRESULT sr           = spudgpu_create_image(texture->instance->desc.device, &image_desc, &new_image);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_texture2d_resize to %ux%u failed: %s\n", new_width, new_height, spudresult_str(sr));
		return false;
	}

	// The old image goes only now that its replacement exists, so a failed
	// resize leaves the texture exactly as it was.
	spudgpu_destroy_image(texture->image);
	texture->image           = new_image;
	texture->desc.width      = new_width;
	texture->desc.height     = new_height;
	texture->desc.mip_levels = image_desc.mip_levels;
	texture->current_layout  = SPUDGPU_IMAGE_LAYOUT_UNDEFINED; // a new image, with no contents
	return true;
}
aprend_texture3d aprend_texture3d_create(
    aprend_instance instance,
    const aprend_texture3d_desc *desc) {
	if (!instance || !desc)
		return nullptr;
	if (!(desc->width && desc->height && desc->depth && desc->format))
		return nullptr;

	aprend_texture3d_t *result = (aprend_texture3d_t *)malloc(sizeof(aprend_texture3d_t));
	if (result)
		result = new (result) aprend_texture3d_t();
	else
		return nullptr;

	result->instance             = instance;
	result->desc                 = *desc;
	result->requested_mip_levels = desc->mip_levels;
	spudgpu_image_desc image_desc{};
	image_desc.usage        = (SPUDGPU_IMAGE_USAGE)desc->usage | SPUDGPU_IMAGE_USAGE_TRANSFER_DST | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC;
	image_desc.type         = SPUDGPU_IMAGE_TYPE_3D;
	image_desc.memory_flags = desc->memory_flags;
	image_desc.format       = desc->format;
	image_desc.width        = desc->width;
	image_desc.height       = desc->height;
	image_desc.depth        = desc->depth;
	image_desc.array_layers = 1; // 3D textures do not support array layers
	image_desc.mip_levels   = desc->mip_levels ? desc->mip_levels : aprend_compute_mip_levels(desc->width, desc->height, desc->depth);
#if _DEBUG
	image_desc.debug_name = desc->debug_name;
#endif
	/* TODO: desc->initial_data upload requires aprend_texture3d_update, once mip_level targeting is added to it. */

	SPUDRESULT sr = spudgpu_create_image(result->instance->desc.device, &image_desc, &result->image);
	if (sr != SPUD_SUCCESS)
		goto failedattempt;

	// desc->mip_levels may have been 0 (meaning "auto"); record what was
	// actually allocated so aprend_texture3d_get_desc and later view creation
	// see the real count instead of the sentinel.
	result->desc.mip_levels = image_desc.mip_levels;

	sr = aprend_texture3d_create_image_view(result->image, desc->format, image_desc.mip_levels, &result->image_view);
	if (sr != SPUD_SUCCESS)
		goto failedattempt;

	return result;
failedattempt:
	printf("apricot: aprend_texture3d_create failed (%ux%ux%u): %s\n", desc->width, desc->height, desc->depth, spudresult_str(sr));
	result->~aprend_texture3d_t();
	free(result);
	return nullptr;
}
void aprend_texture3d_destroy(aprend_texture3d texture) {
	if (texture) {
		texture->~aprend_texture3d_t();
		free(texture);
	}
}
aprend_texture3d_desc aprend_texture3d_get_desc(aprend_texture3d texture) { return texture ? texture->desc : aprend_texture3d_desc{}; }
spudgpu_image_view aprend_texture3d_get_spudgpu_image_view(aprend_texture3d texture) { return texture ? texture->image_view : NULL; }
spudgpu_image aprend_texture3d_get_spudgpu_image(aprend_texture3d texture) { return texture ? texture->image : NULL; }
bool aprend_texture3d_update(
    aprend_texture3d texture,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t z_offset,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    void **ppData) {
	if (!(texture && width && height && depth && ppData))
		return false;
	if (!(*ppData))
		return false;
	if (x_offset + width > texture->desc.width || y_offset + height > texture->desc.height || z_offset + depth > texture->desc.depth)
		return false;

	uint32_t bytes_per_pixel   = spudgpu_format_bit_count(texture->desc.format) / 8;
	uint32_t tight_row_pitch   = width * bytes_per_pixel;
	uint64_t tight_slice_pitch = (uint64_t)tight_row_pitch * height;

	// Only row pitch needs backend alignment (D3D12: 256 bytes); the stride
	// between depth slices in our own staging buffer just needs to match
	// whatever row pitch we actually used.
	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	spudgpu_get_image_buffer_copy_size(texture->image, 0, &aligned_row_pitch, &unused_full_mip_size);
	uint64_t aligned_slice_pitch = aligned_row_pitch * height;
	uint64_t region_size         = aligned_slice_pitch * depth;

	spudgpu_buffer_desc staging_desc{};
	staging_desc.usage        = SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	staging_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT;
	staging_desc.size         = region_size;

	spudgpu_buffer staging_buffer;
	if (spudgpu_create_buffer(texture->instance->desc.device, &staging_desc, &staging_buffer) != SPUD_SUCCESS)
		return false;

	void *pMapped;
	if (spudgpu_map_buffer(staging_buffer, 0, region_size, &pMapped) != SPUD_SUCCESS) {
		spudgpu_destroy_buffer(staging_buffer);
		return false;
	}
	const uint8_t *src = (const uint8_t *)*ppData;
	uint8_t *dst       = (uint8_t *)pMapped;
	for (uint32_t z = 0; z < depth; ++z)
		for (uint32_t row = 0; row < height; ++row)
			memcpy(dst + z * aligned_slice_pitch + row * aligned_row_pitch, src + z * tight_slice_pitch + row * tight_row_pitch, tight_row_pitch);
	spudgpu_unmap_buffer(staging_buffer);

	uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / bytes_per_pixel);

	bool ok = aprend_immediate_on_texture(texture, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST, SPUDGPU_IMAGE_LAYOUT_SHADER_READ_ONLY, [&](spudgpu_command_list cmd) {
		spudgpu_image_buffer_copy_desc copy_desc{};
		copy_desc.mip_level           = 0;
		copy_desc.array_layer_count   = 1;
		copy_desc.buffer_row_length   = buffer_row_length_texels;
		copy_desc.buffer_image_height = height;
		copy_desc.image_x             = x_offset;
		copy_desc.image_y             = y_offset;
		copy_desc.image_z             = z_offset;
		copy_desc.width               = width;
		copy_desc.height              = height;
		copy_desc.depth               = depth;
		spudgpu_cmd_copy_buffer_to_image(cmd, staging_buffer, texture->image, &copy_desc);
	});

	spudgpu_destroy_buffer(staging_buffer);
	return ok;
}
bool aprend_texture3d_get_data(
    aprend_texture3d texture,
    uint32_t x_offset,
    uint32_t y_offset,
    uint32_t z_offset,
    uint32_t width,
    uint32_t height,
    uint32_t depth,
    void **ppData) {
	if (!(texture && width && height && depth && ppData))
		return false;
	if (!(*ppData))
		return false;
	if (x_offset + width > texture->desc.width || y_offset + height > texture->desc.height || z_offset + depth > texture->desc.depth)
		return false;

	uint32_t bytes_per_pixel   = spudgpu_format_bit_count(texture->desc.format) / 8;
	uint32_t tight_row_pitch   = width * bytes_per_pixel;
	uint64_t tight_slice_pitch = (uint64_t)tight_row_pitch * height;

	uint64_t aligned_row_pitch = 0, unused_full_mip_size = 0;
	spudgpu_get_image_buffer_copy_size(texture->image, 0, &aligned_row_pitch, &unused_full_mip_size);
	uint64_t aligned_slice_pitch = aligned_row_pitch * height;
	uint64_t region_size         = aligned_slice_pitch * depth;

	spudgpu_buffer_desc staging_desc{};
	staging_desc.usage        = SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
	staging_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_CACHED;
	staging_desc.size         = region_size;

	spudgpu_buffer staging_buffer;
	if (spudgpu_create_buffer(texture->instance->desc.device, &staging_desc, &staging_buffer) != SPUD_SUCCESS)
		return false;

	uint32_t buffer_row_length_texels = (uint32_t)(aligned_row_pitch / bytes_per_pixel);

	bool ok = aprend_immediate_on_texture(texture, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC, [&](spudgpu_command_list cmd) {
		spudgpu_image_buffer_copy_desc copy_desc{};
		copy_desc.mip_level           = 0;
		copy_desc.array_layer_count   = 1;
		copy_desc.buffer_row_length   = buffer_row_length_texels;
		copy_desc.buffer_image_height = height;
		copy_desc.image_x             = x_offset;
		copy_desc.image_y             = y_offset;
		copy_desc.image_z             = z_offset;
		copy_desc.width               = width;
		copy_desc.height              = height;
		copy_desc.depth               = depth;
		spudgpu_cmd_copy_image_to_buffer(cmd, texture->image, staging_buffer, &copy_desc);
	});
	if (!ok) {
		spudgpu_destroy_buffer(staging_buffer);
		return false;
	}

	void *pMapped;
	if (spudgpu_map_buffer(staging_buffer, 0, region_size, &pMapped) != SPUD_SUCCESS) {
		spudgpu_destroy_buffer(staging_buffer);
		return false;
	}
	spudgpu_invalidate_buffer(staging_buffer, 0, region_size);

	const uint8_t *src = (const uint8_t *)pMapped;
	uint8_t *dst       = (uint8_t *)*ppData;
	for (uint32_t z = 0; z < depth; ++z)
		for (uint32_t row = 0; row < height; ++row)
			memcpy(dst + z * tight_slice_pitch + row * tight_row_pitch, src + z * aligned_slice_pitch + row * aligned_row_pitch, tight_row_pitch);
	spudgpu_unmap_buffer(staging_buffer);

	spudgpu_destroy_buffer(staging_buffer);
	return true;
}
bool aprend_texture3d_resize(
    aprend_texture3d texture,
    uint32_t new_width,
    uint32_t new_height,
    uint32_t new_depth) {
	if (!texture)
		return false;
	if (!new_width)
		return false;
	if (!new_height)
		return false;
	if (!new_depth)
		return false;
	if (new_width == texture->desc.width && new_height == texture->desc.height && new_depth == texture->desc.depth)
		return true; // No resize needed

	spudgpu_image_desc image_desc{};
	if (!aprend_resized_image_desc(texture->image, texture->requested_mip_levels, new_width, new_height, new_depth, &image_desc))
		return false;

	spudgpu_image new_image = nullptr;
	SPUDRESULT sr           = spudgpu_create_image(texture->instance->desc.device, &image_desc, &new_image);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_texture3d_resize to %ux%ux%u failed: %s\n", new_width, new_height, new_depth, spudresult_str(sr));
		return false;
	}
	spudgpu_image_view new_view = nullptr;
	sr                          = aprend_texture3d_create_image_view(new_image, texture->desc.format, image_desc.mip_levels, &new_view);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_texture3d_resize to %ux%ux%u failed (view): %s\n", new_width, new_height, new_depth, spudresult_str(sr));
		spudgpu_destroy_image(new_image);
		return false;
	}

	// The old image and its view go only now that their replacements exist,
	// so a failed resize leaves the texture exactly as it was.
	spudgpu_destroy_image_view(texture->image_view);
	spudgpu_destroy_image(texture->image);
	texture->image           = new_image;
	texture->image_view      = new_view;
	texture->desc.width      = new_width;
	texture->desc.height     = new_height;
	texture->desc.depth      = new_depth;
	texture->desc.mip_levels = image_desc.mip_levels;
	texture->current_layout  = SPUDGPU_IMAGE_LAYOUT_UNDEFINED; // a new image, with no contents
	return true;
}

aprend_texture_view aprend_texture_view_create_2d(
    aprend_texture2d texture,
    APREND_TEXTURE_VIEW_TYPE type) {
	if (!texture || type == APREND_TEXTURE_VIEW_TYPE_NONE)
		return nullptr;
	aprend_texture_view_t *result = (aprend_texture_view_t *)malloc(sizeof(aprend_texture_view_t));
	if (!result)
		return nullptr;
	result        = new (result) aprend_texture_view_t();

	result->instance     = texture->instance;
	result->dimension    = APREND_TEXTURE_DIMENSION_2D;
	result->view_type    = type;
	result->texture._t2d = texture;

	spudgpu_image_view_desc imgvd{};
	imgvd.parent_image                        = texture->image;
	imgvd.type                                = SPUDGPU_IMAGE_VIEW_TYPE_2D;
	imgvd.subresource_range.aspect_mask       = aprend_texture2d_view_aspect_mask(texture->desc.format);
	imgvd.subresource_range.base_mip_level    = 0;
	imgvd.subresource_range.mip_level_count   = texture->desc.mip_levels;
	imgvd.subresource_range.base_array_layer  = 0;
	imgvd.subresource_range.array_layer_count = texture->desc.array_layers;
	SPUDRESULT sr = spudgpu_create_image_view(texture->image, &imgvd, &result->image_view);
	if (SPUDFAIL(sr))
		goto failedattempt;

	return result;
failedattempt:
	printf("apricot: aprend_texture_view_create_2d failed: %s\n", spudresult_str(sr));
	result->~aprend_texture_view_t();
	free(result);
	return nullptr;
}
aprend_texture_view aprend_texture_view_create_3d(
    aprend_texture3d texture,
    APREND_TEXTURE_VIEW_TYPE type) {
	if (!texture || type == APREND_TEXTURE_VIEW_TYPE_NONE)
		return nullptr;
	aprend_texture_view_t *result = (aprend_texture_view_t *)malloc(sizeof(aprend_texture_view_t));
	if (!result)
		return nullptr;
	result        = new (result) aprend_texture_view_t();

	result->instance     = texture->instance;
	result->dimension    = APREND_TEXTURE_DIMENSION_3D;
	result->view_type    = type;
	result->texture._t3d = texture;

	spudgpu_image_view_desc imgvd{};
	imgvd.parent_image                        = texture->image;
	imgvd.type                                = SPUDGPU_IMAGE_VIEW_TYPE_3D;
	imgvd.subresource_range.aspect_mask       = aprend_texture2d_view_aspect_mask(texture->desc.format);
	imgvd.subresource_range.base_mip_level    = 0;
	imgvd.subresource_range.mip_level_count   = texture->desc.mip_levels;
	imgvd.subresource_range.base_array_layer  = 0;
	imgvd.subresource_range.array_layer_count = 1; // 3D textures do not support array layers
	SPUDRESULT sr = spudgpu_create_image_view(texture->image, &imgvd, &result->image_view);
	if (SPUDFAIL(sr))
		goto failedattempt;

	return result;
failedattempt:
	printf("apricot: aprend_texture_view_create_3d failed: %s\n", spudresult_str(sr));
	result->~aprend_texture_view_t();
	free(result);
	return nullptr;
}
void aprend_destroy_texture_view(aprend_texture_view view){
	if (view){
		view->~aprend_texture_view_t();
		free(view);
	}
}
APREND_TEXTURE_VIEW_TYPE aprend_texture_view_get_type(aprend_texture_view view) { return view ? view->view_type : APREND_TEXTURE_VIEW_TYPE_NONE; }
APREND_TEXTURE_DIMENSION aprend_texture_view_get_dimension(aprend_texture_view view) { return view ? view->dimension : APREND_TEXTURE_DIMENSION_1D; }
spudgpu_image_view aprend_texture_view_get_spudgpu_image_view(aprend_texture_view view) { return view ? view->image_view : nullptr; }
aprend_texture2d aprend_texture_view_get_texture2d(aprend_texture_view view) {
	return view && view->dimension == APREND_TEXTURE_DIMENSION_2D ? view->texture._t2d : nullptr;
}

aprend_sampler aprend_sampler_create(
    aprend_instance instance,
    const aprend_sampler_desc *desc) {
	if (!instance)
		return nullptr;
	if (!desc)
		return nullptr;

	APREND_MALLOC__T(result, aprend_sampler_t);
	if (!result)
		return nullptr;
	APREND_CONSTRUCT__T(result, aprend_sampler_t);
	result->instance = instance;
	result->desc     = *desc;

	spudgpu_sampler_desc sd{};
	sd.mag_filter     = desc->mag_filter;
	sd.min_filter     = desc->min_filter;
	sd.mipmap_filter  = desc->mipmap_filter;
	sd.address_mode_u = desc->address_mode_u;
	sd.address_mode_v = desc->address_mode_v;
	sd.address_mode_w = desc->address_mode_w;
	sd.mip_lod_bias   = desc->mip_lod_bias;
	sd.min_lod        = desc->min_lod;
	sd.max_lod        = desc->max_lod;
	sd.max_anisotropy = desc->max_anisotropy;
#if _DEBUG
	sd.debug_name = desc->debug_name;
#endif

	SPUDRESULT sr = spudgpu_create_sampler(instance->desc.device, &sd, &result->sampler);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_sampler_create failed: %s\n", spudresult_str(sr));
		APREND_DESTRUCT__T(result, aprend_sampler_t);
		return nullptr;
	}
	return result;
}
void aprend_sampler_destroy(aprend_sampler sampler) {
	if (sampler) {
		APREND_DESTRUCT__T(sampler, aprend_sampler_t);
	}
}
aprend_sampler_desc aprend_sampler_get_desc(aprend_sampler sampler) { return sampler ? sampler->desc : aprend_sampler_desc{}; }
spudgpu_sampler aprend_sampler_get_spudgpu_sampler(aprend_sampler sampler) { return sampler ? sampler->sampler : nullptr; }
}
