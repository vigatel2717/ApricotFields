
#include "render/aprendbuffers.h"
#include "apricot_renderer_internal.hpp"

#include <spudgpu.h>

namespace ApricotRender
{
    bool AprendBufferContext::AvailableSpace(uint32_t offset, uint32_t size) const
    {
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

    uint32_t AprendBufferContext::GetEndAvailableOffset()
    {
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
}

extern "C"
{

    aprend_buffer_context aprend_buffer_context_create(
        aprend_instance instance,
        uint64_t initial_capacity,
        SPUDGPU_BUFFER_USAGE usage,
        SPUDGPU_MEMORY_FLAGS flags)
    {
        /* Implementation-specific buffer context creation logic goes here. */
        if (!instance)
            return nullptr;
        ApricotRender::AprendBufferContext *result = new ApricotRender::AprendBufferContext();
        result->instance = *reinterpret_cast<ApricotRender::AprendInstance *>(instance);

        result->buffer = spudgpu_create_buffer(
            result->instance.desc.device,
            &(result->buffer_desc = {
                  .usage = usage,
                  .memory_flags = flags,
                  .size = initial_capacity,
              }));
        if (!result->buffer)
        {
            delete result;
            return nullptr;
        }

        return reinterpret_cast<aprend_buffer_context>(result);
    }
    void aprend_buffer_context_destroy(
        aprend_buffer_context context)
    {
        if (!context)
            return;
        ApricotRender::AprendBufferContext *ctx = reinterpret_cast<ApricotRender::AprendBufferContext *>(context);
        spudgpu_destroy_buffer(ctx->instance.desc.device, ctx->buffer);
        delete ctx;
    }
    uint64_t aprend_buffer_context_get_current_capacity(
        aprend_buffer_context context)
    {
        if (!context)
            return 0;
        ApricotRender::AprendBufferContext *ctx = reinterpret_cast<ApricotRender::AprendBufferContext *>(context);
        if (!ctx->buffer)
            return 0;
        spudgpu_buffer_desc desc = spudgpu_get_buffer_desc(ctx->buffer);
        return desc.size;
    }
    uint64_t aprend_buffer_context_shrink_to_fit(
        aprend_buffer_context context)
    {
        /* Implementation-specific buffer context shrink-to-fit logic goes here. */
        return context ? aprend_buffer_context_get_current_capacity(context) : 0;
    }

    aprend_uniform_buffer aprend_uniform_buffer_create(
        aprend_buffer_context context,
        const aprend_uniform_layout *layout)
    {
        if (!(context && layout))
            return nullptr;
        ApricotRender::AprendBufferContext *ctx = reinterpret_cast<ApricotRender::AprendBufferContext *>(context);
        ApricotRender::AprendUniformBuffer *result = new ApricotRender::AprendUniformBuffer();
        result->context = *ctx;
        result->layout = *layout;
        for (size_t i = 0; i < layout->count; ++i)
        {
            const aprend_uniform &u = layout->uniforms[i];
            uint32_t uniform_size = aprend_uniform_type_get_size(u.type) * u.size;
            uint32_t padding = (uniform_size % 16) ? (16 - (uniform_size % 16)) : 0; // std140-like alignment
            result->buffer_view_desc.size += uniform_size + padding;
        }
        return reinterpret_cast<aprend_uniform_buffer>(result);
    }
    void aprend_uniform_buffer_destroy(
        aprend_uniform_buffer buffer)
    {
        if (!buffer)
            return;
        ApricotRender::AprendUniformBuffer *buf = reinterpret_cast<ApricotRender::AprendUniformBuffer *>(buffer);
        spudgpu_destroy_buffer_view(buf->buffer_view);
        delete buf;
    }
    aprend_buffer_context aprend_uniform_buffer_get_buffer_context(
        aprend_uniform_buffer buffer)
    {
        if (!buffer)
            return nullptr;
        else
            return reinterpret_cast<aprend_buffer_context>(
                &reinterpret_cast<ApricotRender::AprendUniformBuffer *>(buffer)->context);
    }
    bool aprend_uniform_buffer_update(
        aprend_uniform_buffer buffer,
        uint32_t local_offset,
        uint32_t size,
        void **ppData)
    {
        if (!buffer || !size)
            return false;
        ApricotRender::AprendUniformBuffer *buf = reinterpret_cast<ApricotRender::AprendUniformBuffer *>(buffer);
        uint32_t byte_offset = buf->buffer_view_desc.offset_from_parent_buffer + local_offset;
        return spudgpu_map_buffer(
            buf->context.buffer,
            byte_offset,
            size,
            ppData);
    }
    bool aprend_uniform_buffer_update_by_name(
        aprend_uniform_buffer buffer,
        const char *name,
        void **ppData)
    {
        return false; // Not implemented yet
    }

    aprend_vertex_buffer aprend_vertex_buffer_create(
        aprend_buffer_context context,
        const aprend_buffer_layout *vertex_layout,
        uint32_t vertex_count,
        void **ppData)
    {
        if (!(context && vertex_layout && vertex_count))
            return nullptr;
        ApricotRender::AprendBufferContext *ctx = reinterpret_cast<ApricotRender::AprendBufferContext *>(context);
        ApricotRender::AprendVertexBuffer *result = new ApricotRender::AprendVertexBuffer();
        result->context = *ctx;
        result->vertex_layout = *vertex_layout;
        result->vertex_count = vertex_count;
        result->vertex_stride = aprend_buffer_layout_get_total_size(vertex_layout);
        result->buffer_view_desc = {
            .parent_buffer = ctx->buffer,
            .offset_from_parent_buffer = 0, // This will be set to the correct offset in the buffer when the buffer is allocated and assigned a GPU address.
            .stride = result->vertex_stride,
            .size = result->vertex_count * result->vertex_stride,
        };
        result->buffer_view = spudgpu_create_buffer_view(
            ctx->buffer,
            &result->buffer_view_desc);
        if (!result->buffer_view)
        {
            delete result;
            return nullptr;
        }
        if (ppData)
        {
            void *data_ptr = nullptr;
            if (spudgpu_map_buffer(
                    ctx->buffer,
                    result->buffer_view_desc.offset_from_parent_buffer,
                    result->buffer_view_desc.size,
                    &data_ptr))
            {
                *ppData = data_ptr;
            }
            else
            {
                spudgpu_destroy_buffer_view(result->buffer_view);
                delete result;
                return nullptr;
            }
        }
        return reinterpret_cast<aprend_vertex_buffer>(result);
    }

    void aprend_vertex_buffer_destroy(
        aprend_vertex_buffer buffer)
    {
        if (!buffer)
            return;
        ApricotRender::AprendVertexBuffer *buf = reinterpret_cast<ApricotRender::AprendVertexBuffer *>(buffer);
        spudgpu_destroy_buffer_view(buf->buffer_view);
        delete buf;
    }
    bool aprend_vertex_buffer_update(
        aprend_vertex_buffer buffer,
        uint32_t vertex_offset,
        uint32_t vertex_count,
        void **ppData)
    {
        if (!buffer || !vertex_count)
            return false;
        ApricotRender::AprendVertexBuffer *buf = reinterpret_cast<ApricotRender::AprendVertexBuffer *>(buffer);
        uint32_t byte_offset = buf->buffer_view_desc.offset_from_parent_buffer + (vertex_offset * buf->vertex_stride);
        uint32_t byte_size = vertex_count * buf->vertex_stride;
        return spudgpu_map_buffer(
            buf->context.buffer,
            byte_offset,
            byte_size,
            ppData);
    }
    void aprend_vertex_buffer_get_layout(
        aprend_vertex_buffer buffer,
        aprend_buffer_layout *out_layout,
        uint32_t *out_vertex_count)
    {
        if (!buffer)
            return;
        ApricotRender::AprendVertexBuffer *buf = reinterpret_cast<ApricotRender::AprendVertexBuffer *>(buffer);
        if (out_layout)
            *out_layout = buf->vertex_layout;
        if (out_vertex_count)
            *out_vertex_count = buf->vertex_count;
    }

    aprend_index_buffer aprend_index_buffer_create(
        aprend_buffer_context context,
        bool index_32bit,
        uint32_t index_count,
        void **ppData)
    {
        if (!(context && index_count))
            return nullptr;

        ApricotRender::AprendBufferContext *ctx = reinterpret_cast<ApricotRender::AprendBufferContext *>(context);
        ApricotRender::AprendIndexBuffer *result = new ApricotRender::AprendIndexBuffer();
        result->context = *ctx;
        result->index_count = index_count;
        result->index_32bit = index_32bit;
        uint32_t index_size = index_32bit ? sizeof(uint32_t) : sizeof(uint16_t);
        result->buffer_view_desc = {
            .parent_buffer = ctx->buffer,
            .offset_from_parent_buffer = 0, // This will be set to the correct offset in the buffer when the buffer is allocated and assigned a GPU address.
            .stride = index_size,
            .size = result->index_count * index_size,
        };
        result->buffer_view = spudgpu_create_buffer_view(
            ctx->buffer,
            &result->buffer_view_desc);
        if (!result->buffer_view)
        {
            delete result;
            return nullptr;
        }
        if (ppData)
        {
            void *data_ptr = nullptr;
            if (spudgpu_map_buffer(
                    ctx->buffer,
                    result->buffer_view_desc.offset_from_parent_buffer,
                    result->buffer_view_desc.size,
                    &data_ptr))
            {
                *ppData = data_ptr;
            }
            else
            {
                spudgpu_destroy_buffer_view(result->buffer_view);
                delete result;
                return nullptr;
            }
            spudgpu_unmap_buffer(ctx->buffer);
        }

        return reinterpret_cast<aprend_index_buffer>(result);
    }
    void aprend_index_buffer_destroy(
        aprend_index_buffer buffer)
    {
        if (!buffer)
            return;
        ApricotRender::AprendIndexBuffer *buf = reinterpret_cast<ApricotRender::AprendIndexBuffer *>(buffer);
        spudgpu_destroy_buffer_view(buf->buffer_view);
        delete buf;
    }
    bool aprend_index_buffer_update(
        aprend_index_buffer buffer,
        uint32_t index_offset,
        uint32_t index_count,
        void **ppData)
    {
        if (!(buffer && index_count && ppData))
            return false;
        ApricotRender::AprendIndexBuffer *buf = reinterpret_cast<ApricotRender::AprendIndexBuffer *>(buffer);
        uint32_t index_size = buf->index_32bit ? sizeof(uint32_t) : sizeof(uint16_t);
        uint32_t byte_offset = buf->buffer_view_desc.offset_from_parent_buffer + (index_offset * index_size);
        uint32_t byte_size = index_count * index_size;
        void *data_ptr = nullptr;
        if (!spudgpu_map_buffer(
                buf->context.buffer,
                byte_offset,
                byte_size,
                &data_ptr))
            return false;
        memcpy(data_ptr, *ppData, byte_size);
        spudgpu_unmap_buffer(buf->context.buffer);
        return true;
    }
    bool aprend_index_buffer_is_32bit(
        aprend_index_buffer buffer)
    {
        if (!buffer)
            return false;
        else
            return reinterpret_cast<ApricotRender::AprendIndexBuffer *>(buffer)->index_32bit;
    }

    aprend_storage_buffer aprend_storage_buffer_create(
        aprend_buffer_context context,
        uint64_t size,
        void **ppData)
    {
        if (!(context && size))
            return nullptr;
        ApricotRender::AprendBufferContext *ctx = reinterpret_cast<ApricotRender::AprendBufferContext *>(context);
        ApricotRender::AprendStorageBuffer *result = new ApricotRender::AprendStorageBuffer();
        result->context = *ctx;
        result->buffer_view_desc = {
            .parent_buffer = ctx->buffer,
            .offset_from_parent_buffer = 0, // This will be set to the correct offset in the buffer when the buffer is allocated and assigned a GPU address.
            .stride = 0,
            .size = size,
        };
        result->buffer_view = spudgpu_create_buffer_view(
            ctx->buffer,
            &result->buffer_view_desc);
        if (!result->buffer_view)
        {
            delete result;
            return nullptr;
        }
        if (ppData)
        {
            void *data_ptr = nullptr;
            if (spudgpu_map_buffer(
                    ctx->buffer,
                    result->buffer_view_desc.offset_from_parent_buffer,
                    result->buffer_view_desc.size,
                    &data_ptr))
            {
                memcpy(data_ptr, *ppData, size);
            }
            else
            {
                spudgpu_destroy_buffer_view(result->buffer_view);
                delete result;
                return nullptr;
            }
            spudgpu_unmap_buffer(ctx->buffer);
        }
        return reinterpret_cast<aprend_storage_buffer>(result);
    }
    void aprend_storage_buffer_destroy(
        aprend_storage_buffer buffer)
    {
        if (!buffer)
            return;
        ApricotRender::AprendStorageBuffer *buf = reinterpret_cast<ApricotRender::AprendStorageBuffer *>(buffer);
        spudgpu_destroy_buffer_view(buf->buffer_view);
        delete buf;
    }
    bool aprend_storage_buffer_update(
        aprend_storage_buffer buffer,
        uint64_t local_offset,
        uint64_t size,
        void **ppData)
    {
        if (!(buffer && size && ppData))
            return false;
        ApricotRender::AprendStorageBuffer *buf = reinterpret_cast<ApricotRender::AprendStorageBuffer *>(buffer);
        uint64_t byte_offset = buf->buffer_view_desc.offset_from_parent_buffer + local_offset;
        void *data_ptr = nullptr;
        if (!spudgpu_map_buffer(
                buf->context.buffer,
                byte_offset,
                size,
                &data_ptr))
            return false;
        memcpy(data_ptr, *ppData, size);
        spudgpu_unmap_buffer(buf->context.buffer);
        return true;
    }
}
