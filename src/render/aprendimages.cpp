
#include "render/aprendimages.h"
#include "apricot_renderer_internal.hpp"

extern "C" {
    aprend_texture2d aprend_texture2d_create(
        aprend_instance instance,
        uint32_t width,
        uint32_t height,
        SPUDGPU_FORMAT format,
        SPUDGPU_IMAGE_USAGE usage,
        SPUDGPU_MEMORY_FLAGS memory_flags)
    {
        /* Implementation-specific texture creation logic goes here. */
        if (!instance)
            return nullptr;
        ApricotRender::AprendTexture2D *result = new ApricotRender::AprendTexture2D();
        result->instance = *reinterpret_cast<ApricotRender::AprendInstance *>(instance);
        result->image_desc = {
            .type = SPUDGPU_IMAGE_TYPE_2D,
            .format = format,
            .width = width,
            .height = height,
            .depth = 1,
            .array_layers = 1,
            .mip_levels = 1,
            .usage = (SPUDGPU_IMAGE_USAGE) usage,
            .memory_flags = memory_flags,
        };

        result->image = spudgpu_create_image(
            result->instance.desc.device,
            &result->image_desc);
        if (!result->image)
        {
            delete result;
            return nullptr;
        }
        return reinterpret_cast<aprend_texture2d>(result);
    }
    void aprend_texture2d_destroy(
        aprend_texture2d texture)
    {
        if (!texture)
            return;
        ApricotRender::AprendTexture2D *tex = reinterpret_cast<ApricotRender::AprendTexture2D *>(texture);
        spudgpu_destroy_image(tex->instance.desc.device, tex->image);
        delete tex;
    }
    bool aprend_texture2d_update(
        aprend_texture2d texture,
        uint32_t x_offset,
        uint32_t y_offset,
        uint32_t width,
        uint32_t height,
        void **ppData)
    {
        if (!texture || !ppData)
            return false;
        ApricotRender::AprendTexture2D *tex = reinterpret_cast<ApricotRender::AprendTexture2D *>(texture);
        /* Implementation-specific texture update logic goes here. */
        return false; // Not implemented yet
    }
    bool aprend_texture2d_get_data(
        aprend_texture2d texture,
        uint32_t x_offset,
        uint32_t y_offset,
        uint32_t width,
        uint32_t height,
        void **ppData)
    {
        if (!(texture && ppData && width && height))
            return false;
        ApricotRender::AprendTexture2D *tex = reinterpret_cast<ApricotRender::AprendTexture2D *>(texture);
        if (x_offset + width > tex->image_desc.width || y_offset + height > tex->image_desc.height)
            return false;
        /* Implementation-specific texture readback logic goes here. */
        return false; // Not implemented yet
    }
    bool aprend_texture2d_resize(
        aprend_texture2d texture,
        uint32_t new_width,
        uint32_t new_height)
    {
        if (!(texture && new_width && new_height))
            return false;
        ApricotRender::AprendTexture2D *tex = reinterpret_cast<ApricotRender::AprendTexture2D *>(texture);
        if (new_width == tex->image_desc.width && new_height == tex->image_desc.height)
            return true; // No resize needed
        /* Implementation-specific texture resizing logic goes here. */
        return false; // Not implemented yet
}
