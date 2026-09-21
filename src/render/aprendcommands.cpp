
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

aprend_command_list_t::~aprend_command_list_t() {
	commands.clear();
	spudgpu_destroy_command_list(cmd_list);
}

extern "C" {

aprend_command_list aprend_command_list_create(aprend_instance instance) {
	if (!instance)
		return nullptr;
	aprend_command_list_t *result = (aprend_command_list_t *)malloc(sizeof(aprend_command_list_t));
	if (!result)
		return nullptr;
	result = new (result) aprend_command_list_t();

	result->instance = instance;

	if (SPUDFAIL(spudgpu_create_command_list(instance->cmd_allocator, &result->cmd_list)))
		goto failedattempt;

	return result;
failedattempt:
	result->~aprend_command_list_t();
	free(result);
	return nullptr;
}
void aprend_command_list_destroy(aprend_command_list cmd_list) {
	if (cmd_list) {
		cmd_list->~aprend_command_list_t();
		free(cmd_list);
	}
}

void aprend_command_list_reset(aprend_command_list cmd_list) {
	if (!cmd_list)
		return;
	const size_t preserve_size = cmd_list->commands.size();
	cmd_list->commands.clear();
	cmd_list->commands.reserve(preserve_size);
}
void aprend_send_command(
    aprend_command_list cmd_list,
    APREND_COMMAND cmd) {
	if (!cmd_list || cmd._type == APREND_COMMAND_NONE)
		return;
	cmd_list->commands.emplace_back(cmd);
}

/* Opens (or re-opens after a prior SET_RENDER_TARGETS) a dynamic-rendering
 * pass for the given attachments, transitioning each into the layout
 * spudgpu_cmd_begin_rendering requires. [pending_clear_color] /
 * [pending_clear_depth] / [pending_clear_stencil] are whatever the most
 * recent CLEAR_COLORS / CLEAR_DEPTH commands set them to — CLEAR_COLORS and
 * CLEAR_DEPTH are latched state, only actually applied once a
 * SET_RENDER_TARGETS with a CLEAR load op consumes them. */
static void aprend_cmd_begin_render_targets(
    spudgpu_command_list cmd,
    const APREND_COMMAND &command,
    const float pending_clear_color[4],
    float pending_clear_depth,
    uint32_t pending_clear_stencil) {
	const auto &params = command._params._set_render_targets;

	spudgpu_rendering_begin_desc desc{};
	desc.color_attachment_count = params._color_attachment_count;

	for (uint32_t i = 0; i < params._color_attachment_count; ++i) {
		aprend_texture_view view = params._color_attachments[i];
		aprend_texture2d tex     = view->texture._t2d;

		spudgpu_cmd_image_barrier(cmd, tex->image, tex->current_layout, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		tex->current_layout = SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		spudgpu_color_attachment_desc &color = desc.color_attachments[i];
		color.image_view                     = view->image_view;
		color.load_op                        = (SPUDGPU_LOAD_OP)params._color_load_op;
		color.store_op                       = SPUDGPU_STORE_OP_STORE;
		if (color.load_op == SPUDGPU_LOAD_OP_CLEAR)
			memcpy(color.clear_color, pending_clear_color, sizeof(color.clear_color));

		if (i == 0) {
			desc.width  = tex->desc.width;
			desc.height = tex->desc.height;
		}
	}

	if (params._depth_attachment) {
		aprend_texture_view view = params._depth_attachment;
		aprend_texture2d tex     = view->texture._t2d;

		spudgpu_cmd_image_barrier(cmd, tex->image, tex->current_layout, SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		tex->current_layout = SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		desc.depth_attachment.image_view       = view->image_view;
		desc.depth_attachment.depth_load_op    = (SPUDGPU_LOAD_OP)params._depth_load_op;
		desc.depth_attachment.depth_store_op   = SPUDGPU_STORE_OP_STORE;
		desc.depth_attachment.stencil_load_op  = (SPUDGPU_LOAD_OP)params._depth_load_op;
		desc.depth_attachment.stencil_store_op = SPUDGPU_STORE_OP_STORE;
		if (desc.depth_attachment.depth_load_op == SPUDGPU_LOAD_OP_CLEAR) {
			desc.depth_attachment.clear_depth   = pending_clear_depth;
			desc.depth_attachment.clear_stencil = pending_clear_stencil;
		}

		if (params._color_attachment_count == 0) {
			desc.width  = tex->desc.width;
			desc.height = tex->desc.height;
		}
	}

	spudgpu_cmd_begin_rendering(cmd, &desc);
}

bool aprend_command_list_compile(aprend_command_list cmd_list) {
	if (!cmd_list || !cmd_list->cmd_list)
		return false;

	spudgpu_command_list cmd = cmd_list->cmd_list;
	spudgpu_begin_command_list(cmd);

	bool rendering_open = false;
	float pending_clear_color[4]  = {0.0f, 0.0f, 0.0f, 1.0f};
	float pending_clear_depth     = 1.0f;
	uint32_t pending_clear_stencil = 0;

	std::vector<spudgpu_buffer_view> vertex_buffer_views;

	for (const APREND_COMMAND &command : cmd_list->commands) {
		switch (command._type) {
		case APREND_COMMAND_SET_VERTEX_BUFFERS: {
			const auto &p = command._params._set_vertex_buffers;
			vertex_buffer_views.clear();
			vertex_buffer_views.reserve(p._vertex_buffer_count);
			for (uint32_t i = 0; i < p._vertex_buffer_count; ++i)
				vertex_buffer_views.push_back(p._vertex_buffers[i]->buffer_view);
			spudgpu_set_vertex_buffers(cmd, p._start_slot, p._vertex_buffer_count, vertex_buffer_views.data());
			break;
		}
		case APREND_COMMAND_SET_INDEX_BUFFER: {
			const auto &p = command._params._set_index_buffer;
			spudgpu_set_index_buffer(cmd, p._index_buffer->buffer_view);
			break;
		}
		case APREND_COMMAND_SET_SHADER_PIPELINE: {
			const auto &p = command._params._set_shader_pipeline;
			spudgpu_cmd_bind_pipeline(cmd, p._pipeline->pipeline);
			break;
		}
		case APREND_COMMAND_DRAW: {
			const auto &p = command._params._draw;
			spudgpu_draw(cmd, p._vertex_count, p._start_vertex_location);
			break;
		}
		case APREND_COMMAND_DRAW_INDEXED: {
			const auto &p = command._params._draw_indexed;
			spudgpu_draw_indexed(cmd, p._index_count, p._start_index_location, p._base_vertex_location);
			break;
		}
		case APREND_COMMAND_DRAW_INSTANCED: {
			const auto &p = command._params._draw_instanced;
			spudgpu_draw_instanced(cmd, p._vertex_count_per_instance, p._instance_count, p._start_vertex_location, p._start_instance_location);
			break;
		}
		case APREND_COMMAND_DRAW_INSTANCED_INDEXED: {
			const auto &p = command._params._draw_indexed_instanced;
			spudgpu_draw_indexed_instanced(
			    cmd, p._index_count_per_instance, p._instance_count, p._start_index_location, p._base_vertex_location,
			    p._start_instance_location);
			break;
		}
		case APREND_COMMAND_SET_VIEWPORTS: {
			const auto &p = command._params._set_viewports;
			spudgpu_set_viewports(cmd, p._first_viewport, p._viewport_count, p._viewports);
			break;
		}
		case APREND_COMMAND_SET_SCISSOR_RECTS: {
			const auto &p = command._params._set_scissor_rects;
			spudgpu_set_scissor_rects(cmd, p._first_scissor_rect, p._scissor_rect_count, p._scissor_rects);
			break;
		}
		case APREND_COMMAND_CLEAR_COLORS: {
			const auto &p      = command._params._clear_colors;
			pending_clear_color[0] = p._r;
			pending_clear_color[1] = p._g;
			pending_clear_color[2] = p._b;
			pending_clear_color[3] = p._a;
			break;
		}
		case APREND_COMMAND_CLEAR_DEPTH: {
			const auto &p       = command._params._clear_depth;
			pending_clear_depth   = p._depth;
			pending_clear_stencil = p._stencil;
			break;
		}
		case APREND_COMMAND_SET_RENDER_TARGETS: {
			if (rendering_open)
				spudgpu_cmd_end_rendering(cmd);
			aprend_cmd_begin_render_targets(cmd, command, pending_clear_color, pending_clear_depth, pending_clear_stencil);
			rendering_open = true;
			break;
		}
		default:
			break;
		}
	}

	if (rendering_open)
		spudgpu_cmd_end_rendering(cmd);

	spudgpu_end_command_list(cmd);
	return true;
}

spudgpu_command_list aprend_command_list_get_spudgpu_command_list(aprend_command_list cmd_list) {
	return cmd_list ? cmd_list->cmd_list : nullptr;
}
}
