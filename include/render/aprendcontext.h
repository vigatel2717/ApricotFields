
#ifndef APREND_BASE_H
#define APREND_BASE_H

#include <spudgpu.h>

#if __cplusplus
extern "C" {
#endif

typedef struct aprend_instance_desc {
	spudgpu_device device;
	const char *app_name;
	uint32_t app_version;
	const char *engine_name;
	uint32_t engine_version;
} aprend_instance_desc;

typedef struct aprend_instance_t *aprend_instance;
aprend_instance aprend_instance_create(const aprend_instance_desc *desc);
bool aprend_instance_get_desc(
    aprend_instance instance,
    aprend_instance_desc *out_desc);
void aprend_instance_destroy(aprend_instance instance);

#if __cplusplus
}
#endif

#endif // APREND_BASE_H
