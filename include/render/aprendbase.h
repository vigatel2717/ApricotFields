
#ifndef APREND_BASE_H
#define APREND_BASE_H

#include <spudgpu.h>

#if __cplusplus
extern "C" {
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
