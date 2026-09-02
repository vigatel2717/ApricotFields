
#ifndef APRENDEXTINTERNAL_HPP
#define APRENDEXTINTERNAL_HPP

#include "render/aprendcontext.h"

#if __cplusplus
extern "C" {
#endif // __cplusplus

typedef struct aprend_ext_scene_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_ext_scene_t() = default;
	~aprend_ext_scene_t();
	aprend_instance_t *ap_instance{nullptr};
} aprend_ext_scene_t;

#if __cplusplus
}
#endif // __cplusplus

#endif // APRENDEXTINTERNAL_HPP
