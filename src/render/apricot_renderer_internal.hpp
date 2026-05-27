#pragma once

/* Internal header — never included outside ApricotFields/src/render/.
 * Defines the concrete structs behind every opaque handle in apricot_renderer.h
 * and provides zero-cost GLM conversion helpers. */

#include "../../include/render/apricot_renderer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>


/* ============================================================
   GLM conversion helpers
   Reinterpret-cast is valid because ApriXxx structs are laid out
   identically to their GLM counterparts (same member order, same types).
   ============================================================ */

inline glm::vec2&        to_glm(ApriVec2&  v) { return reinterpret_cast<glm::vec2&>(v);  }
inline glm::vec3&        to_glm(ApriVec3&  v) { return reinterpret_cast<glm::vec3&>(v);  }
inline glm::vec4&        to_glm(ApriVec4&  v) { return reinterpret_cast<glm::vec4&>(v);  }
inline glm::dvec3&       to_glm(ApriDVec3& v) { return reinterpret_cast<glm::dvec3&>(v); }
inline glm::dvec4&       to_glm(ApriDVec4& v) { return reinterpret_cast<glm::dvec4&>(v); }
inline glm::quat&        to_glm(ApriQuat&  v) { return reinterpret_cast<glm::quat&>(v);  }
inline glm::mat4&        to_glm(ApriMat4&  v) { return reinterpret_cast<glm::mat4&>(v);  }
inline glm::dmat4&       to_glm(ApriDMat4& v) { return reinterpret_cast<glm::dmat4&>(v); }

inline const glm::vec2&  to_glm(const ApriVec2&  v) { return reinterpret_cast<const glm::vec2&>(v);  }
inline const glm::vec3&  to_glm(const ApriVec3&  v) { return reinterpret_cast<const glm::vec3&>(v);  }
inline const glm::vec4&  to_glm(const ApriVec4&  v) { return reinterpret_cast<const glm::vec4&>(v);  }
inline const glm::dvec3& to_glm(const ApriDVec3& v) { return reinterpret_cast<const glm::dvec3&>(v); }
inline const glm::quat&  to_glm(const ApriQuat&  v) { return reinterpret_cast<const glm::quat&>(v);  }
inline const glm::mat4&  to_glm(const ApriMat4&  v) { return reinterpret_cast<const glm::mat4&>(v);  }

inline ApriVec3  from_glm(const glm::vec3&  v) { return { v.x, v.y, v.z };       }
inline ApriDVec3 from_glm(const glm::dvec3& v) { return { v.x, v.y, v.z };       }
inline ApriQuat  from_glm(const glm::quat&  v) { return { v.x, v.y, v.z, v.w };  }
inline ApriMat4  from_glm(const glm::mat4&  m) { ApriMat4 r; std::memcpy(r.m, &m[0][0], sizeof(r.m)); return r; }


/* ============================================================
   apricot_node_t
   ============================================================ */

struct apricot_node_t {
    std::string             name;

    /* Local transform */
    glm::dvec3              local_translation { 0.0, 0.0, 0.0 };
    glm::quat               local_rotation    { 1.f, 0.f, 0.f, 0.f };
    glm::vec3               local_scale       { 1.f, 1.f, 1.f };

    /* World transform — recomputed by apricot_scene_update */
    glm::dmat4              world_transform   { 1.0 };

    /* Hierarchy */
    apricot_node_t*                  parent   { nullptr };
    std::vector<apricot_node_t*>     children;

    /* Attached scene objects */
    std::vector<apricot_geometry_t*> geometries;
    std::vector<apricot_light_t*>    lights;
};


/* ============================================================
   apricot_scene_t
   ============================================================ */

struct apricot_scene_t {
    /* Flat registries — used for ownership and scene-wide queries.
     * The node hierarchy is expressed through node parent/child pointers. */
    std::vector<apricot_node_t*>     all_nodes;
    std::vector<apricot_geometry_t*> all_geometries;
    std::vector<apricot_light_t*>    all_lights;

    /* Picking registry — index is pick_id, value is the geometry at that ID.
     * Index 0 is always nullptr (reserved as the "nothing hit" sentinel).
     * Slots are nulled on geometry destroy but never removed — pick_ids are stable
     * for the lifetime of the scene. */
    std::vector<apricot_geometry_t*> pick_registry;

    /* Current picking state — mirrored to the GPU UBO each time it changes. */
    uint32_t hovered_id  { 0 };
    uint32_t selected_id { 0 };
    uint32_t cursor_x    { 0 };
    uint32_t cursor_y    { 0 };

    /* GPU-side uniform buffer bound to every real shader.
     * Contains ApricotPickingState. Updated whenever hover/selection/cursor changes. */
    spudgpu_buffer picking_state_buffer { nullptr };
};


/* ============================================================
   apricot_mesh_t
   ============================================================ */

struct apricot_mesh_t {
    std::vector<ApricotVertex>  vertices;
    std::vector<uint32_t>       indices;

    /* GPU resources — valid only after apricot_mesh_upload() */
    spudgpu_buffer      vertex_buffer { nullptr };
    spudgpu_buffer      index_buffer  { nullptr };
    spudgpu_buffer_view vertex_view   { nullptr };
    spudgpu_buffer_view index_view    { nullptr };

    bool uploaded { false };
};


/* ============================================================
   apricot_material_t
   ============================================================ */

struct apricot_material_t {
    std::string  shader_name;
    glm::vec4    color { 1.f, 1.f, 1.f, 1.f };

    std::unordered_map<std::string, float>     float_params;
    std::unordered_map<std::string, glm::vec4> vec4_params;

    /* Resolved by the material system when the shader is loaded */
    spudgpu_shader_pipeline pipeline { nullptr };
};


/* ============================================================
   apricot_geometry_t
   ============================================================ */

struct apricot_geometry_t {
    std::string         name;

    /* Compact scene-local pick ID — index into apricot_scene_t::pick_registry.
     * 0 = unassigned (should never happen after creation).
     * Stored as two uint32_t in push constants so the picking shader
     * can output it to the R32G32_UINT picking texture. */
    uint32_t            pick_id     { 0 };

    apricot_node_t*     parent_node { nullptr };
    apricot_mesh_t*     mesh        { nullptr };
    apricot_material_t* material    { nullptr };
};


/* ============================================================
   apricot_camera_t
   ============================================================ */

enum class ApricotProjectionType { Perspective, Orthographic };

struct apricot_camera_t {
    ApricotProjectionType projection_type { ApricotProjectionType::Perspective };

    /* Perspective params */
    float fov_y  { glm::radians(60.f) };
    float aspect { 16.f / 9.f };
    float near_z { 0.1f };
    float far_z  { 10000.f };

    /* Orthographic params */
    float ortho_width  { 10.f };
    float ortho_height { 10.f };

    /* View */
    glm::dvec3  position { 0.0, 0.0, 0.0 };
    glm::quat   rotation { 1.f, 0.f, 0.f, 0.f };

    /* Cached matrices — float for GPU submission.
     * Large-world offset (camera-relative rendering) is a future TODO. */
    glm::mat4  view_matrix { 1.f };
    glm::mat4  proj_matrix { 1.f };

    bool dirty { true };
};


/* ============================================================
   apricot_light_t
   ============================================================ */

struct apricot_light_t {
    APRICOT_LIGHT_TYPE  type       { APRICOT_LIGHT_DIRECTIONAL };
    glm::vec4           color      { 1.f, 1.f, 1.f, 1.f };
    float               intensity  { 1.f };
    float               range      { 10.f };
    float               spot_angle { 45.f };

    apricot_node_t*     node       { nullptr };
};


/* ============================================================
   apricot_viewport_t
   ============================================================ */

struct apricot_viewport_t {
    apricot_scene_t*   scene      { nullptr };
    apricot_camera_t*  camera     { nullptr };
    spudgpu_swap_chain swap_chain { nullptr };

    /* Normalized 0-1 rect within the swap chain surface */
    float x { 0.f }, y { 0.f }, w { 1.f }, h { 1.f };

    /* Off-screen rendering path (is_offscreen == true).
     * render_width / render_height are fixed at creation time.
     * color_image is COLOR_ATTACHMENT | SAMPLED; the caller transitions
     * it to SHADER_READ_ONLY before passing it to ImGui. */
    bool               is_offscreen  { false };
    spudgpu_image      color_image   { nullptr };
    spudgpu_image_view color_view    { nullptr };
    uint32_t           render_width  { 0 };
    uint32_t           render_height { 0 };

    /* Offscreen picking render target — SPUDGPU_FORMAT_R32G32_UINT.
     * Each pixel stores uvec2(pick_id, 0). Shared depth with the main pass
     * so occluded geometry cannot be picked through surfaces in front of it.
     * Dimensions match the pixel area of this viewport's rect. */
    spudgpu_image      picking_image      { nullptr };
    spudgpu_image_view picking_image_view { nullptr };

    /* CPU-readable staging buffer — picking_image is copied here after the
     * picking pass so apricot_viewport_query_pick() can read a pixel without
     * a GPU stall. Size = picking_width * picking_height * 8 bytes.
     * TODO: requires spudgpu_cmd_copy_image_to_buffer (not yet in SpudGPU). */
    spudgpu_buffer     picking_readback   { nullptr };

    uint32_t           picking_width  { 0 };
    uint32_t           picking_height { 0 };

    /* Last resolved hover ID read back from the picking texture this frame. */
    uint32_t           last_hovered_id { 0 };
};
