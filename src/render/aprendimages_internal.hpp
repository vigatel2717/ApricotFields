
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

	record(cmd);

	spudgpu_command_queue queue      = spudgpu_get_graphics_queue(instance->desc.device);
	spudgpu_command_list cmd_lists[] = {cmd};
	bool ok                          = spudgpu_submit_command_lists(queue, cmd_lists, 1) == SPUD_SUCCESS;
	spudgpu_queue_wait_idle(queue);

	spudgpu_destroy_command_list(cmd);
	spudgpu_destroy_command_allocator(allocator);
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
    spudgpu_image image;
    spudgpu_image_view image_view{nullptr};
    SPUDGPU_IMAGE_LAYOUT current_layout{SPUDGPU_IMAGE_LAYOUT_UNDEFINED};
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
    spudgpu_image image;
    spudgpu_image_view image_view{nullptr};
    SPUDGPU_IMAGE_LAYOUT current_layout{SPUDGPU_IMAGE_LAYOUT_UNDEFINED};
} aprend_texture3d_t;


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
    std::vector<aprend_texture2d> color_attachments;
    aprend_texture2d depth_attachment{nullptr};
    bool has_depth_attachment{false};
} aprend_framebuffer_t;
