
#include "render/aprendcontext.h"
#include "aprend_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

aprend_instance_t::~aprend_instance_t() {
	// An instance that failed to create may have neither.
	if (this->frame_fence)
		spudgpu_destroy_fence(this->frame_fence);
	if (this->cmd_allocator)
		spudgpu_destroy_command_allocator(this->cmd_allocator);
}

SPUDRESULT aprend_instance_submit(
    aprend_instance instance,
    spudgpu_command_queue queue,
    spudgpu_command_list *lists,
    uint32_t list_count,
    spudgpu_swap_chain swap_chain,
    uint64_t *out_serial) {
	const uint64_t serial = instance->submit_serial + 1;
	spudgpu_submit_desc submit{};
	submit.cmd_lists          = lists;
	submit.cmd_list_count     = list_count;
	submit.signal_fence       = instance->frame_fence;
	submit.signal_fence_value = serial;
	submit.swap_chain         = swap_chain;
	SPUDRESULT sr             = spudgpu_queue_submit(queue, &submit);
	if (SPUDFAIL(sr))
		return sr;
	// Taken only by a submission that went in, so the fence's value never
	// has a gap a wait could hang on.
	instance->submit_serial                            = serial;
	instance->frame_last_serial[instance->frame_index] = serial;
	if (out_serial)
		*out_serial = serial;
	return SPUD_SUCCESS;
}

SPUDRESULT aprend_instance_wait_serial(
    aprend_instance instance,
    uint64_t serial) {
	return spudgpu_wait_for_fences(instance->desc.device, &instance->frame_fence, &serial, 1, true, UINT64_MAX);
}

extern "C" {
aprend_instance aprend_instance_create(const aprend_instance_desc *desc) {
	if (!desc)
		return nullptr;
	if (!desc->device) {
		printf("apricot: aprend_instance_create: device is NULL\n");
		return nullptr;
	}
	if (desc->frames_in_flight == 0) {
		printf("apricot: aprend_instance_create: frames_in_flight is 0\n");
		return nullptr;
	}
	if (desc->frames_in_flight > APREND_MAX_FRAMES_IN_FLIGHT) {
		printf("apricot: aprend_instance_create: frames_in_flight %u is above APREND_MAX_FRAMES_IN_FLIGHT\n", desc->frames_in_flight);
		return nullptr;
	}
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

	{
		SPUDGPU_DEVICE_PROPERTIES properties{};
		sr = spudgpu_get_device_properties(desc->device, &properties);
		if (SPUDFAIL(sr))
			goto failedattempt;
		result->unified_memory = properties.unified_memory;
	}

	sr = spudgpu_create_fence(desc->device, SPUDGPU_FENCE_FLAG_NONE, 0, &result->frame_fence);
	if (SPUDFAIL(sr)) {
		result->frame_fence = nullptr;
		goto failedattempt;
	}

	return result;
failedattempt:
	printf("apricot: aprend_instance_create failed: %s\n", spudresult_str(sr));
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

uint32_t aprend_instance_next_frame(aprend_instance instance) {
	if (!instance)
		return 0;
	instance->frame_index = (instance->frame_index + 1) % instance->desc.frames_in_flight;

	// Everything submitted the last time this index was current must have
	// finished before its buffer set copies are written again.
	SPUDRESULT sr = aprend_instance_wait_serial(instance, instance->frame_last_serial[instance->frame_index]);
	if (SPUDFAIL(sr))
		printf("apricot: aprend_instance_next_frame: waiting for frame %u failed: %s\n", instance->frame_index, spudresult_str(sr));
	return instance->frame_index;
}
uint32_t aprend_instance_get_frame_index(aprend_instance instance) { return instance ? instance->frame_index : 0; }
bool aprend_instance_wait_idle(aprend_instance instance) {
	if (!instance)
		return false;
	SPUDRESULT sr = aprend_instance_wait_serial(instance, instance->submit_serial);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_instance_wait_idle failed: %s\n", spudresult_str(sr));
		return false;
	}
	return true;
}

void aprend_instance_destroy(aprend_instance instance) {
	if (instance) {
		instance->~aprend_instance_t();
		free(instance);
	}
}
} // Extern "C"
