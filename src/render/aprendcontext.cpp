
#include "render/aprendcontext.h"
#include "aprend_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

static spudgpu_shader_module aprend___internal___load_spirv(
    spudgpu_device device,
    SPUDGPU_SHADER_STAGE stage,
    const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("apricot: cannot open shader '%s'\n", path);
		return nullptr;
	}
	fseek(f, 0, SEEK_END);
	size_t sz = static_cast<size_t>(ftell(f));
	rewind(f);
	auto *code = static_cast<uint32_t *>(malloc(sz));
	fread(code, 1, sz, f);
	fclose(f);

	spudgpu_shader_module_desc desc{};
	desc.stage                = stage;
	desc.spirv_code           = code;
	desc.spirv_size           = sz;
	spudgpu_shader_module mod = nullptr;

	SPUDRESULT sr = spudgpu_create_shader_module(device, &desc, &mod);
	free(code);
	if (SPUDFAIL(sr))
		return nullptr;
	else
		return mod;
}

aprend_instance_t::~aprend_instance_t() {
	spudgpu_destroy_command_list(this->cmd_list);
	spudgpu_destroy_command_allocator(this->cmd_allocator);
}

extern "C" {
aprend_instance aprend_instance_create(const aprend_instance_desc *desc) {
	if (!desc)
		return nullptr;
	aprend_instance_t *result = (aprend_instance_t *)malloc(sizeof(aprend_instance_t));
	if (!result)
		return nullptr;
	result = new (result) aprend_instance_t();

	result->desc = *desc;

	SPUDRESULT sr = SPUD_SUCCESS;

	spudgpu_command_allocator_desc aDesc = {};
	aDesc.flags                          = 0;
	aDesc.type                           = SPUDGPU_COMMAND_LIST_TYPE_DIRECT;

	sr = spudgpu_create_command_allocator(desc->device, &aDesc, &result->cmd_allocator);
	if (SPUDFAIL(sr))
		goto failedattempt;
	sr = spudgpu_create_command_list(result->cmd_allocator, &result->cmd_list);
	if (SPUDFAIL(sr))
		goto failedattempt;

	return result;
failedattempt:
	result->~aprend_instance_t();
	free(result);
	return nullptr;
}
bool aprend_instance_get_desc(
    aprend_instance instance,
    aprend_instance_desc *out_desc) {
	if (instance && out_desc) {
		*out_desc = instance->desc;
		return true;
	} else
		return false;
}

void aprend_instance_destroy(aprend_instance instance) {
	if (instance) {
		instance->~aprend_instance_t();
		free(instance);
	}
}
} // Extern "C"
