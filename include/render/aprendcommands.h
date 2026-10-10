
#ifndef APREND_COMMANDS_H
#define APREND_COMMANDS_H

#include "aprendcontext.h"
#include "aprendimages.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_vertex_buffer_t *aprend_vertex_buffer;
typedef struct aprend_index_buffer_t *aprend_index_buffer;
typedef struct aprend_graphics_pipeline_t *aprend_graphics_pipeline;
typedef struct aprend_binding_set_t *aprend_binding_set;
typedef struct aprend_swap_chain_t *aprend_swap_chain;

/* Upper bound on color targets in one BEGIN_RENDERING pass. */
#define APREND_MAX_COLOR_TARGETS SPUDGPU_MAX_COLOR_ATTACHMENTS

/* aprend_color_target / aprend_depth_target (aprendimages.h) describe how one
 * pass uses each attachment view. Zero-initialized ops mean LOAD + STORE. */

typedef uint32_t APREND_RENDERING_FLAGS;
enum {
	APREND_RENDERING_FLAG_NONE = 0,
	/* A bundle will be executed inside this pass (SpudGPU's
	 * spudgpu_rendering_begin_desc::will_execute_bundles). The caller always
	 * knows this up front, so it is never inferred. */
	APREND_RENDERING_FLAG_EXECUTES_BUNDLES = 1 << 0,
};

typedef uint32_t APREND_COMMAND_TYPE;

enum {
	APREND_COMMAND_NONE                   = 0,
	/* Puts _vertex_buffers[i] at vertex buffer slot _start_slot + i. Slot N
	 * is what a pipeline's _vertex_bindings[N] reads (aprendpipeline.h), per
	 * vertex or per instance as that binding says. At each draw, every slot
	 * the bound pipeline reads must hold a buffer whose stride is the
	 * binding's. */
	APREND_COMMAND_SET_VERTEX_BUFFERS     = 1,
	APREND_COMMAND_SET_INDEX_BUFFER       = 2,
	/* Inside a pass only, and it lasts until the pass ends: each pass sets
	 * its own pipeline before it draws. */
	APREND_COMMAND_SET_SHADER_PIPELINE    = 3,
	APREND_COMMAND_DRAW                   = 4,
	APREND_COMMAND_DRAW_INDEXED           = 5,
	APREND_COMMAND_DRAW_INSTANCED         = 6,
	APREND_COMMAND_DRAW_INSTANCED_INDEXED = 7,
	APREND_COMMAND_SET_VIEWPORTS          = 8,
	APREND_COMMAND_SET_SCISSOR_RECTS      = 9,
	/* Pass scope. Draws are only valid inside a pass; BEGIN_RENDERING can't
	 * be nested and must be closed by END_RENDERING before the list is
	 * compiled. aprend_command_list_compile fails on any violation. */
	APREND_COMMAND_BEGIN_RENDERING        = 10,
	APREND_COMMAND_END_RENDERING          = 11,
	/* Puts an aprend_binding_set (aprendpipeline.h) at set slot _slot for the
	 * following draws, until another is put there. It stays through
	 * SET_SHADER_PIPELINE and from one pass to the next, so it may be sent
	 * before or after the pipeline, inside a pass or outside. At each draw,
	 * every slot the bound pipeline declares must hold a set created from the
	 * layout the pipeline declares there.
	 *
	 * Before a pass begins, every texture in a set that a draw of the pass
	 * reads (one in a slot the pipeline bound at that draw declares) is
	 * moved into the layout its binding reads it in. A set left in a slot no
	 * draw of the pass reads is not touched. Such a texture can't also be a target of that pass, nor be
	 * in a sampled slot and a storage image slot in the same pass. */
	APREND_COMMAND_SET_BINDING_SET        = 12,
	/* Copies _source into _swap_chain's acquired back buffer (aprendswapchain.h)
	 * and leaves it ready to present. Outside any pass; at most one per list.
	 * Where the sizes differ, only the overlapping top-left region is copied.
	 * The back buffer must have been acquired before compiling. */
	APREND_COMMAND_PRESENT_TEXTURE        = 13,
	/* Writes _size bytes at byte _offset of the bound pipeline's push
	 * constant block, for the following draws. After SET_SHADER_PIPELINE, in
	 * the same pass: the bytes are written for that pipeline and are not
	 * carried to the next one set. _offset and _size are multiples of 4,
	 * _size is not 0, and the bytes must lie inside the ranges the pipeline
	 * declares (_push_constant_ranges). What a draw reads from a byte no
	 * PUSH_CONSTANTS has written since its pipeline was set is undefined. */
	APREND_COMMAND_PUSH_CONSTANTS         = 14,
};

typedef struct APREND_COMMAND {
	APREND_COMMAND_TYPE _type;
	union {
		/* Every array a command points at (_vertex_buffers, _viewports,
		 * _scissor_rects, _color_targets, the _data of PUSH_CONSTANTS) is
		 * copied into the command list by aprend_send_command, so the
		 * caller's array only has to live for that call. What the entries refer to - buffers, views - must stay
		 * alive until the list's submission has completed. */
		struct {
			aprend_vertex_buffer *_vertex_buffers;
			uint32_t _vertex_buffer_count;
			uint32_t _start_slot;
		} _set_vertex_buffers;
		struct {
			aprend_index_buffer _index_buffer;
		} _set_index_buffer;
		struct {
			aprend_graphics_pipeline _pipeline;
		} _set_shader_pipeline;
		struct {
			uint32_t _vertex_count;
			uint32_t _start_vertex_location;
		} _draw;
		struct {
			uint32_t _index_count;
			uint32_t _start_index_location;
			int32_t _base_vertex_location;
		} _draw_indexed;
		struct {
			uint32_t _vertex_count_per_instance;
			uint32_t _instance_count;
			uint32_t _start_vertex_location;
			uint32_t _start_instance_location;
		} _draw_instanced;
		struct {
			uint32_t _index_count_per_instance;
			uint32_t _instance_count;
			uint32_t _start_index_location;
			int32_t _base_vertex_location;
			uint32_t _start_instance_location;
		} _draw_indexed_instanced;
		struct {
			const SPUDGPU_VIEWPORT *_viewports;
			uint32_t _first_viewport;
			uint32_t _viewport_count;
		} _set_viewports;
        struct {
			const SPUDGPU_SCISSOR_RECT *_scissor_rects;
			uint32_t _first_scissor_rect;
			uint32_t _scissor_rect_count;
		} _set_scissor_rects;
		struct {
			const aprend_color_target *_color_targets;
			uint32_t _color_target_count;
			aprend_depth_target _depth_target;
			/* Render area. _width/_height of 0 means the full size of the
			 * first color target (or the depth target if there's none). */
			int32_t _x, _y;
			uint32_t _width, _height;
			APREND_RENDERING_FLAGS _flags;
		} _begin_rendering;
		struct {
			uint32_t _slot;
			aprend_binding_set _set;
		} _set_binding_set;
		struct {
			aprend_texture2d _source;
			aprend_swap_chain _swap_chain;
		} _present_texture;
		struct {
			const void *_data;
			uint32_t _offset;
			uint32_t _size;
		} _push_constants;
	} _params;
} APREND_COMMAND;

typedef struct aprend_command_list_t *aprend_command_list;

aprend_command_list aprend_command_list_create(aprend_instance instance);
void aprend_command_list_destroy(aprend_command_list cmd_list);

void aprend_command_list_reset(aprend_command_list cmd_list);
/* A malformed command isn't recorded and makes the next
 * aprend_command_list_compile fail, until aprend_command_list_reset:
 * a BEGIN_RENDERING with more than APREND_MAX_COLOR_TARGETS color targets;
 * a NULL array with a nonzero count in BEGIN_RENDERING, SET_VERTEX_BUFFERS,
 * SET_VIEWPORTS or SET_SCISSOR_RECTS; a NULL entry in _vertex_buffers; a
 * SET_VERTEX_BUFFERS that runs past slot APREND_MAX_VERTEX_BINDINGS - 1; a
 * PUSH_CONSTANTS with NULL _data, a _size of 0, or an _offset or _size that
 * is not a multiple of 4. */
void aprend_send_command(
    aprend_command_list cmd_list,
    APREND_COMMAND cmd);

/* Translates every command recorded since the last aprend_command_list_reset
 * into the underlying spudgpu_command_list and closes it for submission
 * (spudgpu_begin_command_list / ... / spudgpu_end_command_list). Only
 * compiles; submit with aprend_command_list_submit. Compiling has no effect
 * on any texture's tracked layout, so a list that's compiled but never
 * submitted (or fails to compile) leaves nothing out of sync.
 *
 * Returns false, without a submittable result, if a malformed command was
 * sent or the pass scope is violated: a draw outside any pass, a nested
 * BEGIN_RENDERING, END_RENDERING with no open BEGIN_RENDERING, or a
 * BEGIN_RENDERING left open at the end of the list. Also fails on a
 * SET_SHADER_PIPELINE outside a pass or with a NULL pipeline, on a draw in a
 * pass that hasn't set a pipeline, on a draw whose pipeline reads a vertex
 * buffer slot that is empty or holds a buffer of another stride, on a
 * PUSH_CONSTANTS with no pipeline set in its pass or with bytes outside the
 * pipeline's push constant ranges, on a
 * SET_BINDING_SET with a NULL set or a _slot of APREND_MAX_BINDING_LAYOUTS or
 * more, on a draw whose pipeline declares a set slot that holds no set or a
 * set of another layout, and on a pass whose binding sets break the texture
 * rule above (APREND_COMMAND_SET_BINDING_SET). */
bool aprend_command_list_compile(aprend_command_list cmd_list);

/* Submits the last successful compile on [queue] and commits the image
 * layouts it leaves its textures in. The submission counts towards the
 * current frame, which aprend_instance_next_frame (aprendcontext.h) waits
 * for; every list of one instance goes on the same queue. Whatever was written to the vertex,
 * index and storage buffers the list uses, and has not reached the device
 * yet, is copied there first in the same submission (aprendbuffers.h). Those
 * buffers must still exist: destroying one and then submitting a list
 * compiled against it is a use after free. Submissions must run in submit order
 * (one queue), which is what the tracked layouts assume.
 *
 * Returns false without submitting if the list hasn't compiled successfully,
 * if a texture it uses has changed layout since it was compiled (another
 * list touching the same texture was submitted in between), or if it binds a
 * set holding a buffer set and aprend_instance_next_frame has moved the frame
 * on since it was compiled - recompile it and submit again. A compiled list may be submitted again as long as that still
 * holds. The caller decides when to wait for the GPU.
 *
 * A list with a PRESENT_TEXTURE must be submitted on its swap chain's queue,
 * while the back buffer it was compiled against is still the acquired one;
 * follow it with aprend_swap_chain_present. */
bool aprend_command_list_submit(aprend_command_list cmd_list, spudgpu_command_queue queue);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_COMMANDS_H
