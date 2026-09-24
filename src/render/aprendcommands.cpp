
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

#include <cstdio>

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
	cmd_list->color_target_storage.clear();
	cmd_list->recording_error = false;
	cmd_list->layout_uses.clear();
	cmd_list->compiled           = false;
	cmd_list->present_swap_chain = nullptr;
}
void aprend_send_command(
    aprend_command_list cmd_list,
    APREND_COMMAND cmd) {
	if (!cmd_list)
		return;
	if (cmd._type == APREND_COMMAND_NONE)
		return;

	if (cmd._type == APREND_COMMAND_BEGIN_RENDERING) {
		auto &p = cmd._params._begin_rendering;
		if (p._color_target_count > APREND_MAX_COLOR_TARGETS || (p._color_target_count > 0 && !p._color_targets)) {
			printf("aprend: BEGIN_RENDERING with invalid color targets (count %u)\n", p._color_target_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._color_target_count > 0) {
			cmd_list->color_target_storage.emplace_back(p._color_targets, p._color_targets + p._color_target_count);
			p._color_targets = cmd_list->color_target_storage.back().data();
		} else {
			p._color_targets = nullptr;
		}
	}

	cmd_list->commands.emplace_back(cmd);
}

/* Moves [view]'s texture into [layout]. Issued even when the layout already
 * matches: back-to-back passes on the same attachment (e.g. a LOAD after a
 * previous pass's STORE) still need the barrier's memory dependency. Render
 * targets are always 2D views (see aprend_begin_rendering_valid).
 *
 * Tracks the layout in [cmd_list]'s own layout_uses, never on the texture:
 * the texture's layout only changes once the list is actually submitted. */
static void aprend_cmd_transition_texture(
    aprend_command_list cmd_list,
    aprend_texture2d tex,
    SPUDGPU_IMAGE_LAYOUT layout) {
	aprend_command_list_t::layout_use *use = nullptr;
	for (auto &u : cmd_list->layout_uses)
		if (u.texture == tex)
			use = &u;
	if (!use) {
		// First use in this list: start from the layout everything submitted
		// so far leaves it in.
		cmd_list->layout_uses.push_back({tex, tex->current_layout, tex->current_layout});
		use = &cmd_list->layout_uses.back();
	}

	spudgpu_cmd_image_barrier(cmd_list->cmd_list, tex->image, use->final_layout, layout);
	use->final_layout = layout;
}
static void aprend_cmd_transition_view(
    aprend_command_list cmd_list,
    aprend_texture_view view,
    SPUDGPU_IMAGE_LAYOUT layout) {
	aprend_cmd_transition_texture(cmd_list, view->texture._t2d, layout);
}

/* PRESENT_TEXTURE: copies the source into the acquired back buffer and moves
 * the back buffer to PRESENT_SRC. The back buffer's previous contents are
 * discarded (UNDEFINED old layout) since the copy overwrites the region that
 * gets shown. */
static bool aprend_cmd_present_texture(
    aprend_command_list cmd_list,
    const APREND_COMMAND &command) {
	const auto &p         = command._params._present_texture;
	aprend_swap_chain sc  = p._swap_chain;
	aprend_texture2d src  = p._source;
	if (!sc || !sc->swap_chain || !src) {
		printf("aprend: PRESENT_TEXTURE with a NULL source or swap chain\n");
		return false;
	}
	if (sc->acquired_image == APREND_SWAP_CHAIN_NO_IMAGE || sc->submitted) {
		printf("aprend: PRESENT_TEXTURE without a freshly acquired back buffer (call aprend_swap_chain_acquire first)\n");
		return false;
	}
	if (cmd_list->present_swap_chain) {
		printf("aprend: more than one PRESENT_TEXTURE in one command list\n");
		return false;
	}

	spudgpu_image_view back_view = spudgpu_get_swap_chain_image_view(sc->swap_chain, sc->acquired_image);
	spudgpu_image_view_desc back_view_desc{};
	if (!back_view || SPUDFAIL(spudgpu_get_image_view_desc(back_view, &back_view_desc)) || !back_view_desc.parent_image) {
		printf("aprend: PRESENT_TEXTURE couldn't get the acquired back buffer\n");
		return false;
	}
	// The real back-buffer size, which the surface can override on Vulkan -
	// not necessarily the size the swap chain was requested at.
	spudgpu_swap_chain_desc actual{};
	if (SPUDFAIL(spudgpu_get_swap_chain_desc(sc->swap_chain, &actual)) || !actual.width || !actual.height) {
		printf("aprend: PRESENT_TEXTURE couldn't get the back buffer size\n");
		return false;
	}

	spudgpu_command_list cmd = cmd_list->cmd_list;
	aprend_cmd_transition_texture(cmd_list, src, SPUDGPU_IMAGE_LAYOUT_TRANSFER_SRC);
	spudgpu_cmd_image_barrier_view(cmd, back_view, SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST);

	uint32_t width  = src->desc.width < actual.width ? src->desc.width : actual.width;
	uint32_t height = src->desc.height < actual.height ? src->desc.height : actual.height;
	spudgpu_image_blit_desc blit{};
	blit.src_array_layer_count = 1;
	blit.src_x1                = width;
	blit.src_y1                = height;
	blit.src_z1                = 1;
	blit.dst_array_layer_count = 1;
	blit.dst_x1                = width;
	blit.dst_y1                = height;
	blit.dst_z1                = 1;
	blit.filter                = SPUDGPU_FILTER_NEAREST;
	spudgpu_cmd_blit_image(cmd, src->image, back_view_desc.parent_image, &blit);

	spudgpu_cmd_image_barrier_view(cmd, back_view, SPUDGPU_IMAGE_LAYOUT_TRANSFER_DST, SPUDGPU_IMAGE_LAYOUT_PRESENT_SRC);

	cmd_list->present_swap_chain  = sc;
	cmd_list->present_image_index = sc->acquired_image;
	return true;
}

static bool aprend_render_view_valid(aprend_texture_view view) {
	if (!view)
		return false;
	return view->image_view && view->dimension == APREND_TEXTURE_DIMENSION_2D && view->texture._t2d;
}

static bool aprend_begin_rendering_valid(const APREND_COMMAND &command) {
	const auto &p = command._params._begin_rendering;
	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		if (!aprend_render_view_valid(p._color_targets[i].view)) {
			printf("aprend: BEGIN_RENDERING color target %u is not a valid 2D texture view\n", i);
			return false;
		}
		/* spudgpu_color_attachment_desc has no resolve attachment yet, so a
		 * resolve can't be honored — fail rather than silently skip it. */
		if (p._color_targets[i].resolve_view) {
			printf("aprend: BEGIN_RENDERING color target %u has a resolve_view, which SpudGPU doesn't support yet\n", i);
			return false;
		}
	}
	if (p._depth_target.view && !aprend_render_view_valid(p._depth_target.view)) {
		printf("aprend: BEGIN_RENDERING depth target is not a valid 2D texture view\n");
		return false;
	}
	if (p._color_target_count == 0 && !p._depth_target.view) {
		printf("aprend: BEGIN_RENDERING has no color or depth target\n");
		return false;
	}
	return true;
}

/* Opens an explicit BEGIN_RENDERING pass: transitions every attachment into
 * its attachment layout (barriers only happen outside a pass), then begins
 * dynamic rendering with each attachment's own load/store ops and clear
 * values. Assumes aprend_begin_rendering_valid passed. */
static void aprend_cmd_begin_rendering(
    aprend_command_list cmd_list,
    const APREND_COMMAND &command) {
	const auto &p            = command._params._begin_rendering;
	spudgpu_command_list cmd = cmd_list->cmd_list;

	spudgpu_rendering_begin_desc desc{};
	desc.color_attachment_count = p._color_target_count;
	desc.x                      = p._x;
	desc.y                      = p._y;
	desc.width                  = p._width;
	desc.height                 = p._height;
	desc.will_execute_bundles   = (p._flags & APREND_RENDERING_FLAG_EXECUTES_BUNDLES) != 0;

	for (uint32_t i = 0; i < p._color_target_count; ++i) {
		const aprend_color_target &target = p._color_targets[i];
		aprend_cmd_transition_view(cmd_list, target.view, SPUDGPU_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

		spudgpu_color_attachment_desc &color = desc.color_attachments[i];
		color.image_view                     = target.view->image_view;
		color.load_op                        = target.load_op;
		color.store_op                       = target.store_op;
		memcpy(color.clear_color, target.clear_color, sizeof(color.clear_color));
	}

	const aprend_depth_target &depth = p._depth_target;
	if (depth.view) {
		aprend_cmd_transition_view(cmd_list, depth.view, SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

		desc.depth_attachment.image_view       = depth.view->image_view;
		desc.depth_attachment.depth_load_op    = depth.depth_load_op;
		desc.depth_attachment.depth_store_op   = depth.depth_store_op;
		desc.depth_attachment.stencil_load_op  = depth.stencil_load_op;
		desc.depth_attachment.stencil_store_op = depth.stencil_store_op;
		desc.depth_attachment.clear_depth      = depth.clear_depth;
		desc.depth_attachment.clear_stencil    = depth.clear_stencil;
	}

	if (desc.width == 0 || desc.height == 0) {
		aprend_texture2d sizing = p._color_target_count > 0 ? p._color_targets[0].view->texture._t2d : depth.view->texture._t2d;
		desc.width              = sizing->desc.width;
		desc.height             = sizing->desc.height;
	}

	spudgpu_cmd_begin_rendering(cmd, &desc);
}

bool aprend_command_list_compile(aprend_command_list cmd_list) {
	if (!cmd_list)
		return false;
	if (!cmd_list->cmd_list)
		return false;
	cmd_list->compiled = false;
	cmd_list->layout_uses.clear();
	cmd_list->present_swap_chain = nullptr;
	if (cmd_list->recording_error)
		return false;

	spudgpu_command_list cmd = cmd_list->cmd_list;
	spudgpu_begin_command_list(cmd);

	/* Whether a BEGIN_RENDERING pass is currently open. */
	bool in_pass = false;
	bool ok      = true;
	/* The pipeline most recently bound, and the uniform set bound since then. */
	aprend_graphics_pipeline bound_pipeline = nullptr;
	aprend_uniform_set bound_uniform_set    = nullptr;

	std::vector<spudgpu_buffer_view> vertex_buffer_views;

	for (const APREND_COMMAND &command : cmd_list->commands) {
		switch (command._type) {
		case APREND_COMMAND_DRAW:
		case APREND_COMMAND_DRAW_INDEXED:
		case APREND_COMMAND_DRAW_INSTANCED:
		case APREND_COMMAND_DRAW_INSTANCED_INDEXED:
			if (!in_pass) {
				printf("aprend: draw command recorded outside a rendering pass\n");
				ok = false;
			} else if (bound_pipeline && bound_pipeline->uniform_layout && !bound_uniform_set) {
				printf("aprend: draw with a pipeline that declares uniform bindings, but no SET_UNIFORM_SET since binding it\n");
				ok = false;
			}
			break;
		default:
			break;
		}
		if (!ok)
			break;

		switch (command._type) {
		case APREND_COMMAND_SET_VERTEX_BUFFERS: {
			const auto &p = command._params._set_vertex_buffers;
			vertex_buffer_views.clear();
			vertex_buffer_views.reserve(p._vertex_buffer_count);
			for (uint32_t i = 0; i < p._vertex_buffer_count; ++i)
				vertex_buffer_views.push_back(p._vertex_buffers[i]->buffer_view);
			spudgpu_cmd_set_vertex_buffers(cmd, p._start_slot, p._vertex_buffer_count, vertex_buffer_views.data());
			break;
		}
		case APREND_COMMAND_SET_INDEX_BUFFER: {
			const auto &p = command._params._set_index_buffer;
			spudgpu_cmd_set_index_buffer(cmd, p._index_buffer->buffer_view);
			break;
		}
		case APREND_COMMAND_SET_SHADER_PIPELINE: {
			const auto &p = command._params._set_shader_pipeline;
			if (!p._pipeline) {
				printf("aprend: SET_SHADER_PIPELINE with a NULL pipeline\n");
				ok = false;
				break;
			}
			spudgpu_cmd_bind_pipeline(cmd, p._pipeline->pipeline);
			bound_pipeline    = p._pipeline;
			bound_uniform_set = nullptr; // not guaranteed to survive a pipeline change on every backend
			break;
		}
		case APREND_COMMAND_SET_UNIFORM_SET: {
			aprend_uniform_set set = command._params._set_uniform_set._uniform_set;
			if (!set || !bound_pipeline || set->pipeline != bound_pipeline) {
				printf("aprend: SET_UNIFORM_SET with a set that doesn't belong to the bound pipeline\n");
				ok = false;
				break;
			}
			spudgpu_cmd_bind_descriptor_sets(cmd, bound_pipeline->pipeline, 0, &set->set, 1);
			bound_uniform_set = set;
			break;
		}
		case APREND_COMMAND_PRESENT_TEXTURE: {
			if (in_pass) {
				printf("aprend: PRESENT_TEXTURE inside a rendering pass\n");
				ok = false;
				break;
			}
			ok = aprend_cmd_present_texture(cmd_list, command);
			break;
		}
		case APREND_COMMAND_DRAW: {
			const auto &p = command._params._draw;
			spudgpu_cmd_draw(cmd, p._vertex_count, p._start_vertex_location);
			break;
		}
		case APREND_COMMAND_DRAW_INDEXED: {
			const auto &p = command._params._draw_indexed;
			spudgpu_cmd_draw_indexed(cmd, p._index_count, p._start_index_location, p._base_vertex_location);
			break;
		}
		case APREND_COMMAND_DRAW_INSTANCED: {
			const auto &p = command._params._draw_instanced;
			spudgpu_cmd_draw_instanced(cmd, p._vertex_count_per_instance, p._instance_count, p._start_vertex_location, p._start_instance_location);
			break;
		}
		case APREND_COMMAND_DRAW_INSTANCED_INDEXED: {
			const auto &p = command._params._draw_indexed_instanced;
			spudgpu_cmd_draw_indexed_instanced(
			    cmd, p._index_count_per_instance, p._instance_count, p._start_index_location, p._base_vertex_location,
			    p._start_instance_location);
			break;
		}
		case APREND_COMMAND_SET_VIEWPORTS: {
			const auto &p = command._params._set_viewports;
			spudgpu_cmd_set_viewports(cmd, p._first_viewport, p._viewport_count, p._viewports);
			break;
		}
		case APREND_COMMAND_SET_SCISSOR_RECTS: {
			const auto &p = command._params._set_scissor_rects;
			spudgpu_cmd_set_scissor_rects(cmd, p._first_scissor_rect, p._scissor_rect_count, p._scissor_rects);
			break;
		}
		case APREND_COMMAND_BEGIN_RENDERING: {
			if (in_pass) {
				printf("aprend: BEGIN_RENDERING while another rendering pass is open\n");
				ok = false;
				break;
			}
			if (!aprend_begin_rendering_valid(command)) {
				ok = false;
				break;
			}
			aprend_cmd_begin_rendering(cmd_list, command);
			in_pass = true;
			break;
		}
		case APREND_COMMAND_END_RENDERING: {
			if (!in_pass) {
				printf("aprend: END_RENDERING without an open BEGIN_RENDERING pass\n");
				ok = false;
				break;
			}
			spudgpu_cmd_end_rendering(cmd);
			in_pass = false;
			break;
		}
		default:
			break;
		}
		if (!ok)
			break;
	}

	if (ok && in_pass) {
		printf("aprend: BEGIN_RENDERING pass left open at the end of the command list\n");
		ok = false;
	}

	/* Always leave the spudgpu list closed and balanced, even on failure —
	 * aprend_command_list_submit refuses it when this returns false. */
	if (in_pass)
		spudgpu_cmd_end_rendering(cmd);

	spudgpu_end_command_list(cmd);
	if (!ok) {
		cmd_list->layout_uses.clear();
		cmd_list->present_swap_chain = nullptr;
	}
	cmd_list->compiled = ok;
	return ok;
}

bool aprend_command_list_submit(aprend_command_list cmd_list, spudgpu_command_queue queue) {
	if (!cmd_list || !queue)
		return false;
	if (!cmd_list->compiled) {
		printf("aprend: submit of a command list without a successful compile\n");
		return false;
	}
	// Every texture must still be in the layout this list's barriers were
	// compiled against; if another submission moved it since, the barriers
	// would lie to the GPU.
	for (const auto &use : cmd_list->layout_uses) {
		if (use.texture->current_layout != use.initial_layout) {
			printf("aprend: submit refused: a texture's layout changed since this list was compiled; recompile it\n");
			return false;
		}
	}

	aprend_swap_chain sc = cmd_list->present_swap_chain;
	if (sc) {
		// The list copies into one specific acquired back buffer; it's only
		// valid while that same one is still acquired and unsubmitted.
		if (sc->acquired_image != cmd_list->present_image_index || sc->submitted) {
			printf("aprend: submit refused: the back buffer this list presents into is no longer the acquired one; recompile it\n");
			return false;
		}
		if (queue != sc->desc.queue) {
			printf("aprend: submit refused: a list presenting into a swap chain must go on that swap chain's queue\n");
			return false;
		}
	}

	spudgpu_command_list lists[] = {cmd_list->cmd_list};
	if (sc) {
		// Waits for the acquire to finish and signals presentation.
		if (SPUDFAIL(spudgpu_submit_command_lists_synced(queue, lists, 1, sc->swap_chain)))
			return false;
		sc->submitted = true;
	} else if (SPUDFAIL(spudgpu_submit_command_lists(queue, lists, 1))) {
		return false;
	}

	for (const auto &use : cmd_list->layout_uses)
		use.texture->current_layout = use.final_layout;
	return true;
}
}
