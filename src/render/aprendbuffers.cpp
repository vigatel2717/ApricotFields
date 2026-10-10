
#include "render/aprendbuffers.h"
#include "aprend_internal.hpp"

#include <spudgpu.h>

aprend_uniform_buffer_t::~aprend_uniform_buffer_t() {
	spudgpu_unmap_buffer(this->buffer);
	spudgpu_destroy_buffer_view(this->buffer_view);
	spudgpu_destroy_buffer(this->buffer);
    free(this->layout.uniforms);
}

// A buffer that failed to create may have no view, and its store may hold
// nothing: each is destroyed only if it exists, the view before its buffer.
aprend_vertex_buffer_t::~aprend_vertex_buffer_t() {
	if (this->buffer_view)
		spudgpu_destroy_buffer_view(this->buffer_view);
	aprend_buffer_store_destroy(&this->store);
	free(this->vertex_layout.elements);
}

aprend_index_buffer_t::~aprend_index_buffer_t() {
	if (this->buffer_view)
		spudgpu_destroy_buffer_view(this->buffer_view);
	aprend_buffer_store_destroy(&this->store);
}

aprend_storage_buffer_t::~aprend_storage_buffer_t() {
	if (this->buffer_view)
		spudgpu_destroy_buffer_view(this->buffer_view);
	aprend_buffer_store_destroy(&this->store);
}

// A set that failed part-way holds only the buffers it got as far as making.
aprend_uniform_buffer_set_t::~aprend_uniform_buffer_set_t() {
	for (uint32_t i = 0; i < this->buffer_count; ++i)
		aprend_uniform_buffer_destroy(this->buffers[i]);
}

aprend_storage_buffer_set_t::~aprend_storage_buffer_set_t() {
	for (uint32_t i = 0; i < this->buffer_count; ++i)
		aprend_storage_buffer_destroy(this->buffers[i]);
}

SPUDRESULT aprend_buffer_store_create(
    aprend_buffer_store *store,
    aprend_instance instance,
    uint64_t size,
    SPUDGPU_BUFFER_USAGE usage,
    SPUDGPU_RESOURCE_STATE use_state) {
	store->instance  = instance;
	store->size      = size;
	store->use_state = use_state;

	// A device whose memory is the system's is written directly. One with
	// its own gets the buffer there and a staging buffer to write through.
	const bool staged = !instance->unified_memory;

	spudgpu_buffer_desc bd{};
	bd.buffer_flags = SPUDGPU_RESOURCE_FLAG_NONE;
	bd.heap_flags   = SPUDGPU_HEAP_FLAG_NONE;
	bd.size         = size;
	if (staged) {
		bd.memory_flags = SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;
		bd.usage        = usage | SPUDGPU_BUFFER_USAGE_TRANSFER_DST;
	} else {
		// Host-visible and host-coherent, and not device-local as well: the
		// Vulkan backend requires every property asked for, and memory that
		// is all three isn't something every device has.
		bd.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT;
		bd.usage        = usage;
	}
	SPUDRESULT sr = spudgpu_create_buffer(instance->desc.device, &bd, &store->buffer);
	if (SPUDFAIL(sr)) {
		store->buffer = nullptr;
		return sr;
	}
	if (!staged)
		return SPUD_SUCCESS;

	spudgpu_buffer_desc sd{};
	sd.buffer_flags = SPUDGPU_RESOURCE_FLAG_NONE;
	sd.heap_flags   = SPUDGPU_HEAP_FLAG_NONE;
	sd.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT;
	sd.size         = size;
	sd.usage        = SPUDGPU_BUFFER_USAGE_TRANSFER_SRC;
	sr              = spudgpu_create_buffer(instance->desc.device, &sd, &store->staging);
	if (SPUDFAIL(sr)) {
		spudgpu_destroy_buffer(store->buffer);
		store->buffer  = nullptr;
		store->staging = nullptr;
		return sr;
	}
	return SPUD_SUCCESS;
}

void aprend_buffer_store_destroy(aprend_buffer_store *store) {
	if (store->staging)
		spudgpu_destroy_buffer(store->staging);
	if (store->buffer)
		spudgpu_destroy_buffer(store->buffer);
	store->staging = nullptr;
	store->buffer  = nullptr;
}

SPUDRESULT aprend_buffer_store_write(
    aprend_buffer_store *store,
    uint64_t offset,
    uint64_t size,
    const void *data) {
	spudgpu_buffer target = store->staging ? store->staging : store->buffer;
	void *mapped          = nullptr;
	SPUDRESULT sr         = spudgpu_map_buffer(target, offset, size, &mapped);
	if (SPUDFAIL(sr))
		return sr;
	memcpy(mapped, data, (size_t)size);
	spudgpu_unmap_buffer(target);
	if (!store->staging)
		return SPUD_SUCCESS;

	// One range covers every write since the last copy. Bytes between two
	// writes are copied again with them, which is correct: staging holds
	// everything ever written.
	if (store->dirty_begin == store->dirty_end) {
		store->dirty_begin = offset;
		store->dirty_end   = offset + size;
		return SPUD_SUCCESS;
	}
	if (offset < store->dirty_begin)
		store->dirty_begin = offset;
	if (offset + size > store->dirty_end)
		store->dirty_end = offset + size;
	return SPUD_SUCCESS;
}

extern "C" {

uint32_t aprend_uniform_type_get_size(APREND_UNIFORM_TYPE type) {
	switch (type) {
	case APREND_UNIFORM_TYPE_BOOL:
	case APREND_UNIFORM_TYPE_INT:
	case APREND_UNIFORM_TYPE_UINT:
	case APREND_UNIFORM_TYPE_FLOAT:
		return 4;
	case APREND_UNIFORM_TYPE_INT2:
	case APREND_UNIFORM_TYPE_UINT2:
	case APREND_UNIFORM_TYPE_VEC2:
		return 8;
	case APREND_UNIFORM_TYPE_INT3:
	case APREND_UNIFORM_TYPE_UINT3:
	case APREND_UNIFORM_TYPE_VEC3:
		return 12;
	case APREND_UNIFORM_TYPE_INT4:
	case APREND_UNIFORM_TYPE_UINT4:
	case APREND_UNIFORM_TYPE_VEC4:
		return 16;
	case APREND_UNIFORM_TYPE_MAT4:
		return 64;
	default:
		return 0;
	}
}
// The distance between two elements of an array of [type] in a std140 block:
// the type's size rounded up to 16.
static uint32_t aprend_uniform_array_stride(APREND_UNIFORM_TYPE type) { return (aprend_uniform_type_get_size(type) + 15) & ~15u; }

// The bytes [u] takes in a std140 block from its offset. u.size is an array
// element count, not a byte size; 0 and 1 are both "not an array".
static uint64_t aprend_uniform_footprint(const aprend_uniform &u) {
	if (u.size <= 1)
		return aprend_uniform_type_get_size(u.type);
	return (uint64_t)aprend_uniform_array_stride(u.type) * u.size;
}

aprend_uniform_buffer aprend_uniform_buffer_create(aprend_instance instance, const aprend_uniform_layout *layout) {
	if (!instance || !layout)
		return nullptr;
	aprend_uniform_buffer_t *result = (aprend_uniform_buffer_t *)malloc(sizeof(aprend_uniform_buffer_t));
	if (result)
		result = new (result) aprend_uniform_buffer_t();
	else
		return NULL;

    SPUDRESULT sr = SPUD_SUCCESS;
	{
		// One block: the uniforms, then a copy of each name, so
		// aprend_uniform_buffer_update_by_name never reads a string the caller
		// has since released. The destructor frees it in one go.
		size_t name_bytes = 0;
		for (uint32_t i = 0; i < layout->count; ++i)
			if (layout->uniforms[i].name)
				name_bytes += strlen(layout->uniforms[i].name) + 1;
		const size_t uniform_bytes = sizeof(aprend_uniform) * layout->count;

		result->layout.uniforms = (aprend_uniform *)malloc(uniform_bytes + name_bytes);
		if (!result->layout.uniforms)
			goto failedattempt;
		result->layout.count = layout->count;
		memcpy(result->layout.uniforms, layout->uniforms, uniform_bytes);

		char *names = (char *)result->layout.uniforms + uniform_bytes;
		for (uint32_t i = 0; i < layout->count; ++i) {
			const char *name = layout->uniforms[i].name;
			if (!name)
				continue;
			const size_t length = strlen(name) + 1;
			memcpy(names, name, length);
			result->layout.uniforms[i].name = names;
			names += length;
		}
	}
	// The block ends where its furthest uniform does, rounded up to 16 as
	// std140 rounds a block. Offsets are the caller's; nothing is placed here.
	result->total_size = 0;
	for (size_t i = 0; i < layout->count; ++i) {
		const aprend_uniform &u    = layout->uniforms[i];
		const uint64_t uniform_end = (uint64_t)u.offset + aprend_uniform_footprint(u);
		if (uniform_end > UINT32_MAX - 15) {
			printf("apricot: aprend_uniform_buffer_create: uniform %u ends past what 32 bits can address\n", (unsigned)i);
			goto failedattempt;
		}
		if (uniform_end > result->total_size)
			result->total_size = (uint32_t)uniform_end;
	}
	result->total_size = (result->total_size + 15) & ~15u;

	{
		spudgpu_buffer_desc bd{};
		bd.buffer_flags = SPUDGPU_RESOURCE_FLAG_NONE;
		bd.heap_flags   = SPUDGPU_HEAP_FLAG_NONE;
		// Host-visible and host-coherent only, as aprend_storage_buffer_create.
		bd.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT;
		bd.size         = result->total_size;
		bd.usage        = SPUDGPU_BUFFER_USAGE_UNIFORM;
		sr = spudgpu_create_buffer(instance->desc.device, &bd, &result->buffer);
        if (sr != SPUD_SUCCESS)
			goto failedattempt;

		result->buffer_view_desc.parent_buffer = result->buffer;
		result->buffer_view_desc.offset_from_parent_buffer = 0;
		result->buffer_view_desc.stride = 0; // raw, unformatted buffer
		result->buffer_view_desc.size = result->total_size;
		sr = spudgpu_create_buffer_view(result->buffer, &result->buffer_view_desc, &result->buffer_view);
        if (sr != SPUD_SUCCESS)
			goto failedattempt;

		sr = spudgpu_map_buffer(result->buffer, 0, result->total_size, &result->uniform_data_ptr);
        if (sr != SPUD_SUCCESS)
			goto failedattempt;
	}

	return result;
failedattempt:
    printf("%s", spudresult_str(sr));
	result->~aprend_uniform_buffer_t();
	free(result);
	return NULL;
}
void aprend_uniform_buffer_destroy(aprend_uniform_buffer buffer) {
	if (buffer) {
		buffer->~aprend_uniform_buffer_t();
		free(buffer);
	}
}
spudgpu_buffer_view aprend_uniform_buffer_get_spudgpu_buffer_view(aprend_uniform_buffer buffer) { return buffer ? buffer->buffer_view : NULL; }
bool aprend_uniform_buffer_update(aprend_uniform_buffer buffer, uint32_t local_offset, uint32_t size, void *pData) {
	if (!buffer || !pData || size == 0)
		return false;
	if ((uint64_t)local_offset + size > buffer->total_size)
		return false;
	uint32_t byte_offset = buffer->buffer_view_desc.offset_from_parent_buffer + local_offset;
	memcpy((uint8_t *)buffer->uniform_data_ptr + byte_offset, pData, size);
	return true;
}
bool aprend_uniform_buffer_update_by_name(aprend_uniform_buffer buffer, const char *name, void *pData) {
	if (!buffer)
		return false;
	if (!name)
		return false;
	if (!pData)
		return false;
	for (uint32_t i = 0; i < buffer->layout.count; ++i) {
		const aprend_uniform &u = buffer->layout.uniforms[i];
		if (!u.name || strcmp(u.name, name) != 0)
			continue;
		const uint32_t type_size = aprend_uniform_type_get_size(u.type);
		const uint32_t stride    = aprend_uniform_array_stride(u.type);
		// Bounds are aprend_uniform_buffer_update's to check: a layout whose
		// offsets run past the buffer fails there.
		if (u.size <= 1 || stride == type_size)
			return aprend_uniform_buffer_update(buffer, u.offset, type_size * (u.size ? u.size : 1), pData);
		// An array whose elements are smaller than their std140 stride: the
		// caller's elements are tightly packed, the block's are not.
		for (uint32_t element = 0; element < u.size; ++element) {
			if (!aprend_uniform_buffer_update(buffer, u.offset + element * stride, type_size, (uint8_t *)pData + (size_t)element * type_size))
				return false;
		}
		return true;
	}
	return false; // no uniform of that name
}

uint32_t aprend_buffer_element_type_get_size(APREND_BUFFER_ELEMENT_TYPE type) {
	switch (type) {
	case APREND_BUFFER_ELEMENT_TYPE_FLOAT:
	case APREND_BUFFER_ELEMENT_TYPE_INT:
		return 4;
	case APREND_BUFFER_ELEMENT_TYPE_VEC2:
	case APREND_BUFFER_ELEMENT_TYPE_INT2:
		return 8;
	case APREND_BUFFER_ELEMENT_TYPE_VEC3:
	case APREND_BUFFER_ELEMENT_TYPE_INT3:
		return 12;
	case APREND_BUFFER_ELEMENT_TYPE_VEC4:
	case APREND_BUFFER_ELEMENT_TYPE_INT4:
		return 16;
	default:
		return 0;
	}
}

uint32_t aprend_buffer_layout_get_element_index(const aprend_buffer_layout *layout, const char *name) {
	if (!layout)
		return UINT32_MAX;
	if (!layout->elements)
		return UINT32_MAX;
	if (!name)
		return UINT32_MAX;
	for (uint32_t i = 0; i < layout->count; ++i) {
		const char *element_name = layout->elements[i].name;
		if (element_name && strcmp(element_name, name) == 0)
			return i;
	}
	return UINT32_MAX;
}

uint32_t aprend_buffer_layout_get_total_size(const aprend_buffer_layout *layout) {
	if (!layout)
		return 0;
	if (!layout->elements)
		return 0;
	uint32_t total_size = 0;
	for (uint32_t i = 0; i < layout->count; ++i) {
		uint32_t element_end = layout->elements[i].offset + layout->elements[i].size;
		if (element_end > total_size)
			total_size = element_end;
	}
	return total_size;
}

aprend_vertex_buffer aprend_vertex_buffer_create(aprend_instance instance, const aprend_buffer_layout *vertex_layout, uint32_t vertex_count, void *pData) {
	if (!instance)
		return nullptr;
	if (!vertex_layout)
		return nullptr;
	if (!vertex_layout->elements)
		return nullptr;
	if (vertex_layout->count == 0)
		return nullptr;
	if (vertex_count == 0)
		return nullptr;
	const uint32_t vertex_stride = aprend_buffer_layout_get_total_size(vertex_layout);
	if (vertex_stride == 0)
		return nullptr;

	aprend_vertex_buffer_t *result = (aprend_vertex_buffer_t *)malloc(sizeof(aprend_vertex_buffer_t));
	if (result)
		result = new (result) aprend_vertex_buffer_t();
	else
		return nullptr;

	result->vertex_count  = vertex_count;
	result->vertex_stride = vertex_stride;

	const uint64_t byte_size  = (uint64_t)vertex_stride * vertex_count;
	const size_t layout_bytes = (size_t)vertex_layout->count * sizeof(aprend_buffer_element);

	SPUDRESULT sr                  = SPUDRESULT_OUT_OF_MEMORY;
	result->vertex_layout.elements = (aprend_buffer_element *)malloc(layout_bytes);
	if (!result->vertex_layout.elements)
		goto failedattempt;
	memcpy(result->vertex_layout.elements, vertex_layout->elements, layout_bytes);
	result->vertex_layout.count = vertex_layout->count;

	sr = aprend_buffer_store_create(&result->store, instance, byte_size, SPUDGPU_BUFFER_USAGE_VERTEX, SPUDGPU_RESOURCE_STATE_VERTEX_BUFFER);
	if (SPUDFAIL(sr))
		goto failedattempt;

	result->buffer_view_desc.parent_buffer             = result->store.buffer;
	result->buffer_view_desc.offset_from_parent_buffer = 0;
	result->buffer_view_desc.stride                    = result->vertex_stride;
	result->buffer_view_desc.size                      = byte_size;
	sr = spudgpu_create_buffer_view(result->store.buffer, &result->buffer_view_desc, &result->buffer_view);
	if (SPUDFAIL(sr)) {
		result->buffer_view = nullptr;
		goto failedattempt;
	}

	if (pData) {
		sr = aprend_buffer_store_write(&result->store, 0, byte_size, pData);
		if (SPUDFAIL(sr))
			goto failedattempt;
	}

	return result;
failedattempt:
	printf("apricot: aprend_vertex_buffer_create failed: %s\n", spudresult_str(sr));
	result->~aprend_vertex_buffer_t();
	free(result);
	return nullptr;
}

void aprend_vertex_buffer_destroy(aprend_vertex_buffer buffer) {
	if (!buffer)
		return;
	buffer->~aprend_vertex_buffer_t();
	free(buffer);
}
spudgpu_buffer_view aprend_vertex_buffer_get_spudgpu_buffer_view(aprend_vertex_buffer buffer) { return buffer ? buffer->buffer_view : nullptr; }
bool aprend_vertex_buffer_update(aprend_vertex_buffer buffer, uint32_t vertex_offset, uint32_t vertex_count, void *pData) {
	if (!buffer)
		return false;
	if (vertex_count == 0)
		return false;
	if (!pData)
		return false;
	if (vertex_offset > buffer->vertex_count)
		return false;
	if (vertex_count > buffer->vertex_count - vertex_offset)
		return false;
	const uint64_t byte_offset = (uint64_t)vertex_offset * buffer->vertex_stride;
	const uint64_t byte_size   = (uint64_t)vertex_count * buffer->vertex_stride;
	return !SPUDFAIL(aprend_buffer_store_write(&buffer->store, byte_offset, byte_size, pData));
}
void aprend_vertex_buffer_get_layout(aprend_vertex_buffer buffer, aprend_buffer_layout *out_layout, uint32_t *out_vertex_count) {
	if (!buffer)
		return;
	if (out_layout)
		*out_layout = buffer->vertex_layout;
	if (out_vertex_count)
		*out_vertex_count = buffer->vertex_count;
}

aprend_index_buffer aprend_index_buffer_create(aprend_instance instance, APREND_INDEX_STRIDE stride, uint32_t index_count, void *pData) {
	if (!instance)
		return nullptr;
	if (stride != APREND_INDEX_STRIDE_UINT16 && stride != APREND_INDEX_STRIDE_UINT32)
		return nullptr;
	if (index_count == 0)
		return nullptr;

	aprend_index_buffer_t *result = (aprend_index_buffer_t *)malloc(sizeof(aprend_index_buffer_t));
	if (result)
		result = new (result) aprend_index_buffer_t();
	else
		return nullptr;

	result->index_count  = index_count;
	result->index_stride = stride;

	const uint64_t byte_size = (uint64_t)stride * index_count;

	SPUDRESULT sr = aprend_buffer_store_create(&result->store, instance, byte_size, SPUDGPU_BUFFER_USAGE_INDEX, SPUDGPU_RESOURCE_STATE_INDEX_BUFFER);
	if (SPUDFAIL(sr))
		goto failedattempt;

	result->buffer_view_desc.parent_buffer             = result->store.buffer;
	result->buffer_view_desc.offset_from_parent_buffer = 0;
	result->buffer_view_desc.stride                    = (uint64_t)result->index_stride;
	result->buffer_view_desc.size                      = byte_size;
	sr = spudgpu_create_buffer_view(result->store.buffer, &result->buffer_view_desc, &result->buffer_view);
	if (SPUDFAIL(sr)) {
		result->buffer_view = nullptr;
		goto failedattempt;
	}

	if (pData) {
		sr = aprend_buffer_store_write(&result->store, 0, byte_size, pData);
		if (SPUDFAIL(sr))
			goto failedattempt;
	}

	return result;

failedattempt:
	printf("apricot: aprend_index_buffer_create failed: %s\n", spudresult_str(sr));
	result->~aprend_index_buffer_t();
	free(result);
	return nullptr;
}
void aprend_index_buffer_destroy(aprend_index_buffer buffer) {
	if (buffer) {
		buffer->~aprend_index_buffer_t();
		free(buffer);
	}
}
spudgpu_buffer_view aprend_index_buffer_get_spudgpu_buffer_view(aprend_index_buffer buffer) { return buffer ? buffer->buffer_view : nullptr; }
bool aprend_index_buffer_update(aprend_index_buffer buffer, uint32_t index_offset, uint32_t index_count, void *pData) {
	if (!buffer)
		return false;
	if (index_count == 0)
		return false;
	if (!pData)
		return false;
	if (index_offset > buffer->index_count)
		return false;
	if (index_count > buffer->index_count - index_offset)
		return false;
	const uint64_t index_size  = (uint64_t)buffer->index_stride;
	const uint64_t byte_offset = index_offset * index_size;
	const uint64_t byte_size   = index_count * index_size;
	return !SPUDFAIL(aprend_buffer_store_write(&buffer->store, byte_offset, byte_size, pData));
}
APREND_INDEX_STRIDE aprend_index_buffer_get_stride(aprend_index_buffer buffer) { return buffer ? buffer->index_stride : APREND_INDEX_STRIDE_NONE; }
uint32_t aprend_index_buffer_get_index_count(aprend_index_buffer buffer) { return buffer ? buffer->index_count : 0; }

aprend_storage_buffer aprend_storage_buffer_create(aprend_instance instance, uint64_t size, void *pData) {
	if (!instance)
		return nullptr;
	if (size == 0)
		return nullptr;
	aprend_storage_buffer_t *result = (aprend_storage_buffer_t *)malloc(sizeof(aprend_storage_buffer_t));
	if (result)
		result = new (result) aprend_storage_buffer_t();
	else
		return nullptr;

	result->size = size;

	// UNORDERED_ACCESS: a storage buffer slot may be written by the shader as
	// well as read.
	SPUDRESULT sr = aprend_buffer_store_create(&result->store, instance, size, SPUDGPU_BUFFER_USAGE_STORAGE, SPUDGPU_RESOURCE_STATE_UNORDERED_ACCESS);
	if (SPUDFAIL(sr))
		goto failedattempt;

	result->buffer_view_desc.parent_buffer             = result->store.buffer;
	result->buffer_view_desc.offset_from_parent_buffer = 0;
	result->buffer_view_desc.stride                    = 0;
	result->buffer_view_desc.size                      = size;
	sr = spudgpu_create_buffer_view(result->store.buffer, &result->buffer_view_desc, &result->buffer_view);
	if (SPUDFAIL(sr)) {
		result->buffer_view = nullptr;
		goto failedattempt;
	}

	if (pData) {
		sr = aprend_buffer_store_write(&result->store, 0, size, pData);
		if (SPUDFAIL(sr))
			goto failedattempt;
	}

	return result;
failedattempt:
	printf("apricot: aprend_storage_buffer_create failed: %s\n", spudresult_str(sr));
	result->~aprend_storage_buffer_t();
	free(result);
	return nullptr;
}
void aprend_storage_buffer_destroy(aprend_storage_buffer buffer) {
	if (buffer) {
		buffer->~aprend_storage_buffer_t();
		free(buffer);
	}
}
bool aprend_storage_buffer_update(aprend_storage_buffer buffer, uint64_t local_offset, uint64_t size, void *pData) {
	if (!buffer)
		return false;
	if (size == 0)
		return false;
	if (!pData)
		return false;
	if (local_offset > buffer->size)
		return false;
	if (size > buffer->size - local_offset)
		return false;
	return !SPUDFAIL(aprend_buffer_store_write(&buffer->store, local_offset, size, pData));
}

aprend_uniform_buffer_set aprend_uniform_buffer_set_create(aprend_instance instance, const aprend_uniform_layout *layout) {
	if (!instance)
		return nullptr;
	if (!layout)
		return nullptr;
	aprend_uniform_buffer_set_t *result = (aprend_uniform_buffer_set_t *)malloc(sizeof(aprend_uniform_buffer_set_t));
	if (!result)
		return nullptr;
	result           = new (result) aprend_uniform_buffer_set_t();
	result->instance = instance;

	for (uint32_t i = 0; i < instance->desc.frames_in_flight; ++i) {
		result->buffers[i] = aprend_uniform_buffer_create(instance, layout);
		if (!result->buffers[i]) {
			result->~aprend_uniform_buffer_set_t();
			free(result);
			return nullptr;
		}
		++result->buffer_count;
	}
	return result;
}
void aprend_uniform_buffer_set_destroy(aprend_uniform_buffer_set set) {
	if (set) {
		set->~aprend_uniform_buffer_set_t();
		free(set);
	}
}
aprend_uniform_buffer aprend_uniform_buffer_set_get_buffer(aprend_uniform_buffer_set set, uint32_t frame_index) {
	if (!set)
		return nullptr;
	if (frame_index >= set->buffer_count)
		return nullptr;
	return set->buffers[frame_index];
}
bool aprend_uniform_buffer_set_update(aprend_uniform_buffer_set set, uint32_t local_offset, uint32_t size, void *pData) {
	if (!set)
		return false;
	return aprend_uniform_buffer_update(set->buffers[set->instance->frame_index], local_offset, size, pData);
}
bool aprend_uniform_buffer_set_update_by_name(aprend_uniform_buffer_set set, const char *name, void *pData) {
	if (!set)
		return false;
	return aprend_uniform_buffer_update_by_name(set->buffers[set->instance->frame_index], name, pData);
}

aprend_storage_buffer_set aprend_storage_buffer_set_create(aprend_instance instance, uint64_t size, void *pData) {
	if (!instance)
		return nullptr;
	if (size == 0)
		return nullptr;
	aprend_storage_buffer_set_t *result = (aprend_storage_buffer_set_t *)malloc(sizeof(aprend_storage_buffer_set_t));
	if (!result)
		return nullptr;
	result           = new (result) aprend_storage_buffer_set_t();
	result->instance = instance;

	for (uint32_t i = 0; i < instance->desc.frames_in_flight; ++i) {
		result->buffers[i] = aprend_storage_buffer_create(instance, size, pData);
		if (!result->buffers[i]) {
			result->~aprend_storage_buffer_set_t();
			free(result);
			return nullptr;
		}
		++result->buffer_count;
	}
	return result;
}
void aprend_storage_buffer_set_destroy(aprend_storage_buffer_set set) {
	if (set) {
		set->~aprend_storage_buffer_set_t();
		free(set);
	}
}
aprend_storage_buffer aprend_storage_buffer_set_get_buffer(aprend_storage_buffer_set set, uint32_t frame_index) {
	if (!set)
		return nullptr;
	if (frame_index >= set->buffer_count)
		return nullptr;
	return set->buffers[frame_index];
}
bool aprend_storage_buffer_set_update(aprend_storage_buffer_set set, uint64_t local_offset, uint64_t size, void *pData) {
	if (!set)
		return false;
	return aprend_storage_buffer_update(set->buffers[set->instance->frame_index], local_offset, size, pData);
}
}
