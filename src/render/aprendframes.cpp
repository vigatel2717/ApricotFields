
#include "aprendimages_internal.hpp"

/* Populates framebuffer->color_attachments / depth_attachment from
 * framebuffer->desc. On partial failure, whatever was already created is left
 * in place for the caller to clean up (via aprend_framebuffer_destroy_attachments). */
static bool aprend_framebuffer_create_attachments(aprend_framebuffer_t *framebuffer) {
	const aprend_framebuffer_desc &desc = framebuffer->desc;

	framebuffer->color_attachments.reserve(desc.attachment_count);
	for (uint32_t i = 0; i < desc.attachment_count; ++i) {
		const aprend_framebuffer_texture_spec &spec = desc.attachments[i];

		aprend_texture2d_desc tex_desc{};
		tex_desc.width  = desc.width;
		tex_desc.height = desc.height;
		tex_desc.format = spec.format;
		tex_desc.usage  = APREND_TEXTURE_USAGE_BIT_RENDER_TARGET;
		if (spec.shader_resource || spec.use_for_gui)
			tex_desc.usage |= APREND_TEXTURE_USAGE_BIT_SHADER_RESOURCE;
		tex_desc.mip_levels   = 1; // attachments are single-mip; nothing generates a chain for them
		tex_desc.array_layers = 1;
		tex_desc.sample_count = desc.sample_count ? desc.sample_count : 1;
		tex_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;

		aprend_texture2d attachment = aprend_texture2d_create(framebuffer->instance, &tex_desc);
		if (!attachment)
			return false;
		framebuffer->color_attachments.push_back(attachment);
	}

	if (desc.depth_format != SPUDGPU_FORMAT_UNKNOWN) {
		aprend_texture2d_desc depth_desc{};
		depth_desc.width        = desc.width;
		depth_desc.height       = desc.height;
		depth_desc.format       = desc.depth_format;
		depth_desc.usage        = APREND_TEXTURE_USAGE_BIT_DEPTH_STENCIL;
		depth_desc.mip_levels   = 1;
		depth_desc.array_layers = 1;
		depth_desc.sample_count = desc.sample_count ? desc.sample_count : 1;
		depth_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;

		framebuffer->depth_attachment = aprend_texture2d_create(framebuffer->instance, &depth_desc);
		if (!framebuffer->depth_attachment)
			return false;
		framebuffer->has_depth_attachment = true;
	}

	return true;
}

static void aprend_framebuffer_destroy_attachments(aprend_framebuffer_t *framebuffer) {
	for (aprend_texture2d attachment : framebuffer->color_attachments)
		aprend_texture2d_destroy(attachment);
	framebuffer->color_attachments.clear();

	if (framebuffer->has_depth_attachment) {
		aprend_texture2d_destroy(framebuffer->depth_attachment);
		framebuffer->depth_attachment     = nullptr;
		framebuffer->has_depth_attachment = false;
	}
}

aprend_framebuffer_t::~aprend_framebuffer_t() { aprend_framebuffer_destroy_attachments(this); }

extern "C" {
aprend_framebuffer aprend_framebuffer_create(
    aprend_instance instance,
    const aprend_framebuffer_desc *desc) {
	if (!(instance && desc))
		return nullptr;
	if (!(desc->width && desc->height && desc->attachment_count && desc->attachments))
		return nullptr;

	aprend_framebuffer_t *result = (aprend_framebuffer_t *)malloc(sizeof(aprend_framebuffer_t));
	if (!result)
		return nullptr;
	result = new (result) aprend_framebuffer_t();

	result->instance = instance;
	result->desc     = *desc;

	// Copy the attachment specs out of the caller's array — desc->attachments
	// may point at a stack-local (see create_offscreen_color_target), which
	// won't survive until the next aprend_framebuffer_resize() call.
	result->attachment_specs.assign(desc->attachments, desc->attachments + desc->attachment_count);
	result->desc.attachments = result->attachment_specs.data();

	if (!aprend_framebuffer_create_attachments(result))
		goto failedattempt;

	return result;
failedattempt:
	result->~aprend_framebuffer_t();
	free(result);
	return nullptr;
}
void aprend_framebuffer_destroy(aprend_framebuffer framebuffer) {
	if (framebuffer) {
		framebuffer->~aprend_framebuffer_t();
		free(framebuffer);
	}
}
bool aprend_framebuffer_get_desc(
    aprend_framebuffer framebuffer,
    aprend_framebuffer_desc *out_desc) {
	if (framebuffer && out_desc) {
		*out_desc = framebuffer->desc;
		return true;
	} else
		return false;
}
bool aprend_framebuffer_resize(
    aprend_framebuffer framebuffer,
    uint32_t width,
    uint32_t height) {
	if (!(framebuffer && width && height))
		return false;
	if (framebuffer->desc.width == width && framebuffer->desc.height == height)
		return true;

	aprend_framebuffer_destroy_attachments(framebuffer);
	framebuffer->desc.width  = width;
	framebuffer->desc.height = height;
	return aprend_framebuffer_create_attachments(framebuffer);
}
bool aprend_framebuffer_read_pixel(
    aprend_framebuffer framebuffer,
    uint32_t attachment_index,
    uint32_t x,
    uint32_t y,
    void *out_data,
    uint64_t data_size) {
	if (!(framebuffer && out_data && data_size))
		return false;
	if (attachment_index >= framebuffer->color_attachments.size())
		return false;

	aprend_texture2d attachment = framebuffer->color_attachments[attachment_index];
	uint32_t bytes_per_pixel    = spudgpu_format_bit_count(attachment->desc.format) / 8;
	if (data_size < bytes_per_pixel)
		return false;

	void *pixel = out_data;
	return aprend_texture2d_get_data(attachment, x, y, 1, 1, &pixel);
}
bool aprend_framebuffer_clear_colors(
    aprend_framebuffer framebuffer,
    float r,
    float g,
    float b,
    float a) {
	if (!framebuffer)
		return false;
	if (framebuffer->color_attachments.empty())
		return true; // Nothing to clear.

	return aprend_submit_immediate(framebuffer->instance, [&](spudgpu_command_list cmd) {
		for (aprend_texture2d attachment : framebuffer->color_attachments) {
			spudgpu_cmd_image_barrier(cmd, attachment->image, attachment->current_layout, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			spudgpu_cmd_clear_color_attachment(cmd, attachment->image_view, r, g, b, a, attachment->desc.width, attachment->desc.height);
			attachment->current_layout = SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
	});
}
bool aprend_framebuffer_clear_depth(
    aprend_framebuffer framebuffer,
    bool clear_depth,
    bool clear_stencil,
    float depth,
    uint32_t stencil) {
	if (!framebuffer)
		return false;
	if (!framebuffer->has_depth_attachment)
		return false;
	if (!clear_depth && !clear_stencil)
		return true; // Nothing to clear.

	aprend_texture2d attachment = framebuffer->depth_attachment;
	return aprend_submit_immediate(framebuffer->instance, [&](spudgpu_command_list cmd) {
		spudgpu_cmd_image_barrier(cmd, attachment->image, attachment->current_layout, SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		spudgpu_cmd_clear_depth_attachment(
		    cmd, attachment->image_view, clear_depth, clear_stencil, depth, stencil, attachment->desc.width, attachment->desc.height);
		attachment->current_layout = SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	});
}
aprend_texture2d aprend_framebuffer_get_color_attachment_texture(
    aprend_framebuffer framebuffer,
    uint32_t index) {
	if (framebuffer)
		if (index < framebuffer->color_attachments.size())
			return framebuffer->color_attachments[index];
	return nullptr; // Both if's fail.
}
aprend_texture2d aprend_framebuffer_get_depth_attachment_texture(aprend_framebuffer framebuffer) {
	if (framebuffer && framebuffer->has_depth_attachment)
		return framebuffer->depth_attachment;
	else
		return nullptr;
}
}
