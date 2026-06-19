
#ifndef APREND_BASE_H
#define APREND_BASE_H

#include <spudgpu.h>

#if __cplusplus
extern "C" {
#endif

/*
 * In Debug configuration, every implementation of an Apricot opaque handle must
 * have its first member be a pointer to a null-terminated string containing the
 * debug name of the object, for use in debugging and validation layers. This is
 * to garauntee compatibility with casting, for a C++ std::string_view or
 * similar, so C++ backends can provide zero-cost debug names without extra
 * allocations.
 */
#ifdef _DEBUG
void apri_debug_name_set(void *object, const char *name);
#define APRI_SET_DEBUG_NAME(obj, name)                                         \
	apri_debug_name_set((void *)(obj), (name))
const char *apri_debug_name_get(void *object);
#define APRI_GET_DEBUG_NAME(obj) apri_debug_name_get((void *)(obj))
#endif

typedef enum APREND_RESULT {
	APREND_RESULT_SUCCESS                = 0,
	APREND_RESULT_GENERAL_FAILURE        = 1,
	APREND_RESULT_DESC_NULL              = 10,
	APREND_RESULT_INVALID_INSTANCE       = 20,
	APREND_RESULT_INVALID_SPUDGPU_DEVICE = 30
} APREND_RESULT;

typedef struct aprend_instance_desc {
	spudgpu_device device;
	const char *app_name;
	uint32_t app_version;
	const char *engine_name;
	uint32_t engine_version;
} aprend_instance_desc;

typedef struct aprend_instance_t *aprend_instance;
APREND_RESULT aprend_instance_create(
    const aprend_instance_desc *desc, aprend_instance *out_instance);
APREND_RESULT aprend_instance_get_desc(
    aprend_instance instance, aprend_instance_desc *out_desc);
APREND_RESULT aprend_instance_terminate(aprend_instance instance);

#if __cplusplus
}
#endif

#endif // APREND_BASE_H
