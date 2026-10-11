
#ifndef APREND_COMMANDS_H
#define APREND_COMMANDS_H

#include "aprendcontext.h"
#include "aprendimages.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_vertex_buffer_t *aprend_vertex_buffer;
typedef struct aprend_index_buffer_t *aprend_index_buffer;
typedef struct aprend_storage_buffer_t *aprend_storage_buffer;
typedef struct aprend_storage_buffer_set_t *aprend_storage_buffer_set;
typedef struct aprend_graphics_pipeline_t *aprend_graphics_pipeline;
typedef struct aprend_compute_pipeline_t *aprend_compute_pipeline;
typedef struct aprend_binding_set_t *aprend_binding_set;
typedef struct aprend_swap_chain_t *aprend_swap_chain;

/* Upper bound on color targets in one BEGIN_RENDERING pass. */
#define APREND_MAX_COLOR_TARGETS SPUDGPU_MAX_COLOR_ATTACHMENTS

/* aprend_color_target / aprend_depth_target (aprendimages.h) describe how one
 * pass uses each attachment view. Zero-initialized ops mean LOAD + STORE. */

typedef uint32_t APREND_COMMAND_TYPE;

enum {
	APREND_COMMAND_NONE                   = 0,
	/* Puts _vertex_buffers[i] at vertex buffer slot _start_slot + i. Slot N
	 * is what a pipeline's _vertex_bindings[N] reads (aprendpipeline.h), per
	 * vertex or per instance as that binding says. At each draw, every slot
	 * the bound pipeline reads must hold a buffer whose stride is the
	 * binding's. */
	APREND_COMMAND_SET_VERTEX_BUFFERS     = 1,
	/* The index buffer of the following indexed draws (DRAW_INDEXED,
	 * DRAW_INSTANCED_INDEXED, DRAW_INDEXED_INDIRECT), until another is set.
	 * An indexed draw with none set fails the compile. */
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
	/* Compute, in the same list as rendering. Both commands are valid only
	 * outside a pass.
	 *
	 * SET_COMPUTE_PIPELINE sets the aprend_compute_pipeline (aprendpipeline.h)
	 * the following dispatches run. It lasts until the next BEGIN_RENDERING or
	 * PRESENT_TEXTURE: after either, it is set again before a dispatch.
	 *
	 * DISPATCH runs it over _group_count_x by _y by _z workgroups, none of
	 * them 0. The counts are in workgroups, not threads: a workgroup's size is
	 * the shader's own (local_size in GLSL).
	 *
	 * Binding sets are the same slots SET_BINDING_SET fills for draws: at a
	 * dispatch, every slot the compute pipeline declares must hold a set
	 * created from the layout the pipeline declares there. Right before each
	 * dispatch every texture in those sets is moved into the layout its
	 * binding reads it in, and every storage buffer and storage image in them
	 * is ordered after the writes of earlier dispatches and draws, so a
	 * dispatch reads what the one before it wrote. A texture can't be in a
	 * sampled slot and a storage image slot of one dispatch.
	 *
	 * PUSH_CONSTANTS sent outside a pass writes the set compute pipeline's
	 * block, under the same rules as inside a pass for a graphics pipeline:
	 * after SET_COMPUTE_PIPELINE, and not carried to the next one set. */
	APREND_COMMAND_SET_COMPUTE_PIPELINE   = 15,
	APREND_COMMAND_DISPATCH               = 16,
	/* Draws whose arguments the GPU reads from a storage buffer created with
	 * APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS (aprendbuffers.h):
	 * _draw_count draws, the first from the entry at byte _offset of the
	 * buffer and each next one _stride bytes on. The buffer is _buffer, or
	 * the current frame's copy of _set; exactly one of the two is given. A
	 * compute shader may have written the entries through a storage slot
	 * earlier in the list, or the CPU with aprend_storage_buffer_update.
	 * Before the pass the buffer is moved from the state a storage slot uses
	 * it in to the one a draw reads it in, and back before the next dispatch
	 * or pass that has it in a binding set; it can't be both an argument
	 * buffer and in a binding set of one pass. An entry is a
	 * spudgpu_draw_indirect_args for DRAW_INDIRECT and a
	 * spudgpu_draw_indexed_indirect_args for DRAW_INDEXED_INDIRECT
	 * (spudgpu.h). _draw_count is not 0, _offset and _stride are multiples of
	 * 4, _stride is at least the size of one entry, and the last entry ends
	 * inside the buffer. They are draws in every other respect: inside a
	 * pass, with a pipeline set and its vertex buffers and binding sets in
	 * place, and not with a mesh shader pipeline. */
	APREND_COMMAND_DRAW_INDIRECT          = 17,
	APREND_COMMAND_DRAW_INDEXED_INDIRECT  = 18,
	/* Runs the set mesh shader pipeline (aprend_graphics_pipeline_desc::
	 * mesh_shader) over _group_count_x by _y by _z workgroups, none of them
	 * 0. It is that pipeline's draw: inside a pass, with the pipeline set and
	 * its binding sets in place. The draw commands above are refused with a
	 * mesh shader pipeline, and this one with any other. */
	APREND_COMMAND_DISPATCH_MESH          = 19,
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
		struct {
			aprend_compute_pipeline _pipeline;
		} _set_compute_pipeline;
		struct {
			uint32_t _group_count_x;
			uint32_t _group_count_y;
			uint32_t _group_count_z;
		} _dispatch;
		/* DRAW_INDIRECT and DRAW_INDEXED_INDIRECT. */
		struct {
			aprend_storage_buffer _buffer;
			uint64_t _offset;
			uint32_t _draw_count;
			uint32_t _stride;
			/* A storage buffer set in place of _buffer. */
			aprend_storage_buffer_set _set;
		} _draw_indirect;
		struct {
			uint32_t _group_count_x;
			uint32_t _group_count_y;
			uint32_t _group_count_z;
		} _dispatch_mesh;
	} _params;
} APREND_COMMAND;

typedef struct aprend_command_list_t *aprend_command_list;

/* Threads: the rule is in aprendcontext.h ("Threads"). What is particular to
 * a command list:
 *
 * - aprend_send_command and aprend_command_list_reset write the list and
 *   nothing else, so different lists may be recorded from different threads
 *   at once, one thread to a list. What the commands name must not be
 *   destroyed by another thread meanwhile.
 * - aprend_command_list_create, _compile, _submit and _destroy are not part
 *   of that. Every list of an instance is created from and compiled through
 *   the instance's one SpudGPU command allocator; compiling reads the
 *   layouts a submission writes; and a submission takes the next place in
 *   the instance's order. They are made by one thread at a time across the
 *   whole instance, not only one list. */
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
 * is not a multiple of 4; a DISPATCH or DISPATCH_MESH with a group count of
 * 0; a DRAW_INDIRECT or DRAW_INDEXED_INDIRECT with neither or both of
 * _buffer and _set, a _set of another instance, a buffer created without
 * APREND_STORAGE_BUFFER_USAGE_INDIRECT_ARGUMENTS, a _draw_count of 0, an _offset or _stride that is not a multiple of 4, a
 * _stride smaller than one entry, or entries that run past the end of the
 * buffer. */
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
 * pass that hasn't set a pipeline, on an indexed draw with no index buffer
 * set, on a SET_INDEX_BUFFER with a NULL buffer, on a pass with an indirect
 * draw whose
 * argument buffer is also in a binding set the pass's draws read, on a draw
 * command with a mesh shader
 * pipeline or a DISPATCH_MESH with any other, on a draw whose pipeline reads a vertex
 * buffer slot that is empty or holds a buffer of another stride, on a
 * PUSH_CONSTANTS with no pipeline set in its pass (or, outside a pass, no
 * compute pipeline set) or with bytes outside the pipeline's push constant
 * ranges, on a SET_COMPUTE_PIPELINE or DISPATCH inside a pass, a
 * SET_COMPUTE_PIPELINE with a NULL pipeline, a DISPATCH with no compute
 * pipeline set, a DISPATCH whose pipeline declares a set slot that holds no
 * set or a set of another layout, or whose sets hold one texture in a sampled
 * slot and a storage image slot, on a
 * SET_BINDING_SET with a NULL set or a _slot of APREND_MAX_BINDING_LAYOUTS or
 * more, on a draw whose pipeline declares a set slot that holds no set or a
 * set of another layout, and on a pass whose binding sets break the texture
 * rule above (APREND_COMMAND_SET_BINDING_SET). */
bool aprend_command_list_compile(aprend_command_list cmd_list);

/* Submits the last successful compile on [queue] and commits the image
 * layouts it leaves its textures in. The submission counts towards the
 * current frame, which aprend_instance_next_frame (aprendcontext.h) waits
 * for.
 *
 * [queue] is the instance's one queue (aprendcontext.h): the queue of the
 * instance's first submission, and every one after it. Another is refused.
 * Submissions run in submit order on it, which is what the tracked layouts
 * and the frame pacing assume.
 *
 * Whatever was written to the vertex, index and storage buffers the list
 * uses, and has not reached the device yet, is copied there first in the same
 * submission (aprendbuffers.h). Those buffers must still exist: destroying
 * one and then submitting a list compiled against it is a use after free.
 *
 * Returns false without submitting if the list hasn't compiled successfully,
 * if [queue] is not the queue the instance already submits on, if a texture it uses has changed layout since it was compiled (another
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
