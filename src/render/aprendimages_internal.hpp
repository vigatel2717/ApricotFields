
#include "render/aprendimages.h"
#include "render/aprendframes.h"
#include "aprend_internal.hpp"
#include <vector>

/* Records `record` onto a throwaway command list, submits it on the graphics
 * queue, and blocks until the GPU is done. Not for hot paths (see
 * spudgpu_queue_wait_idle) — only used for one-shot texture update/readback/
 * clear operations. Shared by aprendimages.cpp and aprendframes.cpp. */
template <typename Fn> static bool aprend_submit_immediate(aprend_instance instance, Fn &&record) {
	spudgpu_command_allocator_desc alloc_desc{};
	alloc_desc.type = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;

	spudgpu_command_allocator allocator;
	if (spudgpu_create_command_allocator(instance->desc.device, &alloc_desc, &allocator) != SPUD_SUCCESS)
		return false;

	spudgpu_command_list cmd;
	if (spudgpu_create_command_list(allocator, &cmd) != SPUD_SUCCESS) {
		spudgpu_destroy_command_allocator(allocator);
		return false;
	}

	spudgpu_begin_command_list(cmd);
	record(cmd);
	spudgpu_end_command_list(cmd);

	spudgpu_command_queue queue      = spudgpu_get_graphics_queue(instance->desc.device);
	spudgpu_command_list cmd_lists[] = {cmd};
	bool ok                          = spudgpu_submit_command_lists(queue, cmd_lists, 1) == SPUD_SUCCESS;
	spudgpu_queue_wait_idle(queue);

	spudgpu_destroy_command_list(cmd);
	spudgpu_destroy_command_allocator(allocator);
	return ok;
}

/* The layout an immediate operation leaves a texture in: the one it found it
 * in, so the texture's tracked layout doesn't change and command lists already
 * compiled against it stay submittable. A texture with no layout yet
 * (UNDEFINED) can't be put back, and ends in [if_undefined]. */
inline SPUDGPU_IMAGE_LAYOUT aprend_immediate_final_layout(SPUDGPU_IMAGE_LAYOUT found, SPUDGPU_IMAGE_LAYOUT if_undefined) {
	return found == SPUDGPU_IMAGE_LAYOUT_UNDEFINED ? if_undefined : found;
}

/* aprend_submit_immediate for work on one texture (aprend_texture2d_t or
 * aprend_texture3d_t): moves it into [working], runs [record], and moves it to
 * aprend_immediate_final_layout. The texture's tracked layout is only written
 * once the submission has succeeded - a failed one ran nothing, so the layout
 * is still what it was. */
template <typename Texture, typename Fn>
static bool aprend_immediate_on_texture(Texture *texture, SPUDGPU_IMAGE_LAYOUT working, SPUDGPU_IMAGE_LAYOUT if_undefined, Fn &&record) {
	const SPUDGPU_IMAGE_LAYOUT found       = texture->current_layout;
	const SPUDGPU_IMAGE_LAYOUT final_layout = aprend_immediate_final_layout(found, if_undefined);

	bool ok = aprend_submit_immediate(texture->instance, [&](spudgpu_command_list cmd) {
		spudgpu_cmd_image_barrier(cmd, texture->image, found, working);
		record(cmd);
		if (final_layout != working)
			spudgpu_cmd_image_barrier(cmd, texture->image, working, final_layout);
	});
	if (ok)
		texture->current_layout = final_layout;
	return ok;
}

typedef struct aprend_texture2d_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_texture2d_t() = default;
    ~aprend_texture2d_t();
    aprend_instance instance;
    aprend_texture2d_desc desc;
    spudgpu_image image{nullptr};
    SPUDGPU_IMAGE_LAYOUT current_layout{SPUDGPU_IMAGE_LAYOUT_UNDEFINED};
    // desc.mip_levels as the caller gave it (0 = full chain). desc holds the
    // count actually allocated, so a resize needs this to know which was asked.
    uint32_t requested_mip_levels{0};
} aprend_texture2d_t;

typedef struct aprend_texture3d_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_texture3d_t() = default;
    ~aprend_texture3d_t();
    aprend_instance instance;
    aprend_texture3d_desc desc;
    spudgpu_image image{nullptr};
    spudgpu_image_view image_view{nullptr};
    SPUDGPU_IMAGE_LAYOUT current_layout{SPUDGPU_IMAGE_LAYOUT_UNDEFINED};
    // As aprend_texture2d_t::requested_mip_levels.
    uint32_t requested_mip_levels{0};
} aprend_texture3d_t;

typedef struct aprend_texture_view_t
{
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_texture_view_t() = default;
	~aprend_texture_view_t();
	aprend_instance instance{nullptr};
	APREND_TEXTURE_VIEW_TYPE view_type{APREND_TEXTURE_VIEW_TYPE_NONE};
	APREND_TEXTURE_DIMENSION dimension{APREND_TEXTURE_DIMENSION_1D};
	spudgpu_image_view image_view{nullptr};
	union {
		aprend_texture2d _t2d;
		aprend_texture3d _t3d;
	} texture;
} aprend_texture_view_t;

/* The image [view] looks at, and through [out_tracked] where that texture's
 * tracked layout is kept. Views are of a 2D or a 3D texture, nothing else. */
inline spudgpu_image aprend_texture_view_image(aprend_texture_view_t *view, SPUDGPU_IMAGE_LAYOUT **out_tracked) {
	if (view->dimension == APREND_TEXTURE_DIMENSION_3D) {
		*out_tracked = &view->texture._t3d->current_layout;
		return view->texture._t3d->image;
	}
	*out_tracked = &view->texture._t2d->current_layout;
	return view->texture._t2d->image;
}

typedef struct aprend_sampler_t
{
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_sampler_t() = default;
	~aprend_sampler_t();
	aprend_instance instance{nullptr};
	aprend_sampler_desc desc{};
	spudgpu_sampler sampler{nullptr};
} aprend_sampler_t;

typedef struct aprend_framebuffer_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_framebuffer_t() = default;
    ~aprend_framebuffer_t();
    aprend_instance instance;
    aprend_framebuffer_desc desc;
    // Owns the attachment specs desc.attachments points at — the caller's
    // array (see create_offscreen_color_target's stack-local color_spec) is
    // not guaranteed to outlive this framebuffer, but aprend_framebuffer_resize
    // re-reads desc.attachments on every resize, long after creation returns.
    std::vector<aprend_framebuffer_texture_spec> attachment_specs;
    std::vector<aprend_texture_view> color_attachments;
    // Parallel to color_attachments: the underlying texture if we created it
    // (needs destroying), or nullptr if it was borrowed via
    // aprend_framebuffer_texture_spec::existing_texture (caller still owns it).
    std::vector<aprend_texture2d> owned_color_textures;
    aprend_texture_view depth_attachment{nullptr};
    bool has_depth_attachment{false};
} aprend_framebuffer_t;
