#ifndef APREND_INTERNAL_HPP
#define APREND_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/.
 * Defines the concrete structs behind every opaque handle in aprendscene.h
 * and aprendbase.h, and provides zero-cost GLM conversion helpers. */

#include "render/aprendscene.h"
#include "render/aprendbuffers.h"
#include "render/aprendframes.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>

/* ============================================================
   GLM conversion helpers
   Reinterpret-cast is valid because Apri### structs are laid out
   identically to their GLM counterparts (same member order, same types).
   ============================================================ */

inline glm::vec2 &to_glm(ApriVec2 &v) { return reinterpret_cast<glm::vec2 &>(v); }
inline glm::vec3 &to_glm(ApriVec3 &v) { return reinterpret_cast<glm::vec3 &>(v); }
inline glm::vec4 &to_glm(ApriVec4 &v) { return reinterpret_cast<glm::vec4 &>(v); }
inline glm::dvec3 &to_glm(ApriDVec3 &v) { return reinterpret_cast<glm::dvec3 &>(v); }
inline glm::dvec4 &to_glm(ApriDVec4 &v) { return reinterpret_cast<glm::dvec4 &>(v); }
inline glm::quat &to_glm(ApriQuat &v) { return reinterpret_cast<glm::quat &>(v); }
inline glm::mat4 &to_glm(ApriMat4 &v) { return reinterpret_cast<glm::mat4 &>(v); }
inline glm::dmat4 &to_glm(ApriDMat4 &v) { return reinterpret_cast<glm::dmat4 &>(v); }

inline const glm::vec2 &to_glm(const ApriVec2 &v) { return reinterpret_cast<const glm::vec2 &>(v); }
inline const glm::vec3 &to_glm(const ApriVec3 &v) { return reinterpret_cast<const glm::vec3 &>(v); }
inline const glm::vec4 &to_glm(const ApriVec4 &v) { return reinterpret_cast<const glm::vec4 &>(v); }
inline const glm::dvec3 &to_glm(const ApriDVec3 &v) { return reinterpret_cast<const glm::dvec3 &>(v); }
inline const glm::quat &to_glm(const ApriQuat &v) { return reinterpret_cast<const glm::quat &>(v); }
inline const glm::mat4 &to_glm(const ApriMat4 &v) { return reinterpret_cast<const glm::mat4 &>(v); }

inline ApriVec3 from_glm(const glm::vec3 &v) { return {v.x, v.y, v.z}; }
inline ApriDVec3 from_glm(const glm::dvec3 &v) { return {v.x, v.y, v.z}; }
inline ApriQuat from_glm(const glm::quat &v) { return {v.x, v.y, v.z, v.w}; }
inline ApriMat4 from_glm(const glm::mat4 &m)
{
    ApriMat4 r;
    std::memcpy(r.m, &m[0][0], sizeof(r.m));
    return r;
}


typedef struct aprend_node_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    std::string name;

    /* Owning scene -- lets aprend_node_destroy erase itself from
     * scene->all_nodes instead of leaving a dangling entry there. */
    struct aprend_scene_t *scene{nullptr};

    /* Local transform */
    glm::dvec3 local_translation{0.0, 0.0, 0.0};
    glm::quat local_rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 local_scale{1.f, 1.f, 1.f};

    /* World transform — recomputed by aprend_scene_update */
    glm::dmat4 world_transform{1.0};

    /* Hierarchy */
    aprend_node_t *parent{nullptr};
    std::vector<aprend_node_t *> children;

    /* Attached scene objects */
    std::vector<aprend_geometry_t *> geometries;
    std::vector<aprend_light_t *> lights;
} aprend_node_t;


typedef struct aprend_scene_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_instance instance{nullptr};

    /* Flat registries — used for ownership and scene-wide queries.
     * The node hierarchy is expressed through node parent/child pointers. */
    std::vector<aprend_node_t *> all_nodes;
    std::vector<aprend_geometry_t *> all_geometries;
    std::vector<aprend_light_t *> all_lights;
} aprend_scene_t;

/* ============================================================
   aprend_mesh_t
   ============================================================ */

typedef struct aprend_mesh_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_instance instance{nullptr};

    std::vector<aprend_vertex_binding> vertex_bindings;
    std::vector<void *> vertex_binding_data;
    uint32_t vertex_count{0};
    APREND_INDEX_STRIDE index_stride{APREND_INDEX_STRIDE_UINT16};
    void *indices{nullptr};
    uint32_t index_count{0};

    /* GPU resources — valid only after aprend_mesh_upload() */
    aprend_buffer_context vertex_buffer_ctx{nullptr};
    std::vector<aprend_vertex_buffer> vertex_buffers;
    aprend_index_buffer index_buffer{nullptr};

    bool uploaded{false};
} aprend_mesh_t;


typedef struct aprend_material_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_material_t() = default;
    ~aprend_material_t();

    glm::vec4 color{1.f, 1.f, 1.f, 1.f};

    std::unordered_map<std::string, float> float_params;
    std::unordered_map<std::string, glm::vec4> vec4_params;

    /* Built by aprend_material_create from caller-supplied SPIR-V. */
    spudgpu_shader_pipeline pipeline{nullptr};
} aprend_material_t;


typedef struct aprend_geometry_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    std::string name;

    aprend_node_t *parent_node{nullptr};
    aprend_mesh_t *mesh{nullptr};
    aprend_material_t *material{nullptr};
} aprend_geometry_t;


enum class ApricotProjectionType
{
    Perspective,
    Orthographic
};

typedef struct aprend_camera_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    ApricotProjectionType projection_type{ApricotProjectionType::Perspective};

    /* Perspective params */
    float fov_y{glm::radians(60.f)};
    float aspect{16.f / 9.f};
    float near_z{0.1f};
    float far_z{10000.f};

    /* Orthographic params */
    float ortho_width{10.f};
    float ortho_height{10.f};

    /* View */
    glm::dvec3 position{0.0, 0.0, 0.0};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};

    /* Cached matrices — float for GPU submission.
     * Large-world offset (camera-relative rendering) is a future TODO. */
    glm::mat4 view_matrix{1.f};
    glm::mat4 proj_matrix{1.f};

    bool dirty{true};
} aprend_camera_t;


typedef struct aprend_light_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    APREND_LIGHT_TYPE type{APREND_LIGHT_DIRECTIONAL};
    glm::vec4 color{1.f, 1.f, 1.f, 1.f};
    float intensity{1.f};
    float range{10.f};
    float spot_angle{45.f};

    aprend_node_t *node{nullptr};
} aprend_light_t;


typedef struct aprend_viewport_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_instance instance{nullptr};
    aprend_scene_t *scene{nullptr};
    aprend_camera_t *camera{nullptr};

    /* Off-screen rendering path (is_offscreen == true).
     * render_width / render_height are fixed at creation time.
     * framebuffer's single color attachment is SHADER_RESOURCE | RENDER_TARGET
     * (use_for_gui); the caller transitions it to SHADER_READ_ONLY before
     * passing it to ImGui. */
    bool is_offscreen{false};
    aprend_framebuffer framebuffer{nullptr};
    uint32_t render_width{0};
    uint32_t render_height{0};

    /* The depth attachment (framebuffer->depth_attachment) is internal to
     * Aprend — nothing outside aprend_viewport_record ever touches it, so
     * unlike the color target it needs no caller-facing barrier contract.
     * True right after creation/resize (attachment is fresh, UNDEFINED
     * layout); cleared to false once aprend_viewport_record has done the
     * one-time transition to DEPTH_STENCIL_ATTACHMENT_OPTIMAL. */
    bool depth_image_fresh{true};
} aprend_viewport_t;

typedef struct aprend_instance_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_instance_t() = default;
    ~aprend_instance_t();

    aprend_instance_desc desc{};
    spudgpu_command_allocator cmd_allocator{nullptr};
    spudgpu_command_list cmd_list{nullptr};

    /* Aprend's optional built-in debug shader — see aprend_renderer_init.
     * Device-bound, so it lives per-instance rather than as a global
     * singleton; NULL until aprend_renderer_init succeeds for this instance. */
    spudgpu_shader_module default_vertex_shader{nullptr};
    spudgpu_shader_module default_fragment_shader{nullptr};
    spudgpu_shader_pipeline default_pipeline{nullptr};
} aprend_instance_t;

struct AprendBufferContextRange
{
    uint32_t offset, size;
};

typedef struct aprend_buffer_context_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_instance instance;
    spudgpu_buffer buffer;
    spudgpu_buffer_desc buffer_desc;
    std::vector<AprendBufferContextRange> allocated_ranges; // for dynamic suballocation within the buffer
    bool AvailableSpace(uint32_t offset, uint32_t size) const;
    uint32_t GetEndAvailableOffset();
    // APREND_BUFFER_CONTEXT_FLAGS flags;
} aprend_buffer_context_t;

typedef struct aprend_uniform_buffer_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
	aprend_uniform_buffer_t() = default;
	~aprend_uniform_buffer_t();
	spudgpu_buffer buffer;
    spudgpu_buffer_view buffer_view;
    spudgpu_buffer_view_desc buffer_view_desc;
    aprend_uniform_layout layout;
    uint32_t total_size;
    void *uniform_data_ptr;
} aprend_uniform_buffer_t;

typedef struct aprend_vertex_buffer_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
    aprend_vertex_buffer_t() = default;
    ~aprend_vertex_buffer_t();
    spudgpu_buffer buffer;
    spudgpu_buffer_view buffer_view;
    spudgpu_buffer_view_desc buffer_view_desc;
    aprend_buffer_layout vertex_layout;
    uint32_t vertex_count;
    uint32_t vertex_stride;
} aprend_vertex_buffer_t;

typedef struct aprend_index_buffer_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
	aprend_index_buffer_t() = default;
	~aprend_index_buffer_t();
	spudgpu_buffer buffer;
    spudgpu_buffer_view buffer_view;
    spudgpu_buffer_view_desc buffer_view_desc;
    uint32_t index_count;
    APREND_INDEX_STRIDE index_stride;
} aprend_index_buffer_t;

typedef struct aprend_storage_buffer_t
{
#if _DEBUG
    char *debug_name{nullptr};
#endif
aprend_storage_buffer_t() = default;
~aprend_storage_buffer_t();
    spudgpu_buffer buffer;
    spudgpu_buffer_view buffer_view;
    spudgpu_buffer_view_desc buffer_view_desc;
    uint64_t size;
} aprend_storage_buffer_t;

#endif // APREND_INTERNAL_HPP
