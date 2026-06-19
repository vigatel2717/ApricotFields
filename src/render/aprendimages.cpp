
#include "render/aprendimages.h"
#include "aprend_internal.hpp"

extern "C" {
    aprend_texture2d aprend_texture2d_create(
        aprend_instance instance,
        const aprend_texture_desc *desc)
    {
        /* Implementation-specific texture creation logic goes here. */
        if (!(instance && desc && desc->width && desc->height && desc->format && desc->type == SPUDGPU_IMAGE_TYPE_2D))
            return nullptr;
        aprend_texture2d_t *result = (aprend_texture2d_t *) calloc(1, sizeof(aprend_texture2d_t));
        result->instance = instance;
        result->image_desc = {
            .usage = (SPUDGPU_IMAGE_USAGE) desc->usage,
            .type = SPUDGPU_IMAGE_TYPE_2D,
            .format = desc->format,
            .width = desc->width,
            .height = desc->height,
            .depth = 1,
            .array_layers = 1,
            .mip_levels = 1,
            .memory_flags = desc->memory_flags,
        };

        if (spudgpu_create_image(
            result->instance->desc.device,
            &result->image_desc,
            &result->image) != SPUD_SUCCESS)
        {
            free(result);
            return nullptr;
        }
        return result;
    }
    void aprend_texture2d_destroy(
        aprend_texture2d texture)
    {
        spudgpu_destroy_image(texture->image);
        free(texture);
    }
    bool aprend_texture2d_update(
        aprend_texture2d texture,
        uint32_t x_offset,
        uint32_t y_offset,
        uint32_t width,
        uint32_t height,
        void **ppData)
    {
        if (!(texture && ppData && width && height))
            return false;
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
        if (x_offset + width > texture->image_desc.width || y_offset + height > texture->image_desc.height)
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
        if (new_width == texture->image_desc.width && new_height == texture->image_desc.height)
            return true; // No resize needed
        /* Implementation-specific texture resizing logic goes here. */
        return false; // Not implemented yet
    }
}
