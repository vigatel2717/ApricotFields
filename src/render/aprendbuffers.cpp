
#include "render/aprendbuffers.h"
#include "aprend_internal.hpp"

#include <spudgpu.h>

bool aprend_buffer_context_t::AvailableSpace(uint32_t offset, uint32_t size) const {
	/*bool available = true;
	for (size_t i=0;i<allocated_ranges.size();++i)
	{
	    const AprendBufferContextRange &range = allocated_ranges[i];
	    if (offset + size <= range.offset)
	        available = true;
	}
	return available;*/
	return false; // Not implemented yet
}

uint32_t aprend_buffer_context_t::GetEndAvailableOffset() {
	/*uint32_t end_offset = 0;
	for (size_t i=0;i<allocated_ranges.size();++i)
	{
	    const AprendBufferContextRange &range = allocated_ranges[i];
	    if (range.offset + range.size > end_offset)
	        end_offset = range.offset + range.size;
	}
	return end_offset;*/
	return 0; // Not implemented yet
}

aprend_uniform_buffer_t::~aprend_uniform_buffer_t() {
	spudgpu_unmap_buffer(this->buffer);
	spudgpu_destroy_buffer_view(this->buffer_view);
	spudgpu_destroy_buffer(this->buffer);
    free(this->layout.uniforms);
}

aprend_vertex_buffer_t::~aprend_vertex_buffer_t() {
	spudgpu_destroy_buffer_view(this->buffer_view);
	spudgpu_destroy_buffer(this->buffer);
    free(this->vertex_layout.elements);
}

aprend_index_buffer_t::~aprend_index_buffer_t() {
	spudgpu_destroy_buffer_view(this->buffer_view);
	spudgpu_destroy_buffer(this->buffer);
}

aprend_storage_buffer_t::~aprend_storage_buffer_t() {
	spudgpu_destroy_buffer_view(this->buffer_view);
	spudgpu_destroy_buffer(this->buffer);
}

extern "C" {

aprend_buffer_context
aprend_buffer_context_create(aprend_instance instance, uint64_t initial_capacity, SPUDGPU_BUFFER_USAGE usage, SPUDGPU_MEMORY_FLAGS flags) {
	/* Implementation-specific buffer context creation logic goes here. */
	if (!instance)
		return nullptr;
	aprend_buffer_context_t *result = (aprend_buffer_context_t *)calloc(1, sizeof(aprend_buffer_context_t));
	result->instance                = instance;

	if (spudgpu_create_buffer(
	        result->instance->desc.device,
	        &(result->buffer_desc =
	              {
	                  .usage        = usage,
	                  .memory_flags = flags,
	                  .size         = initial_capacity,
	              }),
	        &result->buffer) != SPUD_SUCCESS) {
		free(result);
		return nullptr;
	}

	return result;
}
void aprend_buffer_context_destroy(aprend_buffer_context context) {
	if (!context)
		return;
	spudgpu_destroy_buffer(context->buffer);
	free(context);
}
uint64_t aprend_buffer_context_get_current_capacity(aprend_buffer_context context) {
	if (context) {
		if (context->buffer) {
			spudgpu_buffer_desc desc = {0};
			spudgpu_get_buffer_desc(context->buffer, &desc);
			return desc.size;
		}
	}
	return 0;
}
uint64_t aprend_buffer_context_shrink_to_fit(aprend_buffer_context context) {
	/* Implementation-specific buffer context shrink-to-fit logic goes here. */
	return context ? aprend_buffer_context_get_current_capacity(context) : 0;
}

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
aprend_uniform_buffer aprend_uniform_buffer_create(aprend_instance instance, const aprend_uniform_layout *layout) {
	if (!(instance && layout))
		return nullptr;
	aprend_uniform_buffer_t *result = (aprend_uniform_buffer_t *)malloc(sizeof(aprend_uniform_buffer_t));
	if (result)
		result = new (result) aprend_uniform_buffer_t();
	else
		return NULL;

    SPUDRESULT sr = SPUD_SUCCESS;
	result->layout.uniforms = (aprend_uniform *)malloc(sizeof(aprend_uniform) * layout->count);
	if (!result->layout.uniforms)
		goto failedattempt;
	result->layout.count = layout->count;
	memcpy(result->layout.uniforms, layout->uniforms, sizeof(aprend_uniform) * layout->count);
	result->total_size = 0;
	for (size_t i = 0; i < layout->count; ++i) {
		const aprend_uniform &u = layout->uniforms[i];
		uint32_t uniform_size   = aprend_uniform_type_get_size(u.type) * u.size;
		uint32_t padding        = (uniform_size % 16) ? (16 - (uniform_size % 16)) : 0; // std140-like alignment
		result->buffer_view_desc.size += uniform_size + padding;
		result->total_size += uniform_size;
	}

	{
		spudgpu_buffer_desc bd;
		bd.buffer_flags = SPUDGPU_BUFFER_FLAG_NONE;
		bd.heap_flags   = SPUDGPU_HEAP_FLAG_NONE;
		bd.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT | SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;
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
    printf(spudresult_str(sr));
	result->~aprend_uniform_buffer_t();
	free(result);
	return NULL;
}
void aprend_uniform_buffer_destroy(aprend_uniform_buffer buffer) {
	if (!buffer)
		return;
	buffer->~aprend_uniform_buffer_t();
	free(buffer);
}
spudgpu_buffer_view aprend_uniform_buffer_get_spudgpu_buffer_view(aprend_uniform_buffer buffer) { return buffer ? buffer->buffer_view : NULL; }
aprend_buffer_context aprend_uniform_buffer_get_buffer_context(aprend_uniform_buffer buffer) { return nullptr; }
bool aprend_uniform_buffer_update(aprend_uniform_buffer buffer, uint32_t local_offset, uint32_t size, void *pData) {
	if (!(buffer && size))
		return false;
	uint32_t byte_offset = buffer->buffer_view_desc.offset_from_parent_buffer + local_offset;
	memcpy((uint8_t *)buffer->uniform_data_ptr + byte_offset, pData, size);
	return true;
}
bool aprend_uniform_buffer_update_by_name(aprend_uniform_buffer buffer, const char *name, void *pData) {
	return false; // Not implemented yet
}

uint32_t aprend_buffer_layout_get_total_size(const aprend_buffer_layout *layout) {
	if (!(layout && layout->elements))
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
	if (!(instance && vertex_layout && vertex_count))
		return NULL;
	aprend_vertex_buffer_t *result = (aprend_vertex_buffer_t *)malloc(sizeof(aprend_vertex_buffer_t));
	if (result)
		result = new (result) aprend_vertex_buffer_t();
	else
		return NULL;

	result->vertex_count  = vertex_count;
	result->vertex_stride = aprend_buffer_layout_get_total_size(vertex_layout);

	const size_t layout_bytes       = (size_t)vertex_layout->count * sizeof(aprend_buffer_element);
	result->vertex_layout.elements  = (aprend_buffer_element *)malloc(layout_bytes);
	memcpy(result->vertex_layout.elements, vertex_layout->elements, layout_bytes);
	result->vertex_layout.count = vertex_layout->count;

	SPUDRESULT sr = SPUD_SUCCESS;
	{
		spudgpu_buffer_desc bd;
		bd.buffer_flags = SPUDGPU_BUFFER_FLAG_NONE;
		bd.heap_flags   = SPUDGPU_HEAP_FLAG_NONE;
		bd.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT | SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;
		bd.size         = result->vertex_stride * result->vertex_count;
		bd.usage        = SPUDGPU_BUFFER_USAGE_VERTEX;
		sr = spudgpu_create_buffer(instance->desc.device, &bd, &result->buffer);
		if (sr != SPUD_SUCCESS)
			goto failedattempt;

		result->buffer_view_desc.parent_buffer = result->buffer;
		result->buffer_view_desc.offset_from_parent_buffer =
		    0; // This will be set to the correct offset in the buffer when the buffer is allocated and assigned a GPU address.
		result->buffer_view_desc.stride = result->vertex_stride;
		result->buffer_view_desc.size   = result->vertex_count * result->vertex_stride;
		sr = spudgpu_create_buffer_view(result->buffer, &result->buffer_view_desc, &result->buffer_view);
		if (sr != SPUD_SUCCESS)
			goto failedattempt;

		if (pData) {
			void *data_ptr = nullptr;
			sr = spudgpu_map_buffer(result->buffer, result->buffer_view_desc.offset_from_parent_buffer, result->buffer_view_desc.size, &data_ptr);
			if (sr != SPUD_SUCCESS)
				goto failedattempt;

			memcpy(data_ptr, pData, result->buffer_view_desc.size);
			spudgpu_unmap_buffer(result->buffer);
		}
	}

	return result;
failedattempt:
	printf(spudresult_str(sr));
	result->~aprend_vertex_buffer_t();
	free(result);
	return NULL;
}

void aprend_vertex_buffer_destroy(aprend_vertex_buffer buffer) {
	if (!buffer)
		return;
	buffer->~aprend_vertex_buffer_t();
	free(buffer);
}
spudgpu_buffer_view aprend_vertex_buffer_get_spudgpu_buffer_view(aprend_vertex_buffer buffer) { return buffer ? buffer->buffer_view : NULL; }
bool aprend_vertex_buffer_update(aprend_vertex_buffer buffer, uint32_t vertex_offset, uint32_t vertex_count, void *pData) {
	if (!(buffer && vertex_count))
		return false;
	uint32_t byte_offset = buffer->buffer_view_desc.offset_from_parent_buffer + (vertex_offset * buffer->vertex_stride);
	uint32_t byte_size   = vertex_count * buffer->vertex_stride;

	void *cpuAddress = nullptr;
	if (spudgpu_map_buffer(buffer->buffer, byte_offset, byte_size, &cpuAddress) != SPUD_SUCCESS)
		return false;
	memcpy(cpuAddress, pData, byte_size);
	spudgpu_unmap_buffer(buffer->buffer);
	return true;
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
	if (!(instance && index_count))
		return NULL;

	aprend_index_buffer_t *result = (aprend_index_buffer_t *)malloc(sizeof(aprend_index_buffer_t));
	if (result)
		result = new (result) aprend_index_buffer_t();
	else
		return NULL;

	result->index_count  = index_count;
	result->index_stride = stride;

	SPUDRESULT sr = SPUD_SUCCESS;
	{
		spudgpu_buffer_desc bd;
		bd.buffer_flags = SPUDGPU_BUFFER_FLAG_NONE;
		bd.heap_flags   = SPUDGPU_HEAP_FLAG_NONE;
		bd.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT | SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;
		bd.size         = stride * index_count;
		bd.usage        = SPUDGPU_BUFFER_USAGE_INDEX;
		sr = spudgpu_create_buffer(instance->desc.device, &bd, &result->buffer);
		if (sr != SPUD_SUCCESS)
			goto failedattempt;

		result->buffer_view_desc.parent_buffer = result->buffer;
		result->buffer_view_desc.offset_from_parent_buffer =
		    0; // This will be set to the correct offset in the buffer when the buffer is allocated and assigned a GPU address.
		result->buffer_view_desc.stride = (uint64_t)result->index_stride;
		result->buffer_view_desc.size   = result->index_count * (uint64_t)result->index_stride;

		sr = spudgpu_create_buffer_view(result->buffer, &result->buffer_view_desc, &result->buffer_view);
		if (sr != SPUD_SUCCESS)
			goto failedattempt;
		
		if (pData) {
			void *data_ptr = nullptr;
			sr = spudgpu_map_buffer(result->buffer, result->buffer_view_desc.offset_from_parent_buffer, result->buffer_view_desc.size, &data_ptr);
			if (sr != SPUD_SUCCESS) {
				goto failedattempt;
			}
			memcpy(data_ptr, pData, (size_t)result->buffer_view_desc.size);
			spudgpu_unmap_buffer(result->buffer);
		}
	}

	return result;

failedattempt:
	printf(spudresult_str(sr));
	result->~aprend_index_buffer_t();
	free(result);
	return NULL;
}
void aprend_index_buffer_destroy(aprend_index_buffer buffer) {
	if (!buffer)
		return;
	buffer->~aprend_index_buffer_t();
	free(buffer);
}
spudgpu_buffer_view aprend_index_buffer_get_spudgpu_buffer_view(aprend_index_buffer buffer) { return buffer ? buffer->buffer_view : NULL; }
bool aprend_index_buffer_update(aprend_index_buffer buffer, uint32_t index_offset, uint32_t index_count, void *pData) {
	if (!(buffer && index_count))
		return false;
	uint32_t index_size  = buffer->index_stride;
	uint32_t byte_offset = buffer->buffer_view_desc.offset_from_parent_buffer + (index_offset * index_size);
	uint32_t byte_size   = index_count * index_size;

	void *data_ptr = nullptr;
	if (spudgpu_map_buffer(buffer->buffer, byte_offset, byte_size, &data_ptr) != SPUD_SUCCESS)
		return false;
	memcpy(data_ptr, pData, byte_size);
	spudgpu_unmap_buffer(buffer->buffer);
	return true;
}
uint32_t aprend_index_buffer_get_format(aprend_index_buffer buffer) { return buffer ? buffer->index_stride : APREND_INDEX_STRIDE_NONE; }

aprend_storage_buffer aprend_storage_buffer_create(aprend_instance instance, uint64_t size, void *pData) {
	if (!(instance && size))
		return NULL;
	aprend_storage_buffer_t *result = (aprend_storage_buffer_t *)malloc(sizeof(aprend_storage_buffer_t));
	if (result)
		result = new (result) aprend_storage_buffer_t();
	else
		return NULL;

	{
		spudgpu_buffer_desc bd;
		bd.buffer_flags = SPUDGPU_BUFFER_FLAG_NONE;
		bd.heap_flags   = SPUDGPU_HEAP_FLAG_NONE;
		bd.memory_flags = SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;
		bd.size         = size;
		bd.usage        = SPUDGPU_BUFFER_USAGE_STORAGE;
		if (spudgpu_create_buffer(instance->desc.device, &bd, &result->buffer) != SPUD_SUCCESS)
			goto failedattempt;

		result->buffer_view_desc.parent_buffer = result->buffer;
		result->buffer_view_desc.offset_from_parent_buffer =
		    0; // This will be set to the correct offset in the buffer when the buffer is allocated and assigned a GPU address.
		result->buffer_view_desc.stride = 0;
		result->buffer_view_desc.size   = size;
		if (spudgpu_create_buffer_view(result->buffer, &result->buffer_view_desc, &result->buffer_view) != SPUD_SUCCESS)
			goto failedattempt;

		if (pData) {
			void *data_ptr = nullptr;
			if (spudgpu_map_buffer(result->buffer, result->buffer_view_desc.offset_from_parent_buffer, result->buffer_view_desc.size, &data_ptr) !=
			    SPUD_SUCCESS)
				goto failedattempt;

			memcpy(data_ptr, pData, size);
			spudgpu_unmap_buffer(result->buffer);
		}
	}

	return result;
failedattempt:
	result->~aprend_storage_buffer_t();
	free(result);
	return NULL;
}
void aprend_storage_buffer_destroy(aprend_storage_buffer buffer) {
	if (!buffer)
		return;
	buffer->~aprend_storage_buffer_t();
	free(buffer);
}
bool aprend_storage_buffer_update(aprend_storage_buffer buffer, uint64_t local_offset, uint64_t size, void *pData) {
	if (!(buffer && size && pData))
		return false;
	uint64_t byte_offset = buffer->buffer_view_desc.offset_from_parent_buffer + local_offset;
	void *data_ptr       = nullptr;
	if (!spudgpu_map_buffer(buffer->buffer, byte_offset, size, &data_ptr))
		return false;
	memcpy(data_ptr, pData, size);
	spudgpu_unmap_buffer(buffer->buffer);
	return true;
}
}
