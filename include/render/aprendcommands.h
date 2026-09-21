
#ifndef APREND_COMMANDS_H
#define APREND_COMMANDS_H

#include "aprendcontext.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_vertex_buffer_t *aprend_vertex_buffer;
typedef struct aprend_index_buffer_t *aprend_index_buffer;
typedef struct aprend_texture_view_t *aprend_texture_view;
typedef struct aprend_graphics_pipeline_t *aprend_graphics_pipeline;

typedef uint32_t APREND_COMMAND_TYPE;

enum APREND_COMMAND_TYPE {
	APREND_COMMAND_NONE                   = 0,
	APREND_COMMAND_SET_VERTEX_BUFFERS     = 1,
	APREND_COMMAND_SET_INDEX_BUFFER       = 2,
	APREND_COMMAND_SET_SHADER_PIPELINE    = 3,
	APREND_COMMAND_DRAW                   = 4,
	APREND_COMMAND_DRAW_INDEXED           = 5,
	APREND_COMMAND_DRAW_INSTANCED         = 6,
	APREND_COMMAND_DRAW_INSTANCED_INDEXED = 7,
	APREND_COMMAND_SET_VIEWPORTS          = 8,
	APREND_COMMAND_SET_SCISSOR_RECTS      = 9,
	APREND_COMMAND_SET_RENDER_TARGETS     = 10,
	APREND_COMMAND_CLEAR_COLORS           = 11,
	APREND_COMMAND_CLEAR_DEPTH            = 12,
};

typedef struct APREND_COMMAND {
	APREND_COMMAND_TYPE _type;
	union {
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
			aprend_texture_view *_color_attachments;
			uint32_t _color_attachment_count;
			uint32_t _color_load_op;
			aprend_texture_view _depth_attachment;
			uint32_t _depth_load_op;
		} _set_render_targets;
        struct {
			float _r, _g, _b, _a;
		} _clear_colors;
        struct {
			float _depth;
			uint32_t _stencil;
		} _clear_depth;
	} _params;
} APREND_COMMAND;

typedef struct aprend_command_list_t *aprend_command_list;

aprend_command_list aprend_command_list_create(aprend_instance instance);
void aprend_command_list_destroy(aprend_command_list cmd_list);

void aprend_command_list_reset(aprend_command_list cmd_list);
void aprend_send_command(
    aprend_command_list cmd_list,
    APREND_COMMAND cmd);

/* Translates every command recorded since the last aprend_command_list_reset
 * into the underlying spudgpu_command_list and closes it for submission
 * (spudgpu_begin_command_list / ... / spudgpu_end_command_list). Call
 * spudgpu_submit_command_lists on the result yourself — this function only
 * compiles, it does not submit. */
bool aprend_command_list_compile(aprend_command_list cmd_list);
spudgpu_command_list aprend_command_list_get_spudgpu_command_list(aprend_command_list cmd_list);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_COMMANDS_H
