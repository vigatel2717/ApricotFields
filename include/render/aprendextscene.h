
#ifndef APRENDEXTSCENE_HPP
#define APRENDEXTSCENE_HPP

#include "aprendcontext.h"
#include "aprendcommands.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_ext_scene_t *aprend_ext_scene;
aprend_ext_scene aprend_ext_scene_create(aprend_instance instance);
void aprend_ext_scene_destroy(aprend_ext_scene scene);

bool aprend_ext_cmd_begin_scene(aprend_command_list cmd_list, aprend_ext_scene scene);
bool aprend_ext_cmd_end_scene(aprend_command_list cmd_list);

#if __cplusplus
}
#endif // __cplusplus

#endif // APRENDEXTSCENE_HPP
