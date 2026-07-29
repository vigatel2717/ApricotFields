
#ifndef APREND_COMMANDS_H
#define APREND_COMMANDS_H

#include "spudgpu.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_vertex_buffer_t *aprend_vertex_buffer;
typedef struct aprend_index_buffer_t *aprend_index_buffer;

typedef uint32_t APREND_COMMAND_TYPE;

enum {
    APREND_COMMAND_NONE = 0,
    APREND_COMMAND_SET_VERTEX_BUFFERS = 1,
    APREND_COMMAND_SET_INDEX_BUFFER = 2,
    APREND_COMMAND_SET_SHADER_PIPELINE = 3,
    APREND_COMMAND_DRAW = 4,
    APREND_COMMAND_DRAW_INDEXED = 5,
    APREND_COMMAND_DRAW_INSTANCED = 6,
    APREND_COMMAND_DRAW_INSTANCED_INDEXED = 7,
    APREND_COMMAND_SET_VIEWPORTS = 8,
    APREND_COMMAND_SET_SCISSOR_RECTS = 8,
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
    } _params;
} APREND_COMMAND;

typedef struct aprend_command_list_t *aprend_command_list;

aprend_command_list aprend_command_list_create();
void aprend_command_list_destroy(aprend_command_list cmd_list);

void aprend_command_list_reset(aprend_command_list cmd_list);
void aprend_send_command(aprend_command_list cmd_list, APREND_COMMAND cmd);

#if __cplusplus
} // Extern "C"
#endif // __cplusplus

#endif // APREND_COMMANDS_H
