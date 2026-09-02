
#include "render/aprendextscene.h"
#include "aprend_ext_internal.hpp"

#include <new>
#include <stdlib.h>

aprend_ext_scene_t::~aprend_ext_scene_t() {}

extern "C" {
aprend_ext_scene aprend_ext_scene_create(aprend_instance instance) {
	if (!instance)
		return nullptr;

	aprend_ext_scene_t *result = (aprend_ext_scene_t *)malloc(sizeof(aprend_ext_scene_t));
	if (!result)
		return nullptr;
	result = new (result) aprend_ext_scene_t();

	result->ap_instance = instance;

	return result;
}
void aprend_ext_scene_destroy(aprend_ext_scene scene) {
	if (scene) {
		scene->~aprend_ext_scene_t();
		free(scene);
	}
}

bool aprenderer_ext_begin_scene(aprend_ext_scene scene) {
	if (!scene)
		return false;
	return true;
}
} // Extern "C"
