
#include "render/aprendcontext.h"
#include "aprend_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

aprend_instance_t::~aprend_instance_t() {
	// Whatever is still queued goes now: the instance is the last thing that
	// could release it. aprend_instance_destroy has waited for the GPU. A
	// release may retire something more, which this loop then reaches.
	for (;;) {
		release_entry entry{};
		{
			std::lock_guard<std::mutex> lock(this->release_mutex);
			if (this->release_head == this->release_queue.size())
				break;
			entry = this->release_queue[this->release_head++];
		}
		entry.release(entry.object);
	}
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
	// One queue for the life of the instance. On a second queue a serial
	// could be reached while work submitted before it, on the first, was
	// still running, and everything that waits on the fence would be told
	// that work had finished.
	if (instance->submit_queue && queue != instance->submit_queue) {
		printf("apricot: submission refused: this instance submits on one queue, and this is not the queue of its first submission\n");
		return SPUDRESULT_GPU_INVALID_COMMAND_QUEUE;
	}
	const uint64_t serial = instance->submit_serial.load() + 1;
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
	// Set by a submission that went in: a failed first one binds nothing.
	instance->submit_queue = queue;
	if (out_serial)
		*out_serial = serial;
	// A point the GPU has usually moved past something by.
	aprend_instance_collect(instance);
	return SPUD_SUCCESS;
}

SPUDRESULT aprend_instance_wait_serial(
    aprend_instance instance,
    uint64_t serial) {
	return spudgpu_wait_for_fences(instance->desc.device, &instance->frame_fence, &serial, 1, true, UINT64_MAX);
}

void aprend_instance_collect(aprend_instance instance) {
	{
		std::lock_guard<std::mutex> lock(instance->release_mutex);
		if (instance->release_head == instance->release_queue.size())
			return;
	}
	// 0 if the value can't be read, which releases nothing. Read once: what
	// finishes while this runs is collected by the next call.
	const uint64_t completed = spudgpu_get_fence_value(instance->frame_fence);
	for (;;) {
		aprend_instance_t::release_entry entry{};
		{
			// Another thread may retire something at any moment, so the
			// queue is only touched under the lock, and the entry is taken
			// by value with the head moved past it before the lock goes.
			std::lock_guard<std::mutex> lock(instance->release_mutex);
			if (instance->release_head == instance->release_queue.size()) {
				// Everything taken: start the queue again from empty.
				instance->release_queue.clear();
				instance->release_head = 0;
				return;
			}
			entry = instance->release_queue[instance->release_head];
			if (entry.serial > completed) {
				// Stopping with entries still waiting. A caller that always
				// has work in flight never empties the queue, so the taken
				// entries at its front are dropped here once they are most
				// of it, or it would grow for as long as the instance lives.
				if (instance->release_head >= 64 && instance->release_head * 2 >= instance->release_queue.size()) {
					instance->release_queue.erase(
					    instance->release_queue.begin(), instance->release_queue.begin() + (std::ptrdiff_t)instance->release_head);
					instance->release_head = 0;
				}
				return;
			}
			++instance->release_head;
		}
		// Outside the lock: releasing calls into SpudGPU, and may itself
		// retire something.
		entry.release(entry.object);
	}
}

void aprend_instance_retire(
    aprend_instance instance,
    void *object,
    void (*release)(void *object)) {
	std::lock_guard<std::mutex> lock(instance->release_mutex);
	// Read under the lock, so entries go on in the order of their serials
	// whichever threads they come from.
	const uint64_t serial = instance->submit_serial;
	try {
		instance->release_queue.push_back({release, object, serial});
	} catch (const std::bad_alloc &) {
		printf("apricot: release queue out of memory: an object destroyed now is left unreleased\n");
	}
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
		result->unified_memory         = properties.unified_memory;
		result->device_mappable_memory = properties.device_mappable_memory;
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
	aprend_instance_collect(instance);
	if (SPUDFAIL(sr))
		printf("apricot: aprend_instance_next_frame: waiting for frame %u failed: %s\n", instance->frame_index, spudresult_str(sr));
	return instance->frame_index;
}
uint32_t aprend_instance_get_frame_index(aprend_instance instance) { return instance ? instance->frame_index : 0; }
bool aprend_instance_wait_idle(aprend_instance instance) {
	if (!instance)
		return false;
	SPUDRESULT sr = aprend_instance_wait_serial(instance, instance->submit_serial.load());
	aprend_instance_collect(instance);
	if (SPUDFAIL(sr)) {
		printf("apricot: aprend_instance_wait_idle failed: %s\n", spudresult_str(sr));
		return false;
	}
	return true;
}

void aprend_instance_destroy(aprend_instance instance) {
	if (instance) {
		// Everything retired is released by the destructor, so the GPU must
		// be done with all of it first.
		SPUDRESULT sr = aprend_instance_wait_serial(instance, instance->submit_serial.load());
		if (SPUDFAIL(sr))
			printf("apricot: aprend_instance_destroy: waiting for the GPU failed: %s\n", spudresult_str(sr));
		instance->~aprend_instance_t();
		free(instance);
	}
}
} // Extern "C"
