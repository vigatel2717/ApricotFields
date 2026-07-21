
#include "render/aprendbase.h"
#include "aprend_internal.hpp"

aprend_instance_t::~aprend_instance_t() {
	spudgpu_destroy_shader_pipeline(this->default_pipeline);
	spudgpu_destroy_shader_module(this->default_vertex_shader);
	spudgpu_destroy_shader_module(this->default_fragment_shader);
	spudgpu_destroy_command_list(this->cmd_list);
	spudgpu_destroy_command_allocator(this->cmd_allocator);
}

extern "C" {
APREND_RESULT aprend_instance_create(const aprend_instance_desc *desc, aprend_instance *out_instance) {
	if (!desc)
		return APREND_RESULT_DESC_NULL;
	aprend_instance_t *result = (aprend_instance_t *)malloc(sizeof(aprend_instance_t));
	if (result)
		result = new (result) aprend_instance_t();
	else
		return APREND_RESULT_GENERAL_FAILURE;

	result->desc = *desc;

	spudgpu_command_allocator_desc aDesc = {};
	aDesc.flags                          = 0;
	aDesc.type                           = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;
	SPUDRESULT sr                        = spudgpu_create_command_allocator(desc->device, &aDesc, &result->cmd_allocator);
	if (sr != SPUD_SUCCESS)
		goto failedattempt;
	sr = spudgpu_create_command_list(result->cmd_allocator, &result->cmd_list);
	if (sr != SPUD_SUCCESS)
		goto failedattempt;

	*out_instance = result;
	return APREND_RESULT_SUCCESS;
failedattempt:
	result->~aprend_instance_t();
	free(result);
	return APREND_RESULT_GENERAL_FAILURE;
}
APREND_RESULT aprend_instance_get_desc(aprend_instance instance, aprend_instance_desc *out_desc) {
	if (!instance)
		return APREND_RESULT_INVALID_INSTANCE;
	*out_desc = instance->desc;
	return APREND_RESULT_SUCCESS;
}

APREND_RESULT aprend_instance_terminate(aprend_instance instance) {
	if (!instance)
		return APREND_RESULT_INVALID_INSTANCE;
	instance->~aprend_instance_t();
	free(instance);
	return APREND_RESULT_SUCCESS;
}
}
