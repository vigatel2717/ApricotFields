
#include "render/aprendbase.h"
#include "aprend_internal.hpp"

#ifdef _DEBUG
void apri_debug_name_set(void *object, const char *name) {
	if (object) {
		/* Store the debug name in the first member of the object struct. */
		char **name_ptr = (char **)object;
		*name_ptr       = (char *)name;
	}
}

const char *apri_debug_name_get(void *object) {
	if (object) {
		/* Get the debug name from the first member of the object struct. */
		char **name_ptr = (char **)object;
		return *name_ptr;
	} else
		return nullptr;
}
#endif // _DEBUG

extern "C" {
APREND_RESULT aprend_instance_create(
    const aprend_instance_desc *desc,
    aprend_instance *out_instance) {
	if (!desc)
		return APREND_RESULT_DESC_NULL;
	aprend_instance_t *result =
	    (aprend_instance_t *)calloc(1, sizeof(aprend_instance_t));
    *out_instance = result;
	return APREND_RESULT_SUCCESS;
}
APREND_RESULT aprend_instance_get_desc(
    aprend_instance instance,
    aprend_instance_desc *out_desc) {
    if (!instance) return APREND_RESULT_INVALID_INSTANCE;
    *out_desc=instance->desc;
    return APREND_RESULT_SUCCESS;
}

APREND_RESULT aprend_instance_terminate(aprend_instance instance) {
	if (!instance)
		return APREND_RESULT_INVALID_INSTANCE;
	free(instance);
    return APREND_RESULT_SUCCESS;
}
}
