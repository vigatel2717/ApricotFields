
#include "aprend_internal.hpp"
#include "aprendimages_internal.hpp"

#include <cstdio>

aprend_command_list_t::~aprend_command_list_t() {
	commands.clear();
	// A list that failed to create may have neither.
	if (upload_list)
		spudgpu_destroy_command_list(upload_list);
	if (cmd_list)
		spudgpu_destroy_command_list(cmd_list);
}

/* Adds [store] to the staged stores [cmd_list] uses, once. A store that is
 * written directly has nothing to copy and isn't added. */
static void aprend_cmd_use_store(
    aprend_command_list cmd_list,
    aprend_buffer_store *store) {
	if (!store->staging)
		return;
	for (aprend_buffer_store *known : cmd_list->staged_stores)
		if (known == store)
			return;
	cmd_list->staged_stores.push_back(store);
}

/* Records into [cmd_list]'s upload list what has to happen ahead of the
 * list's own commands: a copy of the dirty range of every staged store the
 * list uses, each buffer moved into COPY_DEST for the copy and into the state
 * it is used in after it; and every store whose state the list tracks put in
 * its use_state. [out_recorded] says whether there was anything; if not, the
 * upload list is left as it was and must not be submitted. */
static void aprend_cmd_record_uploads(
    aprend_command_list cmd_list,
    bool *out_recorded) {
	*out_recorded = false;
	std::vector<spudgpu_buffer_barrier> before;
	std::vector<spudgpu_buffer_barrier> after;
	for (aprend_buffer_store *store : cmd_list->staged_stores) {
		if (store->dirty_begin == store->dirty_end)
			continue;
		before.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_COMMON, SPUDGPU_RESOURCE_STATE_COPY_DEST});
		after.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_COPY_DEST, store->use_state});
	}
	// A store whose state the list's barriers track must start the list in
	// its use_state, which the copy above leaves it in. One with nothing to
	// copy is put there directly, so the list starts the same either way.
	for (const auto &tracked : cmd_list->buffer_states) {
		aprend_buffer_store *store = tracked.store;
		if (store->staging && store->dirty_begin != store->dirty_end)
			continue;
		before.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_COMMON, store->use_state});
	}
	if (before.empty())
		return;

	spudgpu_command_list upload = cmd_list->upload_list;
	spudgpu_begin_command_list(upload);
	spudgpu_cmd_pipeline_barrier(upload, before.data(), (uint32_t)before.size(), nullptr, 0);
	for (aprend_buffer_store *store : cmd_list->staged_stores) {
		if (store->dirty_begin == store->dirty_end)
			continue;
		spudgpu_cmd_copy_buffer(upload, store->staging, store->buffer, store->dirty_begin, store->dirty_begin, store->dirty_end - store->dirty_begin);
	}
	if (!after.empty())
		spudgpu_cmd_pipeline_barrier(upload, after.data(), (uint32_t)after.size(), nullptr, 0);
	spudgpu_end_command_list(upload);
	*out_recorded = true;
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

	if (SPUDFAIL(spudgpu_create_command_list(instance->cmd_allocator, &result->cmd_list))) {
		result->cmd_list = nullptr;
		goto failedattempt;
	}
	if (SPUDFAIL(spudgpu_create_command_list(instance->cmd_allocator, &result->upload_list))) {
		result->upload_list = nullptr;
		goto failedattempt;
	}

	return result;
failedattempt:
	result->~aprend_command_list_t();
	free(result);
	return nullptr;
}
void aprend_command_list_destroy(aprend_command_list cmd_list) {
	if (!cmd_list)
		return;
	// Released once the GPU has finished everything submitted so far.
	aprend_instance_retire(cmd_list->instance, cmd_list, &aprend_release_handle<aprend_command_list_t>);
}

void aprend_command_list_reset(aprend_command_list cmd_list) {
	if (!cmd_list)
		return;
	const size_t preserve_size = cmd_list->commands.size();
	cmd_list->commands.clear();
	cmd_list->commands.reserve(preserve_size);
	cmd_list->color_target_storage.clear();
	cmd_list->vertex_buffer_storage.clear();
	cmd_list->viewport_storage.clear();
	cmd_list->scissor_rect_storage.clear();
	cmd_list->push_constant_storage.clear();
	cmd_list->recording_error = false;
	cmd_list->layout_uses.clear();
	cmd_list->staged_stores.clear();
	cmd_list->buffer_states.clear();
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

	// The array commands below are copied the same way: the caller's array
	// only has to live for this call.
	if (cmd._type == APREND_COMMAND_SET_VERTEX_BUFFERS) {
		auto &p = cmd._params._set_vertex_buffers;
		if (p._vertex_buffer_count > 0 && !p._vertex_buffers) {
			printf("aprend: SET_VERTEX_BUFFERS with a NULL array (count %u)\n", p._vertex_buffer_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._start_slot > APREND_MAX_VERTEX_BINDINGS) {
			printf("aprend: SET_VERTEX_BUFFERS with _start_slot %u, above APREND_MAX_VERTEX_BINDINGS\n", p._start_slot);
			cmd_list->recording_error = true;
			return;
		}
		if (p._vertex_buffer_count > APREND_MAX_VERTEX_BINDINGS - p._start_slot) {
			printf("aprend: SET_VERTEX_BUFFERS of %u buffers from slot %u runs past the last vertex buffer slot\n", p._vertex_buffer_count,
			       p._start_slot);
			cmd_list->recording_error = true;
			return;
		}
		for (uint32_t i = 0; i < p._vertex_buffer_count; ++i) {
			if (!p._vertex_buffers[i]) {
				printf("aprend: SET_VERTEX_BUFFERS vertex buffer %u is NULL\n", i);
				cmd_list->recording_error = true;
				return;
			}
		}
		if (p._vertex_buffer_count > 0) {
			cmd_list->vertex_buffer_storage.emplace_back(p._vertex_buffers, p._vertex_buffers + p._vertex_buffer_count);
			p._vertex_buffers = cmd_list->vertex_buffer_storage.back().data();
		} else {
			p._vertex_buffers = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_VIEWPORTS) {
		auto &p = cmd._params._set_viewports;
		if (p._viewport_count > 0 && !p._viewports) {
			printf("aprend: SET_VIEWPORTS with a NULL array (count %u)\n", p._viewport_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._viewport_count > 0) {
			cmd_list->viewport_storage.emplace_back(p._viewports, p._viewports + p._viewport_count);
			p._viewports = cmd_list->viewport_storage.back().data();
		} else {
			p._viewports = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_SET_SCISSOR_RECTS) {
		auto &p = cmd._params._set_scissor_rects;
		if (p._scissor_rect_count > 0 && !p._scissor_rects) {
			printf("aprend: SET_SCISSOR_RECTS with a NULL array (count %u)\n", p._scissor_rect_count);
			cmd_list->recording_error = true;
			return;
		}
		if (p._scissor_rect_count > 0) {
			cmd_list->scissor_rect_storage.emplace_back(p._scissor_rects, p._scissor_rects + p._scissor_rect_count);
			p._scissor_rects = cmd_list->scissor_rect_storage.back().data();
		} else {
			p._scissor_rects = nullptr;
		}
	}

	if (cmd._type == APREND_COMMAND_DRAW_INDIRECT || cmd._type == APREND_COMMAND_DRAW_INDEXED_INDIRECT) {
		const auto &p = cmd._params._draw_indirect;
		// What one entry is depends on which of the two commands this is.
		const uint64_t entry_size = cmd._type == APREND_COMMAND_DRAW_INDIRECT ? sizeof(spudgpu_draw_indirect_args)
		                                                                       : sizeof(spudgpu_draw_indexed_indirect_args);
		if (!p._buffer && !p._set) {
			printf("aprend: indirect draw with neither a _buffer nor a _set\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._buffer && p._set) {
			printf("aprend: indirect draw with both a _buffer and a _set\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._set && p._set->instance != cmd_list->instance) {
			printf("aprend: indirect draw with a _set of another instance\n");
			cmd_list->recording_error = true;
			return;
		}
		// Every copy of a set has one size and one usage.
		const aprend_storage_buffer arguments = p._set ? p._set->buffers[0] : p._buffer;
		if (!(arguments->usage & APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS)) {
			printf("aprend: indirect draw with a buffer created without APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._draw_count == 0) {
			printf("aprend: indirect draw with a _draw_count of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset % 4 != 0) {
			printf("aprend: indirect draw with an _offset that is not a multiple of 4\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._stride % 4 != 0) {
			printf("aprend: indirect draw with _stride %u, not a multiple of 4\n", p._stride);
			cmd_list->recording_error = true;
			return;
		}
		if (p._stride < entry_size) {
			printf("aprend: indirect draw with _stride %u, smaller than the %u bytes of one entry\n", p._stride, (unsigned)entry_size);
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset > arguments->size) {
			printf("aprend: indirect draw with an _offset past the end of the buffer\n");
			cmd_list->recording_error = true;
			return;
		}
		// Where the last entry ends: every one before it is a whole stride.
		const uint64_t span = (uint64_t)(p._draw_count - 1) * p._stride + entry_size;
		if (span > arguments->size - p._offset) {
			printf("aprend: indirect draw of %u entries runs past the end of the buffer\n", p._draw_count);
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_DISPATCH_MESH) {
		const auto &p = cmd._params._dispatch_mesh;
		if (p._group_count_x == 0) {
			printf("aprend: DISPATCH_MESH with a _group_count_x of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_y == 0) {
			printf("aprend: DISPATCH_MESH with a _group_count_y of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_z == 0) {
			printf("aprend: DISPATCH_MESH with a _group_count_z of 0\n");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_DISPATCH) {
		const auto &p = cmd._params._dispatch;
		if (p._group_count_x == 0) {
			printf("aprend: DISPATCH with a _group_count_x of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_y == 0) {
			printf("aprend: DISPATCH with a _group_count_y of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._group_count_z == 0) {
			printf("aprend: DISPATCH with a _group_count_z of 0\n");
			cmd_list->recording_error = true;
			return;
		}
	}

	if (cmd._type == APREND_COMMAND_PUSH_CONSTANTS) {
		auto &p = cmd._params._push_constants;
		if (!p._data) {
			printf("aprend: PUSH_CONSTANTS with NULL _data\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._size == 0) {
			printf("aprend: PUSH_CONSTANTS with a _size of 0\n");
			cmd_list->recording_error = true;
			return;
		}
		if (p._offset % 4 != 0) {
			printf("aprend: PUSH_CONSTANTS with _offset %u, not a multiple of 4\n", p._offset);
			cmd_list->recording_error = true;
			return;
		}
		if (p._size % 4 != 0) {
			printf("aprend: PUSH_CONSTANTS with _size %u, not a multiple of 4\n", p._size);
			cmd_list->recording_error = true;
			return;
		}
		const uint8_t *bytes = (const uint8_t *)p._data;
		cmd_list->push_constant_storage.emplace_back(bytes, bytes + p._size);
		p._data = cmd_list->push_constant_storage.back().data();
	}

	cmd_list->commands.emplace_back(cmd);
}

/* Moves the image whose tracked layout is at [tracked] into [layout]. Unless
 * [skip_if_same], issued even when the layout already matches: back-to-back
 * passes on the same attachment (e.g. a LOAD after a previous pass's STORE)
 * still need the barrier's memory dependency. Two reads in a row don't, which
 * is what [skip_if_same] is for.
 *
 * Tracks the layout in [cmd_list]'s own layout_uses, never on the texture:
 * the texture's layout only changes once the list is actually submitted. */
static void aprend_cmd_transition_image(
    aprend_command_list cmd_list,
    spudgpu_image image,
    SPUDGPU_IMAGE_LAYOUT *tracked,
    SPUDGPU_IMAGE_LAYOUT layout,
    bool skip_if_same) {
	aprend_command_list_t::layout_use *use = nullptr;
	for (auto &u : cmd_list->layout_uses)
		if (u.image == image)
			use = &u;
	if (!use) {
		// First use in this list: start from the layout everything submitted
		// so far leaves it in.
		cmd_list->layout_uses.push_back({image, tracked, *tracked, *tracked});
		use = &cmd_list->layout_uses.back();
	}

	if (skip_if_same && use->final_layout == layout)
		return;
	spudgpu_cmd_image_barrier(cmd_list->cmd_list, image, use->final_layout, layout);
	use->final_layout = layout;
}
static void aprend_cmd_transition_texture(
    aprend_command_list cmd_list,
    aprend_texture2d tex,
    SPUDGPU_IMAGE_LAYOUT layout) {
	aprend_cmd_transition_image(cmd_list, tex->image, &tex->current_layout, layout, false);
}
/* Render targets are always 2D views (see aprend_begin_rendering_valid). */
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

/* A texture one pass's binding sets read or write, and the layout that takes. */
struct aprend_pass_image {
	spudgpu_image image;
	SPUDGPU_IMAGE_LAYOUT *tracked;
	SPUDGPU_IMAGE_LAYOUT layout;
};

/* Adds the storage buffers [set] holds for frame [frame] to [stores], each
 * once. */
static void aprend_storage_stores_add_set(
    std::vector<aprend_buffer_store *> &stores,
    aprend_binding_set set,
    uint32_t frame) {
	const uint32_t frame_slot          = aprend_binding_set_frame_slot(set, frame);
	aprend_buffer_store **frame_stores = set->storage_stores + (size_t)frame_slot * set->storage_store_capacity;
	for (uint32_t i = 0; i < set->storage_store_counts[frame_slot]; ++i) {
		bool known = false;
		for (aprend_buffer_store *store : stores)
			if (store == frame_stores[i])
				known = true;
		if (!known)
			stores.push_back(frame_stores[i]);
	}
}

/* Adds [set]'s textures to [images]. False if one is already there in another
 * layout: sampled through one slot and a storage image through another. */
static bool aprend_pass_images_add_set(
    std::vector<aprend_pass_image> &images,
    aprend_binding_set set) {
	for (uint32_t i = 0; i < set->image_use_count; ++i) {
		const auto &use               = set->image_uses[i];
		SPUDGPU_IMAGE_LAYOUT *tracked = nullptr;
		spudgpu_image image           = aprend_texture_view_image(use.view, &tracked);

		bool known = false;
		for (const aprend_pass_image &pass_image : images) {
			if (pass_image.image != image)
				continue;
			if (pass_image.layout != use.layout) {
				printf("aprend: a texture is in a sampled slot and a storage image slot of the binding sets of one pass or dispatch\n");
				return false;
			}
			known = true;
		}
		if (!known)
			images.push_back({image, tracked, use.layout});
	}
	return true;
}

/* The argument buffer of the indirect draw [command], for a list compiled in
 * frame [frame]: its _buffer, or that frame's copy of its _set.
 * aprend_send_command refused a command with neither or both. */
static aprend_storage_buffer aprend_cmd_indirect_arguments(
    const APREND_COMMAND &command,
    uint32_t frame) {
	const auto &p = command._params._draw_indirect;
	return p._set ? p._set->buffers[frame] : p._buffer;
}

/* Moves a multi-state [store] into [state], tracking it in [cmd_list]'s
 * buffer_states. The first time the list uses a store it is in its
 * use_state, which aprend_command_list_submit sees to. Recorded outside any
 * pass. */
static void aprend_cmd_transition_store(
    aprend_command_list cmd_list,
    aprend_buffer_store *store,
    SPUDGPU_RESOURCE_STATE state) {
	aprend_command_list_t::buffer_state *tracked = nullptr;
	for (auto &known : cmd_list->buffer_states)
		if (known.store == store)
			tracked = &known;
	if (!tracked) {
		cmd_list->buffer_states.push_back({store, store->use_state});
		tracked = &cmd_list->buffer_states.back();
	}
	if (tracked->state == state)
		return;
	spudgpu_buffer_barrier barrier{store->buffer, tracked->state, state};
	spudgpu_cmd_pipeline_barrier(cmd_list->cmd_list, &barrier, 1, nullptr, 0);
	tracked->state = state;
}

/* Makes what a pass's or a dispatch's binding sets hold ready to be read:
 * each texture in [images] is moved into the layout its slot reads it in,
 * and every storage image among them and every storage buffer in [stores] is
 * ordered after the storage writes made before this point, since a shader of
 * an earlier dispatch or draw may have written it. Reads after reads need no
 * barrier, so a sampled texture already in its layout gets none.
 *
 * The ordering is a SpudGPU pipeline barrier from UNORDERED_ACCESS to
 * UNORDERED_ACCESS (spudgpu.h): recording it is the caller's, and Aprend
 * can't know which of these a shader wrote, so it records it for all of
 * them. Recorded outside any pass. */
static void aprend_cmd_ready_set_resources(
    aprend_command_list cmd_list,
    const std::vector<aprend_pass_image> &images,
    const std::vector<aprend_buffer_store *> &stores) {
	std::vector<spudgpu_image_barrier> image_barriers;
	for (const aprend_pass_image &pass_image : images) {
		aprend_cmd_transition_image(cmd_list, pass_image.image, pass_image.tracked, pass_image.layout, true);
		// GENERAL is the layout of a storage image slot.
		if (pass_image.layout == SPUDGPU_IMAGE_LAYOUT_GENERAL)
			image_barriers.push_back({pass_image.image, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS});
	}
	std::vector<spudgpu_buffer_barrier> buffer_barriers;
	for (aprend_buffer_store *store : stores) {
		// One that was last an indirect argument buffer comes back to the
		// state a storage slot uses it in.
		if (store->multi_state)
			aprend_cmd_transition_store(cmd_list, store, store->use_state);
		buffer_barriers.push_back({store->buffer, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS});
	}
	if (image_barriers.empty() && buffer_barriers.empty())
		return;
	spudgpu_cmd_pipeline_barrier(
	    cmd_list->cmd_list, buffer_barriers.data(), (uint32_t)buffer_barriers.size(), image_barriers.data(), (uint32_t)image_barriers.size());
}

/* Before the BEGIN_RENDERING at [begin_index] opens its pass: moves every
 * texture of the pass's binding sets into the layout its slot reads it in,
 * since a barrier can't be issued once the pass is open. The pass's sets are
 * the ones a draw in it reads: the pass is walked as the compile will walk
 * it, from the sets in [slots] now, and at each draw the set in every slot
 * the pipeline bound then declares is taken. A set that is only left in a
 * slot, with no draw reading that slot, isn't the pass's.
 * Assumes aprend_begin_rendering_valid passed. */
static bool aprend_cmd_prepare_pass_images(
    aprend_command_list cmd_list,
    size_t begin_index,
    const aprend_binding_set *slots) {
	std::vector<aprend_pass_image> images;
	std::vector<aprend_buffer_store *> stores;
	/* The argument buffers of the pass's indirect draws, each once. */
	std::vector<aprend_buffer_store *> argument_stores;
	aprend_binding_set pass_slots[APREND_MAX_BINDING_LAYOUTS];
	for (uint32_t slot = 0; slot < APREND_MAX_BINDING_LAYOUTS; ++slot)
		pass_slots[slot] = slots[slot];
	aprend_graphics_pipeline pass_pipeline = nullptr;
	bool pass_ended                        = false;
	for (size_t i = begin_index + 1; i < cmd_list->commands.size() && !pass_ended; ++i) {
		const APREND_COMMAND &command = cmd_list->commands[i];
		switch (command._type) {
		case APREND_COMMAND_END_RENDERING:
			pass_ended = true;
			break;
		case APREND_COMMAND_SET_SHADER_PIPELINE:
			// A NULL pipeline is refused when the command itself is reached.
			pass_pipeline = command._params._set_shader_pipeline._pipeline;
			break;
		case APREND_COMMAND_SET_BINDING_SET:
			// As is a slot out of range. A NULL set is refused there too, and
			// until then leaves the slot empty here.
			if (command._params._set_binding_set._slot < APREND_MAX_BINDING_LAYOUTS)
				pass_slots[command._params._set_binding_set._slot] = command._params._set_binding_set._set;
			break;
		case APREND_COMMAND_DRAW:
		case APREND_COMMAND_DRAW_INDEXED:
		case APREND_COMMAND_DRAW_INSTANCED:
		case APREND_COMMAND_DRAW_INSTANCED_INDEXED:
		case APREND_COMMAND_DRAW_INDIRECT:
		case APREND_COMMAND_DRAW_INDEXED_INDIRECT:
		case APREND_COMMAND_DISPATCH_MESH:
			if (command._type == APREND_COMMAND_DRAW_INDIRECT || command._type == APREND_COMMAND_DRAW_INDEXED_INDIRECT) {
				aprend_buffer_store *arguments = &aprend_cmd_indirect_arguments(command, cmd_list->compiled_frame)->store;
				bool known                     = false;
				for (aprend_buffer_store *store : argument_stores)
					if (store == arguments)
						known = true;
				if (!known)
					argument_stores.push_back(arguments);
			}
			// A draw with no pipeline, or with an empty slot, fails the
			// compile when it is reached.
			if (!pass_pipeline)
				break;
			for (uint32_t slot = 0; slot < pass_pipeline->desc._binding_layout_count; ++slot) {
				if (!pass_slots[slot])
					continue;
				if (!aprend_pass_images_add_set(images, pass_slots[slot]))
					return false;
				aprend_storage_stores_add_set(stores, pass_slots[slot], cmd_list->compiled_frame);
			}
			break;
		default:
			break;
		}
	}

	// A texture can't be read through a set and rendered to at once.
	const auto &p = cmd_list->commands[begin_index]._params._begin_rendering;
	for (const aprend_pass_image &pass_image : images) {
		for (uint32_t i = 0; i < p._color_target_count; ++i) {
			if (p._color_targets[i].view->texture._t2d->image == pass_image.image) {
				printf("aprend: color target %u of a pass is also in one of the pass's binding sets\n", i);
				return false;
			}
		}
		if (p._depth_target.view && p._depth_target.view->texture._t2d->image == pass_image.image) {
			printf("aprend: the depth target of a pass is also in one of the pass's binding sets\n");
			return false;
		}
	}

	// A buffer is in one state for the whole pass: it can't be read as draw
	// arguments and be in a storage slot the pass's draws read.
	for (aprend_buffer_store *arguments : argument_stores) {
		for (aprend_buffer_store *store : stores) {
			if (store == arguments) {
				printf("aprend: the argument buffer of an indirect draw is also in one of the pass's binding sets\n");
				return false;
			}
		}
	}

	aprend_cmd_ready_set_resources(cmd_list, images, stores);
	// After the sets, so a buffer a set brought back to a storage slot's
	// state in an earlier pass ends this one's preparation as arguments.
	for (aprend_buffer_store *arguments : argument_stores)
		aprend_cmd_transition_store(cmd_list, arguments, SPUDGPU_RESOURCE_STATE_INDIRECT_ARGUMENT);
	return true;
}

/* Before a dispatch: checks the set in each slot [pipeline] declares, makes
 * what those sets hold ready (aprend_cmd_ready_set_resources) and binds
 * each set that isn't the one already bound there. There is no pass to move
 * the textures ahead of, so this is done right at the dispatch. False if a
 * slot holds no set or one of another layout, or a texture is in a sampled
 * slot and a storage image slot of these sets. */
static bool aprend_cmd_prepare_dispatch(
    aprend_command_list cmd_list,
    aprend_compute_pipeline pipeline,
    const aprend_binding_set *slots,
    aprend_binding_set *applied) {
	std::vector<aprend_pass_image> images;
	std::vector<aprend_buffer_store *> stores;
	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		aprend_binding_set set = slots[slot];
		if (!set) {
			printf("aprend: dispatch with a pipeline that declares set %u, but no SET_BINDING_SET has put a set there\n", slot);
			return false;
		}
		if (set->layout != pipeline->desc._binding_layouts[slot]) {
			printf("aprend: dispatch with a set at slot %u that isn't of the layout the pipeline declares there\n", slot);
			return false;
		}
		if (!aprend_pass_images_add_set(images, set))
			return false;
		aprend_storage_stores_add_set(stores, set, cmd_list->compiled_frame);
	}
	aprend_cmd_ready_set_resources(cmd_list, images, stores);

	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		aprend_binding_set set = slots[slot];
		if (applied[slot] == set)
			continue;
		spudgpu_cmd_bind_descriptor_sets_compute(
		    cmd_list->cmd_list, pipeline->pipeline, slot, &set->sets[aprend_binding_set_frame_slot(set, cmd_list->compiled_frame)], 1);
		applied[slot] = set;
	}
	return true;
}

/* Before a draw: binds the set in each slot [pipeline] declares, where it
 * isn't the one already bound there. False if a slot holds no set, or one of
 * another layout. */
/* Before a draw: every vertex buffer slot [pipeline] reads holds a buffer of
 * the stride its binding has. */
static bool aprend_cmd_vertex_buffers_valid(
    aprend_graphics_pipeline pipeline,
    const aprend_vertex_buffer *slots) {
	for (uint32_t slot = 0; slot < pipeline->desc._vertex_binding_count; ++slot) {
		aprend_vertex_buffer buffer = slots[slot];
		if (!buffer) {
			printf("aprend: draw with a pipeline that reads vertex buffer slot %u, but no SET_VERTEX_BUFFERS has put a buffer there\n", slot);
			return false;
		}
		if (buffer->vertex_stride != pipeline->vertex_strides[slot]) {
			printf("aprend: draw with a vertex buffer of stride %u at slot %u, where the pipeline's binding has stride %u\n",
			       buffer->vertex_stride, slot, pipeline->vertex_strides[slot]);
			return false;
		}
	}
	return true;
}

static bool aprend_cmd_apply_binding_sets(
    spudgpu_command_list cmd,
    aprend_graphics_pipeline pipeline,
    const aprend_binding_set *slots,
    aprend_binding_set *applied,
    uint32_t frame) {
	for (uint32_t slot = 0; slot < pipeline->desc._binding_layout_count; ++slot) {
		aprend_binding_set set = slots[slot];
		if (!set) {
			printf("aprend: draw with a pipeline that declares set %u, but no SET_BINDING_SET has put a set there\n", slot);
			return false;
		}
		if (set->layout != pipeline->desc._binding_layouts[slot]) {
			printf("aprend: draw with a set at slot %u that isn't of the layout the pipeline declares there\n", slot);
			return false;
		}
		if (applied[slot] == set)
			continue;
		spudgpu_cmd_bind_descriptor_sets(cmd, pipeline->pipeline, slot, &set->sets[aprend_binding_set_frame_slot(set, frame)], 1);
		applied[slot] = set;
	}
	return true;
}

bool aprend_command_list_compile(aprend_command_list cmd_list) {
	if (!cmd_list)
		return false;
	if (!cmd_list->cmd_list)
		return false;
	cmd_list->compiled = false;
	cmd_list->layout_uses.clear();
	cmd_list->staged_stores.clear();
	cmd_list->buffer_states.clear();
	cmd_list->present_swap_chain = nullptr;
	// Every buffer set this compile reads is read at this frame's copy.
	cmd_list->compiled_frame  = cmd_list->instance->frame_index;
	cmd_list->uses_frame_sets = false;
	if (cmd_list->recording_error)
		return false;

	spudgpu_command_list cmd = cmd_list->cmd_list;
	spudgpu_begin_command_list(cmd);

	/* Whether a BEGIN_RENDERING pass is currently open. */
	bool in_pass = false;
	bool ok      = true;
	/* The pipeline bound in the open pass; none until the pass sets one. */
	aprend_graphics_pipeline bound_pipeline = nullptr;
	/* The buffer SET_VERTEX_BUFFERS last put in each vertex buffer slot. */
	aprend_vertex_buffer slot_vertex_buffers[APREND_MAX_VERTEX_BINDINGS]{};
	/* The buffer SET_INDEX_BUFFER last set; none until one is. */
	aprend_index_buffer bound_index_buffer = nullptr;
	/* The set SET_BINDING_SET last put in each slot, and the one actually
	 * bound there for the bound pipeline in the open pass. A draw binds
	 * whatever differs. */
	aprend_binding_set slot_sets[APREND_MAX_BINDING_LAYOUTS]{};
	aprend_binding_set applied_sets[APREND_MAX_BINDING_LAYOUTS]{};
	/* The compute pipeline set outside a pass, until the next pass or
	 * present, and the set bound for it in each slot. */
	aprend_compute_pipeline bound_compute_pipeline = nullptr;
	aprend_binding_set applied_compute_sets[APREND_MAX_BINDING_LAYOUTS]{};

	std::vector<spudgpu_buffer_view> vertex_buffer_views;

	for (size_t command_index = 0; command_index < cmd_list->commands.size(); ++command_index) {
		const APREND_COMMAND &command = cmd_list->commands[command_index];
		switch (command._type) {
		case APREND_COMMAND_DRAW:
		case APREND_COMMAND_DRAW_INDEXED:
		case APREND_COMMAND_DRAW_INSTANCED:
		case APREND_COMMAND_DRAW_INSTANCED_INDEXED:
		case APREND_COMMAND_DRAW_INDIRECT:
		case APREND_COMMAND_DRAW_INDEXED_INDIRECT:
		case APREND_COMMAND_DISPATCH_MESH:
			if (!in_pass) {
				printf("aprend: draw command recorded outside a rendering pass\n");
				ok = false;
			} else if (!bound_pipeline) {
				printf("aprend: draw command in a pass that hasn't set a pipeline\n");
				ok = false;
			} else if (bound_pipeline->is_mesh && command._type != APREND_COMMAND_DISPATCH_MESH) {
				printf("aprend: draw command with a mesh shader pipeline, which is run with DISPATCH_MESH\n");
				ok = false;
			} else if (!bound_pipeline->is_mesh && command._type == APREND_COMMAND_DISPATCH_MESH) {
				printf("aprend: DISPATCH_MESH with a pipeline that has no mesh shader\n");
				ok = false;
			} else if (!aprend_cmd_vertex_buffers_valid(bound_pipeline, slot_vertex_buffers)) {
				ok = false;
			} else if (!bound_index_buffer && (command._type == APREND_COMMAND_DRAW_INDEXED ||
			                                   command._type == APREND_COMMAND_DRAW_INSTANCED_INDEXED ||
			                                   command._type == APREND_COMMAND_DRAW_INDEXED_INDIRECT)) {
				printf("aprend: indexed draw with no SET_INDEX_BUFFER before it\n");
				ok = false;
			} else {
				ok = aprend_cmd_apply_binding_sets(cmd, bound_pipeline, slot_sets, applied_sets, cmd_list->compiled_frame);
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
			for (uint32_t i = 0; i < p._vertex_buffer_count; ++i) {
				vertex_buffer_views.push_back(p._vertex_buffers[i]->buffer_view);
				// aprend_send_command refused a command that runs past the last slot.
				slot_vertex_buffers[p._start_slot + i] = p._vertex_buffers[i];
				aprend_cmd_use_store(cmd_list, &p._vertex_buffers[i]->store);
			}
			spudgpu_cmd_set_vertex_buffers(cmd, p._start_slot, p._vertex_buffer_count, vertex_buffer_views.data());
			break;
		}
		case APREND_COMMAND_SET_INDEX_BUFFER: {
			const auto &p = command._params._set_index_buffer;
			if (!p._index_buffer) {
				printf("aprend: SET_INDEX_BUFFER with a NULL buffer\n");
				ok = false;
				break;
			}
			spudgpu_cmd_set_index_buffer(cmd, p._index_buffer->buffer_view);
			bound_index_buffer = p._index_buffer;
			aprend_cmd_use_store(cmd_list, &p._index_buffer->store);
			break;
		}
		case APREND_COMMAND_SET_SHADER_PIPELINE: {
			const auto &p = command._params._set_shader_pipeline;
			if (!p._pipeline) {
				printf("aprend: SET_SHADER_PIPELINE with a NULL pipeline\n");
				ok = false;
				break;
			}
			if (!in_pass) {
				printf("aprend: SET_SHADER_PIPELINE outside a rendering pass\n");
				ok = false;
				break;
			}
			spudgpu_cmd_bind_pipeline(cmd, p._pipeline->pipeline);
			bound_pipeline = p._pipeline;
			// Not guaranteed to survive a pipeline change on every backend:
			// the next draw binds them again.
			for (aprend_binding_set &applied : applied_sets)
				applied = nullptr;
			break;
		}
		case APREND_COMMAND_SET_BINDING_SET: {
			const auto &p = command._params._set_binding_set;
			if (!p._set) {
				printf("aprend: SET_BINDING_SET with a NULL set\n");
				ok = false;
				break;
			}
			if (p._slot >= APREND_MAX_BINDING_LAYOUTS) {
				printf("aprend: SET_BINDING_SET with _slot %u, at or above APREND_MAX_BINDING_LAYOUTS\n", p._slot);
				ok = false;
				break;
			}
			slot_sets[p._slot] = p._set;
			if (p._set->frame_set_count > 1)
				cmd_list->uses_frame_sets = true;
			// The stores of the descriptor set this frame binds. Taken
			// whether or not a draw reads the slot: copying a buffer nothing
			// reads yet is harmless.
			{
				const uint32_t frame_slot          = aprend_binding_set_frame_slot(p._set, cmd_list->compiled_frame);
				aprend_buffer_store **frame_stores = p._set->storage_stores + (size_t)frame_slot * p._set->storage_store_capacity;
				for (uint32_t i = 0; i < p._set->storage_store_counts[frame_slot]; ++i)
					aprend_cmd_use_store(cmd_list, frame_stores[i]);
			}
			break;
		}
		case APREND_COMMAND_PUSH_CONSTANTS: {
			const auto &p = command._params._push_constants;
			if (!in_pass) {
				// Outside a pass the block written is the compute pipeline's.
				if (!bound_compute_pipeline) {
					printf("aprend: PUSH_CONSTANTS outside a pass with no compute pipeline set\n");
					ok = false;
					break;
				}
				if (p._offset > bound_compute_pipeline->push_constant_end) {
					printf("aprend: PUSH_CONSTANTS at offset %u, past the compute pipeline's push constant ranges (%u bytes)\n", p._offset,
					       bound_compute_pipeline->push_constant_end);
					ok = false;
					break;
				}
				if (p._size > bound_compute_pipeline->push_constant_end - p._offset) {
					printf("aprend: PUSH_CONSTANTS of %u bytes at offset %u runs past the compute pipeline's push constant ranges (%u bytes)\n",
					       p._size, p._offset, bound_compute_pipeline->push_constant_end);
					ok = false;
					break;
				}
				spudgpu_cmd_push_constants_compute(cmd, bound_compute_pipeline->pipeline, p._offset, p._size, p._data);
				break;
			}
			if (!bound_pipeline) {
				printf("aprend: PUSH_CONSTANTS in a pass that hasn't set a pipeline\n");
				ok = false;
				break;
			}
			if (p._offset > bound_pipeline->push_constant_end) {
				printf("aprend: PUSH_CONSTANTS at offset %u, past the pipeline's push constant ranges (%u bytes)\n", p._offset,
				       bound_pipeline->push_constant_end);
				ok = false;
				break;
			}
			if (p._size > bound_pipeline->push_constant_end - p._offset) {
				printf("aprend: PUSH_CONSTANTS of %u bytes at offset %u runs past the pipeline's push constant ranges (%u bytes)\n", p._size,
				       p._offset, bound_pipeline->push_constant_end);
				ok = false;
				break;
			}
			spudgpu_cmd_push_constants(cmd, bound_pipeline->pipeline, p._offset, p._size, p._data);
			break;
		}
		case APREND_COMMAND_SET_COMPUTE_PIPELINE: {
			const auto &p = command._params._set_compute_pipeline;
			if (!p._pipeline) {
				printf("aprend: SET_COMPUTE_PIPELINE with a NULL pipeline\n");
				ok = false;
				break;
			}
			if (in_pass) {
				printf("aprend: SET_COMPUTE_PIPELINE inside a rendering pass\n");
				ok = false;
				break;
			}
			spudgpu_cmd_bind_compute_pipeline(cmd, p._pipeline->pipeline);
			bound_compute_pipeline = p._pipeline;
			// Not guaranteed to survive a pipeline change on every backend:
			// the next dispatch binds them again.
			for (aprend_binding_set &applied : applied_compute_sets)
				applied = nullptr;
			break;
		}
		case APREND_COMMAND_DISPATCH: {
			const auto &p = command._params._dispatch;
			if (in_pass) {
				printf("aprend: DISPATCH inside a rendering pass\n");
				ok = false;
				break;
			}
			if (!bound_compute_pipeline) {
				printf("aprend: DISPATCH with no compute pipeline set since the last pass or present\n");
				ok = false;
				break;
			}
			if (!aprend_cmd_prepare_dispatch(cmd_list, bound_compute_pipeline, slot_sets, applied_compute_sets)) {
				ok = false;
				break;
			}
			spudgpu_cmd_dispatch(cmd, p._group_count_x, p._group_count_y, p._group_count_z);
			break;
		}
		case APREND_COMMAND_PRESENT_TEXTURE: {
			if (in_pass) {
				printf("aprend: PRESENT_TEXTURE inside a rendering pass\n");
				ok = false;
				break;
			}
			ok = aprend_cmd_present_texture(cmd_list, command);
			// The copy ends compute recording on a backend that keeps it
			// apart from copies: a compute pipeline is set again after it.
			bound_compute_pipeline = nullptr;
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
		case APREND_COMMAND_DRAW_INDIRECT: {
			const auto &p                   = command._params._draw_indirect;
			aprend_storage_buffer arguments = aprend_cmd_indirect_arguments(command, cmd_list->compiled_frame);
			if (p._set)
				cmd_list->uses_frame_sets = true;
			// What the CPU wrote to it is copied at submit if it is staged.
			// It was moved to the indirect argument state before the pass.
			aprend_cmd_use_store(cmd_list, &arguments->store);
			spudgpu_cmd_draw_indirect(cmd, arguments->store.buffer, p._offset, p._draw_count, p._stride);
			break;
		}
		case APREND_COMMAND_DRAW_INDEXED_INDIRECT: {
			const auto &p                   = command._params._draw_indirect;
			aprend_storage_buffer arguments = aprend_cmd_indirect_arguments(command, cmd_list->compiled_frame);
			if (p._set)
				cmd_list->uses_frame_sets = true;
			aprend_cmd_use_store(cmd_list, &arguments->store);
			spudgpu_cmd_draw_indexed_indirect(cmd, arguments->store.buffer, p._offset, p._draw_count, p._stride);
			break;
		}
		case APREND_COMMAND_DISPATCH_MESH: {
#if SPUDGPU_EXT_MESH_SHADING
			const auto &p = command._params._dispatch_mesh;
			spudgpu_cmd_dispatch_mesh(cmd, p._group_count_x, p._group_count_y, p._group_count_z);
#else
			// No mesh shader pipeline can exist in such a build, so the
			// pipeline check above has already refused this.
			ok = false;
#endif
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
			if (!aprend_cmd_prepare_pass_images(cmd_list, command_index, slot_sets)) {
				ok = false;
				break;
			}
			aprend_cmd_begin_rendering(cmd_list, command);
			in_pass = true;
			// A pipeline is bound inside a pass and doesn't outlive it, and
			// bound sets aren't guaranteed to carry over from one pass to the
			// next either.
			bound_pipeline = nullptr;
			for (aprend_binding_set &applied : applied_sets)
				applied = nullptr;
			// Nor does a compute pipeline outlive the pass that follows it.
			bound_compute_pipeline = nullptr;
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
		cmd_list->staged_stores.clear();
		cmd_list->buffer_states.clear();
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
		if (*use.tracked != use.initial_layout) {
			printf("aprend: submit refused: a texture's layout changed since this list was compiled; recompile it\n");
			return false;
		}
	}

	// A list that binds a per-frame set reads the copies of the frame it
	// was compiled in; in any other frame those are another frame's.
	if (cmd_list->uses_frame_sets && cmd_list->instance->frame_index != cmd_list->compiled_frame) {
		printf("aprend: submit refused: the list reads buffer sets and was compiled in frame %u, and the frame is now %u; recompile it\n",
		       cmd_list->compiled_frame, cmd_list->instance->frame_index);
		return false;
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

	// Whatever was written to the list's staged buffers since they were last
	// copied goes across first, in the same submission, so the list's own
	// commands read what the caller last wrote.
	bool has_uploads = false;
	aprend_cmd_record_uploads(cmd_list, &has_uploads);

	spudgpu_command_list lists[2];
	uint32_t list_count = 0;
	if (has_uploads)
		lists[list_count++] = cmd_list->upload_list;
	lists[list_count++] = cmd_list->cmd_list;

	// Every submission signals the instance's frame fence to the next
	// serial, so aprend_instance_next_frame can wait for the last one of a
	// frame without being told which that was. With a swap chain the
	// submission also waits for the acquire to finish and signals
	// presentation.
	if (SPUDFAIL(aprend_instance_submit(cmd_list->instance, queue, lists, list_count, sc ? sc->swap_chain : nullptr, nullptr)))
		return false;
	if (sc)
		sc->submitted = true;

	// Copied only now that the submission is in: a failed one leaves the
	// ranges dirty for the next.
	for (aprend_buffer_store *store : cmd_list->staged_stores) {
		store->dirty_begin = 0;
		store->dirty_end   = 0;
	}

	for (const auto &use : cmd_list->layout_uses)
		*use.tracked = use.final_layout;
	return true;
}
}
