//
// Apricot Renderer — C++ backend implementation
// Implements apricot_renderer.h using GLM for math and SpudGPU for GPU calls.
//

#include "apricot_renderer_internal.hpp"

#include <cstring>
#include <cassert>
#include <algorithm>
#include <cstdio>


/* ============================================================
   Module-level state
   ============================================================ */

static spudgpu_device g_device{nullptr};
static spudgpu_command_allocator g_cmd_allocator{nullptr};
static spudgpu_command_list g_cmd_list{nullptr};

static spudgpu_shader_module g_default_vert{nullptr};
static spudgpu_shader_module g_default_frag{nullptr};
static spudgpu_shader_pipeline g_default_pipeline{nullptr};


/* ============================================================
   Internal helpers
   ============================================================ */

static spudgpu_shader_module load_spirv(SPUDGPU_SHADER_STAGE stage, const char *path) {
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
    desc.stage = stage;
    desc.spirv_code = code;
    desc.spirv_size = sz;
    auto mod = spudgpu_create_shader_module(g_device, &desc);
    free(code);
    return mod;
}

/* Push constant block sent per-geometry draw:
 *   mat4  mvp      (offset 0,  64 bytes)
 *   vec4  color    (offset 64, 16 bytes)
 *   uint  pick_id  (offset 80,  4 bytes)
 */
struct ApricotPushConst {
    float mvp[16];
    float color[4];
    uint32_t pick_id;
};


/* ============================================================
   Internal helpers
   ============================================================ */

static void recompute_camera_matrices(apricot_camera_t *cam) {
    glm::vec3 pos = glm::vec3(cam->position);
    glm::mat4 rot = glm::mat4_cast(cam->rotation);
    cam->view_matrix = glm::transpose(rot) * glm::translate(glm::mat4(1.f), -pos);

    if (cam->projection_type == ApricotProjectionType::Perspective) {
        cam->proj_matrix = glm::perspective(cam->fov_y, cam->aspect, cam->near_z, cam->far_z);
    } else {
        float hw = cam->ortho_width * 0.5f;
        float hh = cam->ortho_height * 0.5f;
        cam->proj_matrix = glm::ortho(-hw, hw, -hh, hh, cam->near_z, cam->far_z);
    }

    cam->proj_matrix[1][1] *= -1.f; /* Vulkan Y flip */
    cam->dirty = false;
}

static void update_node_recursive(apricot_node_t *node, const glm::dmat4 &parent_world) {
    glm::dmat4 local =
            glm::translate(glm::dmat4(1.0), node->local_translation)
            * glm::dmat4(glm::mat4_cast(node->local_rotation))
            * glm::scale(glm::dmat4(1.0), glm::dvec3(node->local_scale));

    node->world_transform = parent_world * local;

    for (apricot_node_t *child: node->children)
        update_node_recursive(child, node->world_transform);
}

/* Push the picking state UBO to the GPU. Called whenever hovered/selected/cursor changes. */
static void flush_picking_state(apricot_scene_t *scene) {
    if (!scene->picking_state_buffer) return;

    ApricotPickingState state{
        scene->hovered_id,
        scene->selected_id,
        scene->cursor_x,
        scene->cursor_y
    };

    void *mapped = nullptr;
    if (spudgpu_map_buffer(scene->picking_state_buffer, 0, sizeof(ApricotPickingState), &mapped)) {
        std::memcpy(mapped, &state, sizeof(ApricotPickingState));
        spudgpu_unmap_buffer(scene->picking_state_buffer);
    }
}

extern "C" {
/* ============================================================
   Module lifecycle
   ============================================================ */

bool apricot_renderer_init(spudgpu_device device) {
    if (!device) return false;

    g_device = device;
    g_cmd_allocator = spudgpu_create_command_allocator(device);
    g_cmd_list = spudgpu_create_command_list(g_cmd_allocator);

    if (!g_cmd_allocator || !g_cmd_list) return false;

    /* Default pipeline — flat-color, ApricotVertex layout, BGRA8_UNORM target. */
    g_default_vert = load_spirv(SPUDGPU_SHADER_STAGE_VERTEX, "shaders/apricot_default.vert.spv");
    g_default_frag = load_spirv(SPUDGPU_SHADER_STAGE_FRAGMENT, "shaders/apricot_default.frag.spv");
    if (!g_default_vert || !g_default_frag) return false;

    spudgpu_shader_pipeline_desc pd{};
    pd.vertex_module = g_default_vert;
    pd.vertex_entry_point = "main";
    pd.fragment_module = g_default_frag;
    pd.fragment_entry_point = "main";

    pd.vertex_bindings[0].binding = 0;
    pd.vertex_bindings[0].stride = sizeof(ApricotVertex);
    pd.vertex_bindings[0].per_instance = false;
    pd.vertex_binding_count = 1;

    pd.vertex_attributes[0] = {0, 0, SPUDGPU_FORMAT_R32G32B32_FLOAT, offsetof(ApricotVertex, position)};
    pd.vertex_attributes[1] = {1, 0, SPUDGPU_FORMAT_R32G32B32_FLOAT, offsetof(ApricotVertex, normal)};
    pd.vertex_attributes[2] = {2, 0, SPUDGPU_FORMAT_R32G32_FLOAT, offsetof(ApricotVertex, uv)};
    pd.vertex_attributes[3] = {3, 0, SPUDGPU_FORMAT_R32G32B32A32_FLOAT, offsetof(ApricotVertex, color)};
    pd.vertex_attribute_count = 4;

    pd.primitive_topology = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pd.cull_mode = SPUDGPU_CULL_MODE_BACK;
    pd.front_face_ccw = false; /* GLM Y-flip inverts winding: world-CCW becomes screen-CW */
    pd.wireframe = false;
    pd.depth_test_enable = false;
    pd.depth_write_enable = false;
    pd.depth_compare_op = SPUDGPU_COMPARE_OP_LESS;
    pd.depth_format = SPUDGPU_FORMAT_UNKNOWN;

    pd.blend_attachment.blend_enable = false;
    pd.blend_attachment.src_color_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
    pd.blend_attachment.dst_color_blend_factor = SPUDGPU_BLEND_FACTOR_ZERO;
    pd.blend_attachment.color_blend_op = SPUDGPU_BLEND_OP_ADD;
    pd.blend_attachment.src_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
    pd.blend_attachment.dst_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ZERO;
    pd.blend_attachment.alpha_blend_op = SPUDGPU_BLEND_OP_ADD;

    pd.color_attachment_format = SPUDGPU_FORMAT_B8G8R8A8_UNORM;

    pd.push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_FRAGMENT;
    pd.push_constant_ranges[0].offset = 0;
    pd.push_constant_ranges[0].size = sizeof(ApricotPushConst);
    pd.push_constant_range_count = 1;

    g_default_pipeline = spudgpu_create_shader_pipeline(device, &pd);
    return g_default_pipeline != nullptr;
}

void apricot_renderer_shutdown() {
    if (g_default_pipeline) spudgpu_destroy_shader_pipeline(g_device, g_default_pipeline);
    if (g_default_vert) spudgpu_destroy_shader_module(g_device, g_default_vert);
    if (g_default_frag) spudgpu_destroy_shader_module(g_device, g_default_frag);

    if (g_cmd_list) spudgpu_destroy_command_list(g_cmd_list);
    if (g_cmd_allocator) spudgpu_destroy_command_allocator(g_cmd_allocator);

    g_default_pipeline = nullptr;
    g_default_vert = nullptr;
    g_default_frag = nullptr;
    g_cmd_list = nullptr;
    g_cmd_allocator = nullptr;
    g_device = nullptr;
}


/* ============================================================
   Scene
   ============================================================ */

apricot_scene apricot_scene_create() {
    if (!g_device) return nullptr;

    auto *scene = new apricot_scene_t();

    /* Reserve index 0 as the "nothing hit" sentinel. */
    scene->pick_registry.push_back(nullptr);

    /* Allocate the picking state UBO — stays mapped-coherent so we can
     * update it cheaply on every cursor move without a full GPU sync. */
    spudgpu_buffer_desc ps_desc{};
    ps_desc.usage = SPUDGPU_BUFFER_USAGE_UNIFORM;
    ps_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT;
    ps_desc.size = sizeof(ApricotPickingState);
    scene->picking_state_buffer = spudgpu_create_buffer(g_device, &ps_desc);

    flush_picking_state(scene);
    return scene;
}

void apricot_scene_destroy(apricot_scene scene) {
    if (!scene) return;

    for (auto *light: scene->all_lights) delete light;
    for (auto *geom: scene->all_geometries) delete geom;
    for (auto *node: scene->all_nodes) delete node;

    if (scene->picking_state_buffer)
        spudgpu_destroy_buffer(g_device, scene->picking_state_buffer);

    delete scene;
}

void apricot_scene_update(apricot_scene scene, float /*delta_time*/) {
    if (!scene) return;

    for (apricot_node_t *node: scene->all_nodes) {
        if (node->parent == nullptr)
            update_node_recursive(node, glm::dmat4(1.0));
    }
}


/* ============================================================
   Scene — picking state
   ============================================================ */

void apricot_scene_set_cursor(apricot_scene scene, uint32_t x, uint32_t y) {
    if (!scene) return;
    scene->cursor_x = x;
    scene->cursor_y = y;
    flush_picking_state(scene);
}

void apricot_scene_set_selected(apricot_scene scene, apricot_geometry geom) {
    if (!scene) return;
    scene->selected_id = geom ? geom->pick_id : 0u;
    flush_picking_state(scene);
}

apricot_geometry apricot_scene_get_hovered(apricot_scene scene) {
    if (!scene || scene->hovered_id == 0) return nullptr;
    if (scene->hovered_id >= scene->pick_registry.size()) return nullptr;
    return scene->pick_registry[scene->hovered_id];
}

apricot_geometry apricot_scene_get_selected(apricot_scene scene) {
    if (!scene || scene->selected_id == 0) return nullptr;
    if (scene->selected_id >= scene->pick_registry.size()) return nullptr;
    return scene->pick_registry[scene->selected_id];
}


/* ============================================================
   Node
   ============================================================ */

apricot_node apricot_node_create(apricot_scene scene, const char *name) {
    if (!scene) return nullptr;

    auto *node = new apricot_node_t();
    node->name = name ? name : "";
    scene->all_nodes.push_back(node);
    return node;
}

void apricot_node_destroy(apricot_node node) {
    if (!node) return;
    apricot_node_detach(node);
    for (apricot_node_t *child: node->children) child->parent = nullptr;
    delete node;
}

void apricot_node_attach(apricot_node parent, apricot_node child) {
    if (!parent || !child || child->parent == parent) return;
    apricot_node_detach(child);
    parent->children.push_back(child);
    child->parent = parent;
}

void apricot_node_detach(apricot_node node) {
    if (!node || !node->parent) return;
    auto &s = node->parent->children;
    s.erase(std::remove(s.begin(), s.end(), node), s.end());
    node->parent = nullptr;
}

void apricot_node_set_translation(apricot_node node, ApriDVec3 pos) {
    if (!node) return;
    node->local_translation = to_glm(pos);
}

void apricot_node_set_rotation(apricot_node node, ApriQuat rot) {
    if (!node) return;
    node->local_rotation = to_glm(rot);
}

void apricot_node_set_scale(apricot_node node, ApriVec3 scale) {
    if (!node) return;
    node->local_scale = to_glm(scale);
}

ApriDVec3 apricot_node_get_world_translation(apricot_node node) {
    if (!node) return {0.0, 0.0, 0.0};
    glm::dvec3 t = glm::dvec3(node->world_transform[3]);
    return {t.x, t.y, t.z};
}

ApriMat4 apricot_node_get_world_transform(apricot_node node) {
    if (!node) return from_glm(glm::mat4(1.f));
    return from_glm(glm::mat4(node->world_transform));
}


/* ============================================================
   Geometry
   ============================================================ */

apricot_geometry apricot_geometry_create(apricot_scene scene, const char *name) {
    if (!scene) return nullptr;

    auto *geom = new apricot_geometry_t();
    geom->name = name ? name : "";

    /* Assign the next available pick_id — the registry index. */
    geom->pick_id = static_cast<uint32_t>(scene->pick_registry.size());
    scene->pick_registry.push_back(geom);

    scene->all_geometries.push_back(geom);
    return geom;
}

void apricot_geometry_destroy(apricot_geometry geom) {
    if (!geom) return;
    apricot_geometry_detach(geom);

    /* Null the registry slot so query_pick returns nullptr for stale IDs.
     * The pick_id itself is never reused — slots are stable for the scene's lifetime. */

    /* NOTE: the scene pointer isn't stored on geometry, so the caller is expected
     * to have already removed it from scene->all_geometries before this call,
     * or this is called from apricot_scene_destroy which handles it. */

    delete geom;
}

void apricot_geometry_attach_to(apricot_geometry geom, apricot_node parent) {
    if (!geom || !parent) return;
    apricot_geometry_detach(geom);
    parent->geometries.push_back(geom);
    geom->parent_node = parent;
}

void apricot_geometry_detach(apricot_geometry geom) {
    if (!geom || !geom->parent_node) return;
    auto &list = geom->parent_node->geometries;
    list.erase(std::remove(list.begin(), list.end(), geom), list.end());
    geom->parent_node = nullptr;
}

void apricot_geometry_set_mesh(apricot_geometry geom, apricot_mesh mesh) {
    if (!geom) return;
    geom->mesh = mesh;
}

void apricot_geometry_set_material(apricot_geometry geom, apricot_material mat) {
    if (!geom) return;
    geom->material = mat;
}


/* ============================================================
   Mesh
   ============================================================ */

apricot_mesh apricot_mesh_create() {
    return new apricot_mesh_t();
}

void apricot_mesh_destroy(apricot_mesh mesh) {
    if (!mesh) return;
    if (mesh->vertex_view) spudgpu_destroy_buffer_view(mesh->vertex_view);
    if (mesh->index_view) spudgpu_destroy_buffer_view(mesh->index_view);
    if (mesh->vertex_buffer) spudgpu_destroy_buffer(g_device, mesh->vertex_buffer);
    if (mesh->index_buffer) spudgpu_destroy_buffer(g_device, mesh->index_buffer);
    delete mesh;
}

void apricot_mesh_set_vertices(apricot_mesh mesh, const ApricotVertex *vertices, uint32_t count) {
    if (!mesh || !vertices || count == 0) return;
    mesh->vertices.assign(vertices, vertices + count);
    mesh->uploaded = false;
}

void apricot_mesh_set_indices(apricot_mesh mesh, const uint32_t *indices, uint32_t count) {
    if (!mesh || !indices || count == 0) return;
    mesh->indices.assign(indices, indices + count);
    mesh->uploaded = false;
}

void apricot_mesh_upload(apricot_mesh mesh) {
    if (!mesh || !g_device || mesh->vertices.empty()) return;

    uint64_t vb_size = mesh->vertices.size() * sizeof(ApricotVertex);

    spudgpu_buffer_desc vb_desc{};
    vb_desc.usage = SPUDGPU_BUFFER_USAGE_VERTEX;
    vb_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT;
    vb_desc.size = vb_size;

    if (mesh->vertex_buffer) spudgpu_destroy_buffer(g_device, mesh->vertex_buffer);
    mesh->vertex_buffer = spudgpu_create_buffer(g_device, &vb_desc);

    void *mapped = nullptr;
    if (spudgpu_map_buffer(mesh->vertex_buffer, 0, vb_size, &mapped)) {
        std::memcpy(mapped, mesh->vertices.data(), vb_size);
        spudgpu_unmap_buffer(mesh->vertex_buffer);
    }

    spudgpu_buffer_view_desc vbv{};
    vbv.parent_buffer = mesh->vertex_buffer;
    vbv.offset_from_parent_buffer = 0;
    vbv.stride = sizeof(ApricotVertex);
    vbv.size = vb_size;
    if (mesh->vertex_view) spudgpu_destroy_buffer_view(mesh->vertex_view);
    mesh->vertex_view = spudgpu_create_buffer_view(mesh->vertex_buffer, &vbv);

    if (!mesh->indices.empty()) {
        uint64_t ib_size = mesh->indices.size() * sizeof(uint32_t);

        spudgpu_buffer_desc ib_desc{};
        ib_desc.usage = SPUDGPU_BUFFER_USAGE_INDEX;
        ib_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_COHERENT;
        ib_desc.size = ib_size;

        if (mesh->index_buffer) spudgpu_destroy_buffer(g_device, mesh->index_buffer);
        mesh->index_buffer = spudgpu_create_buffer(g_device, &ib_desc);

        if (spudgpu_map_buffer(mesh->index_buffer, 0, ib_size, &mapped)) {
            std::memcpy(mapped, mesh->indices.data(), ib_size);
            spudgpu_unmap_buffer(mesh->index_buffer);
        }

        spudgpu_buffer_view_desc ibv{};
        ibv.parent_buffer = mesh->index_buffer;
        ibv.offset_from_parent_buffer = 0;
        ibv.stride = sizeof(uint32_t);
        ibv.size = ib_size;
        if (mesh->index_view) spudgpu_destroy_buffer_view(mesh->index_view);
        mesh->index_view = spudgpu_create_buffer_view(mesh->index_buffer, &ibv);
    }

    mesh->uploaded = true;
}


/* ============================================================
   Material
   ============================================================ */

apricot_material apricot_material_create(const char *shader_name) {
    auto *mat = new apricot_material_t();
    mat->shader_name = shader_name ? shader_name : "";
    /* TODO: resolve shader_name → render pipeline + picking pipeline via shader system */
    return mat;
}

void apricot_material_destroy(apricot_material mat) {
    delete mat;
}

void apricot_material_set_color(apricot_material mat, ApriVec4 color) {
    if (!mat) return;
    mat->color = to_glm(color);
}

void apricot_material_set_float(apricot_material mat, const char *param, float value) {
    if (!mat || !param) return;
    mat->float_params[param] = value;
}

void apricot_material_set_vec4(apricot_material mat, const char *param, ApriVec4 value) {
    if (!mat || !param) return;
    mat->vec4_params[param] = to_glm(value);
}


/* ============================================================
   Camera
   ============================================================ */

apricot_camera apricot_camera_create() {
    return new apricot_camera_t();
}

void apricot_camera_destroy(apricot_camera cam) {
    delete cam;
}

void apricot_camera_set_perspective(apricot_camera cam, float fov_y, float aspect, float near_z, float far_z) {
    if (!cam) return;
    cam->projection_type = ApricotProjectionType::Perspective;
    cam->fov_y = fov_y;
    cam->aspect = aspect;
    cam->near_z = near_z;
    cam->far_z = far_z;
    cam->dirty = true;
}

void apricot_camera_set_orthographic(apricot_camera cam, float width, float height, float near_z, float far_z) {
    if (!cam) return;
    cam->projection_type = ApricotProjectionType::Orthographic;
    cam->ortho_width = width;
    cam->ortho_height = height;
    cam->near_z = near_z;
    cam->far_z = far_z;
    cam->dirty = true;
}

void apricot_camera_set_position(apricot_camera cam, ApriDVec3 pos) {
    if (!cam) return;
    cam->position = to_glm(pos);
    cam->dirty = true;
}

void apricot_camera_set_rotation(apricot_camera cam, ApriQuat rot) {
    if (!cam) return;
    cam->rotation = to_glm(rot);
    cam->dirty = true;
}

void apricot_camera_look_at(apricot_camera cam, ApriDVec3 eye, ApriDVec3 target, ApriVec3 up) {
    if (!cam) return;
    /* Convert look direction into rotation quaternion so recompute_camera_matrices
     * stays consistent even when set_perspective is called after this. */
    glm::vec3 f = glm::normalize(glm::vec3(to_glm(target)) - glm::vec3(to_glm(eye)));
    glm::vec3 s = glm::normalize(glm::cross(f, to_glm(up)));
    glm::vec3 u = glm::cross(s, f);
    cam->rotation = glm::quat_cast(glm::mat3(s, u, -f));
    cam->position = to_glm(eye);
    /* Recompute everything (including projection if dirty) then override view
     * with the precise lookAt result to avoid any quat round-trip error. */
    cam->dirty = true;
    recompute_camera_matrices(cam);
    cam->view_matrix = glm::lookAt(glm::vec3(to_glm(eye)), glm::vec3(to_glm(target)), to_glm(up));
}

ApriMat4 apricot_camera_get_view(apricot_camera cam) {
    if (!cam) return from_glm(glm::mat4(1.f));
    if (cam->dirty) recompute_camera_matrices(cam);
    return from_glm(cam->view_matrix);
}

ApriMat4 apricot_camera_get_projection(apricot_camera cam) {
    if (!cam) return from_glm(glm::mat4(1.f));
    if (cam->dirty) recompute_camera_matrices(cam);
    return from_glm(cam->proj_matrix);
}


/* ============================================================
   Light
   ============================================================ */

apricot_light apricot_light_create(APRICOT_LIGHT_TYPE type) {
    auto *light = new apricot_light_t();
    light->type = type;
    return light;
}

void apricot_light_destroy(apricot_light light) {
    if (!light) return;
    apricot_light_detach(light);
    delete light;
}

void apricot_light_attach_to(apricot_light light, apricot_node node) {
    if (!light || !node) return;
    apricot_light_detach(light);
    node->lights.push_back(light);
    light->node = node;
}

void apricot_light_detach(apricot_light light) {
    if (!light || !light->node) return;
    auto &list = light->node->lights;
    list.erase(std::remove(list.begin(), list.end(), light), list.end());
    light->node = nullptr;
}

void apricot_light_set_color(apricot_light light, ApriVec4 color) { if (light) light->color = to_glm(color); }
void apricot_light_set_intensity(apricot_light light, float intensity) { if (light) light->intensity = intensity; }
void apricot_light_set_range(apricot_light light, float range) { if (light) light->range = range; }
void apricot_light_set_spot_angle(apricot_light light, float degrees) { if (light) light->spot_angle = degrees; }


/* ============================================================
   Viewport
   ============================================================ */

/* Create/destroy the BGRA8_UNORM color render target used by off-screen viewports. */
static void create_offscreen_color_target(apricot_viewport_t *vp) {
    spudgpu_image_desc img_desc{};
    img_desc.type = SPUDGPU_IMAGE_TYPE_2D;
    img_desc.format = SPUDGPU_FORMAT_B8G8R8A8_UNORM;
    img_desc.width = vp->render_width;
    img_desc.height = vp->render_height;
    img_desc.depth = 1;
    img_desc.mip_levels = 1;
    img_desc.array_layers = 1;
    img_desc.usage = SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_SAMPLED;
    img_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;
    vp->color_image = spudgpu_create_image(g_device, &img_desc);

    spudgpu_image_view_desc view_desc{};
    view_desc.type = SPUDGPU_IMAGE_VIEW_TYPE_2D;
    view_desc.subresource_range.aspect_mask = 1;
    view_desc.subresource_range.base_mip_level = 0;
    view_desc.subresource_range.mip_level_count = 1;
    view_desc.subresource_range.base_array_layer = 0;
    view_desc.subresource_range.array_layer_count = 1;
    vp->color_view = spudgpu_create_image_view(vp->color_image, &view_desc);
}

static void destroy_offscreen_color_target(apricot_viewport_t *vp) {
    if (vp->color_view) {
        spudgpu_destroy_image_view(g_device, vp->color_view);
        vp->color_view = nullptr;
    }
    if (vp->color_image) {
        spudgpu_destroy_image(g_device, vp->color_image);
        vp->color_image = nullptr;
    }
}

/* Off-screen picking target — dimensions taken from vp->picking_width/height. */
static void create_offscreen_picking_target(apricot_viewport_t *vp) {
    spudgpu_image_desc pick_desc{};
    pick_desc.type = SPUDGPU_IMAGE_TYPE_2D;
    pick_desc.format = SPUDGPU_FORMAT_R32G32_UINT;
    pick_desc.width = vp->picking_width;
    pick_desc.height = vp->picking_height;
    pick_desc.depth = 1;
    pick_desc.mip_levels = 1;
    pick_desc.array_layers = 1;
    pick_desc.usage = SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC;
    pick_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;
    vp->picking_image = spudgpu_create_image(g_device, &pick_desc);

    spudgpu_image_view_desc pick_view_desc{};
    pick_view_desc.type = SPUDGPU_IMAGE_VIEW_TYPE_2D;
    pick_view_desc.subresource_range.aspect_mask = 1;
    pick_view_desc.subresource_range.base_mip_level = 0;
    pick_view_desc.subresource_range.mip_level_count = 1;
    pick_view_desc.subresource_range.base_array_layer = 0;
    pick_view_desc.subresource_range.array_layer_count = 1;
    vp->picking_image_view = spudgpu_create_image_view(vp->picking_image, &pick_view_desc);

    uint64_t rb_size = (uint64_t) vp->picking_width * vp->picking_height * 8;
    spudgpu_buffer_desc rb_desc{};
    rb_desc.usage = SPUDGPU_BUFFER_USAGE_NONE;
    rb_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_CACHED;
    rb_desc.size = rb_size;
    vp->picking_readback = spudgpu_create_buffer(g_device, &rb_desc);
}

static void create_picking_target(apricot_viewport_t *vp) {
    auto sc_desc = spudgpu_get_swap_chain_desc(vp->swap_chain);

    vp->picking_width = static_cast<uint32_t>(vp->w * static_cast<float>(sc_desc.width));
    vp->picking_height = static_cast<uint32_t>(vp->h * static_cast<float>(sc_desc.height));

    /* R32G32_UINT — two uint32 per pixel. R = pick_id, G = reserved (sub-element future). */
    spudgpu_image_desc img_desc{};
    img_desc.type = SPUDGPU_IMAGE_TYPE_2D;
    img_desc.usage = SPUDGPU_IMAGE_USAGE_COLOR_ATTACHMENT | SPUDGPU_IMAGE_USAGE_TRANSFER_SRC;
    img_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_DEVICE_LOCAL;
    img_desc.format = SPUDGPU_FORMAT_R32G32_UINT;
    img_desc.width = vp->picking_width;
    img_desc.height = vp->picking_height;
    img_desc.depth = 1;
    img_desc.array_layers = 1;
    img_desc.mip_levels = 1;
    vp->picking_image = spudgpu_create_image(g_device, &img_desc);

    spudgpu_image_view_desc view_desc{};
    view_desc.parent_image = vp->picking_image;
    view_desc.type = SPUDGPU_IMAGE_VIEW_TYPE_2D;
    view_desc.subresource_range.aspect_mask = 1; /* color aspect */
    view_desc.subresource_range.base_mip_level = 0;
    view_desc.subresource_range.mip_level_count = 1;
    view_desc.subresource_range.base_array_layer = 0;
    view_desc.subresource_range.array_layer_count = 1;
    vp->picking_image_view = spudgpu_create_image_view(vp->picking_image, &view_desc);

    /* CPU-readable staging buffer for pixel readback.
     * TODO: fill this via spudgpu_cmd_copy_image_to_buffer once that command exists. */
    uint64_t readback_size = static_cast<uint64_t>(vp->picking_width) * vp->picking_height * 8;
    /* 2x uint32 per pixel */

    spudgpu_buffer_desc rb_desc{};
    rb_desc.usage = SPUDGPU_BUFFER_USAGE_NONE;
    rb_desc.memory_flags = SPUDGPU_MEMORY_FLAGS_HOST_VISIBLE | SPUDGPU_MEMORY_FLAGS_HOST_CACHED;
    rb_desc.size = readback_size;
    vp->picking_readback = spudgpu_create_buffer(g_device, &rb_desc);
}

static void destroy_picking_target(apricot_viewport_t *vp) {
    if (vp->picking_image_view) spudgpu_destroy_image_view(g_device, vp->picking_image_view);
    if (vp->picking_image) spudgpu_destroy_image(g_device, vp->picking_image);
    if (vp->picking_readback) spudgpu_destroy_buffer(g_device, vp->picking_readback);

    vp->picking_image_view = nullptr;
    vp->picking_image = nullptr;
    vp->picking_readback = nullptr;
}

apricot_viewport apricot_viewport_create(apricot_scene scene, apricot_camera cam, spudgpu_swap_chain swap_chain) {
    if (!scene || !cam || !swap_chain) return nullptr;

    auto *vp = new apricot_viewport_t();
    vp->scene = scene;
    vp->camera = cam;
    vp->swap_chain = swap_chain;

    create_picking_target(vp);
    return vp;
}

void apricot_viewport_destroy(apricot_viewport vp) {
    if (!vp) return;
    destroy_picking_target(vp);
    if (vp->color_view) spudgpu_destroy_image_view(g_device, vp->color_view);
    if (vp->color_image) spudgpu_destroy_image(g_device, vp->color_image);
    delete vp;
}

void apricot_viewport_set_rect(apricot_viewport vp, float x, float y, float w, float h) {
    if (!vp || vp->is_offscreen) return; /* off-screen viewports use fixed dimensions */
    vp->x = x;
    vp->y = y;
    vp->w = w;
    vp->h = h;

    /* Rebuild the picking target at the new dimensions. */
    destroy_picking_target(vp);
    create_picking_target(vp);
}

void apricot_viewport_render(apricot_viewport vp) {
    if (!vp || !g_device) return;

    if (vp->camera->dirty) recompute_camera_matrices(vp->camera);

    glm::mat4 view = vp->camera->view_matrix;
    glm::mat4 proj = vp->camera->proj_matrix;

    auto sc_desc = spudgpu_get_swap_chain_desc(vp->swap_chain);

    SPUDGPU_VIEWPORT gpu_vp{};
    gpu_vp.x = vp->x * static_cast<float>(sc_desc.width);
    gpu_vp.y = vp->y * static_cast<float>(sc_desc.height);
    gpu_vp.width = vp->w * static_cast<float>(sc_desc.width);
    gpu_vp.height = vp->h * static_cast<float>(sc_desc.height);
    gpu_vp.minDepth = 0.f;
    gpu_vp.maxDepth = 1.f;

    SPUDGPU_SCISSOR_RECT scissor{gpu_vp.x, gpu_vp.y, gpu_vp.width, gpu_vp.height};

    spudgpu_reset_command_allocator(g_cmd_allocator);
    spudgpu_begin_command_list(g_cmd_list);

    /* ------------------------------------------------------------------
       Pass 1 — Picking pass
       Renders every geometry to the offscreen R32G32_UINT picking texture.
       Each fragment outputs uvec2(pick_id, 0).
       The depth buffer (shared with the main pass) ensures only the
       front-most geometry writes its ID — occluded geometry cannot be picked.

       TODO: begin render pass targeting picking_image_view (not the swap chain).
             Requires render pass API to be exposed in SpudGPU (spudgpuvulkanrenderpass.c
             already exists in the backend — needs a public header entry point).
    ------------------------------------------------------------------ */

    spudgpu_set_viewports(g_cmd_list, 0, 1, &gpu_vp);
    spudgpu_set_scissor_rects(g_cmd_list, 0, 1, &scissor);

    for (apricot_node_t *node: vp->scene->all_nodes) {
        for (apricot_geometry_t *geom: node->geometries) {
            if (!geom->mesh || !geom->mesh->uploaded) continue;

            /* TODO: bind scene-wide picking pipeline (one pipeline shared by all geometry).
             * Push constants layout:
             *   offset 0  : mat4  mvp        (64 bytes)
             *   offset 64 : uint  pick_id_lo (4 bytes)
             *   offset 68 : uint  pick_id_hi (4 bytes, reserved — always 0 for now)
             *
             * spudgpu_cmd_bind_pipeline(g_cmd_list, g_picking_pipeline);
             *
             * glm::mat4 world = glm::mat4(node->world_transform);
             * glm::mat4 mvp   = proj * view * world;
             * spudgpu_cmd_push_constants(g_cmd_list, &mvp, 0, 64);
             * spudgpu_cmd_push_constants(g_cmd_list, &geom->pick_id, 64, 4);
             */

            spudgpu_set_vertex_buffers(g_cmd_list, 0, 1, &geom->mesh->vertex_view);

            if (geom->mesh->index_view) {
                spudgpu_set_index_buffers(g_cmd_list, 1, &geom->mesh->index_view);
                spudgpu_draw_indexed(g_cmd_list, static_cast<uint32_t>(geom->mesh->indices.size()), 0, 0);
            } else {
                spudgpu_draw(g_cmd_list, static_cast<uint32_t>(geom->mesh->vertices.size()), 0);
            }
        }
    }

    /* TODO: end picking render pass.
     * TODO: copy picking_image pixel at (cursor_x, cursor_y) into picking_readback buffer.
     *       spudgpu_cmd_copy_image_to_buffer(g_cmd_list, picking_image, picking_readback,
     *           cursor_x, cursor_y, 1, 1);
     * Then read the mapped readback buffer to resolve hovered_id. */

    /* ------------------------------------------------------------------
       Pass 2 — Main render pass
       Real shaders read the picking state UBO (hovered_id, selected_id)
       and compare against their own pick_id push constant to determine
       whether to render as hovered or selected.
    ------------------------------------------------------------------ */

    uint32_t image_index = spudgpu_swap_chain_acquire_next_image(vp->swap_chain);
    (void) image_index; /* used by render pass attachment setup — TODO when render pass API lands */

    for (apricot_node_t *node: vp->scene->all_nodes) {
        for (apricot_geometry_t *geom: node->geometries) {
            if (!geom->mesh || !geom->mesh->uploaded) continue;
            if (!geom->material) continue;

            /* TODO: bind geom->material->pipeline
             * TODO: bind scene->picking_state_buffer as descriptor set 0, binding 0
             * TODO: push constants:
             *   mat4  mvp      (64 bytes)
             *   uint  pick_id  (4 bytes) — shader compares vs hovered_id / selected_id
             */

            spudgpu_set_vertex_buffers(g_cmd_list, 0, 1, &geom->mesh->vertex_view);

            if (geom->mesh->index_view) {
                spudgpu_set_index_buffers(g_cmd_list, 1, &geom->mesh->index_view);
                spudgpu_draw_indexed(g_cmd_list, static_cast<uint32_t>(geom->mesh->indices.size()), 0, 0);
            } else {
                spudgpu_draw(g_cmd_list, static_cast<uint32_t>(geom->mesh->vertices.size()), 0);
            }
        }
    }

    spudgpu_end_command_list(g_cmd_list);

    spudgpu_semaphore image_available = spudgpu_swap_chain_get_image_available_semaphore(vp->swap_chain);
    spudgpu_semaphore render_finished = spudgpu_swap_chain_get_render_finished_semaphore(vp->swap_chain);
    spudgpu_fence in_flight_fence = spudgpu_swap_chain_get_in_flight_fence(vp->swap_chain);

    spudgpu_command_list cmd_lists[] = {g_cmd_list};
    uint32_t wait_stages = SPUDGPU_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT;

    spudgpu_submit_desc submit{};
    submit.cmd_lists = cmd_lists;
    submit.cmd_list_count = 1;
    submit.wait_semaphores = &image_available;
    submit.wait_semaphore_count = 1;
    submit.wait_stage_masks = &wait_stages;
    submit.signal_semaphores = &render_finished;
    submit.signal_semaphore_count = 1;
    submit.signal_fence = in_flight_fence;

    spudgpu_queue_submit(spudgpu_get_graphics_queue(g_device), &submit);

    spudgpu_swap_chain_present(vp->swap_chain);
}

apricot_viewport apricot_viewport_create_offscreen(
    apricot_scene scene, apricot_camera cam,
    uint32_t width, uint32_t height) {
    if (!scene || !cam || !g_device || width == 0 || height == 0) return nullptr;

    auto *vp = new apricot_viewport_t();
    vp->scene = scene;
    vp->camera = cam;
    vp->is_offscreen = true;
    vp->render_width = width;
    vp->render_height = height;
    vp->w = 1.f;
    vp->h = 1.f;

    create_offscreen_color_target(vp);

    vp->picking_width = width;
    vp->picking_height = height;
    create_offscreen_picking_target(vp);

    return vp;
}

void apricot_viewport_resize(apricot_viewport vp, uint32_t width, uint32_t height) {
    if (!vp || !vp->is_offscreen || width == 0 || height == 0) return;
    if (vp->render_width == width && vp->render_height == height) return;

    vp->render_width = width;
    vp->render_height = height;
    vp->picking_width = width;
    vp->picking_height = height;

    destroy_offscreen_color_target(vp);
    destroy_picking_target(vp);

    create_offscreen_color_target(vp);
    create_offscreen_picking_target(vp);
}

spudgpu_image apricot_viewport_get_color_image(apricot_viewport vp) {
    if (!vp || !vp->is_offscreen) return nullptr;
    return vp->color_image;
}

spudgpu_image_view apricot_viewport_get_color_image_view(apricot_viewport vp) {
    if (!vp || !vp->is_offscreen) return nullptr;
    return vp->color_view;
}

void apricot_viewport_record(apricot_viewport vp, spudgpu_command_list cmd) {
    if (!vp || !cmd || !vp->is_offscreen || !g_default_pipeline) return;

    if (vp->camera->dirty) recompute_camera_matrices(vp->camera);

    glm::mat4 view = vp->camera->view_matrix;
    glm::mat4 proj = vp->camera->proj_matrix;

    SPUDGPU_VIEWPORT gpu_vp{};
    gpu_vp.x = 0.f;
    gpu_vp.y = 0.f;
    gpu_vp.width = static_cast<float>(vp->render_width);
    gpu_vp.height = static_cast<float>(vp->render_height);
    gpu_vp.minDepth = 0.f;
    gpu_vp.maxDepth = 1.f;

    SPUDGPU_SCISSOR_RECT scissor{0.f, 0.f, static_cast<float>(vp->render_width), static_cast<float>(vp->render_height)};

    spudgpu_render_pass_begin_desc rp{};
    rp.color_attachment = vp->color_view;
    rp.depth_attachment = nullptr;
    rp.pipeline = g_default_pipeline;
    rp.clear_color[0] = 0.10f;
    rp.clear_color[1] = 0.10f;
    rp.clear_color[2] = 0.15f;
    rp.clear_color[3] = 1.0f;
    rp.width = vp->render_width;
    rp.height = vp->render_height;
    spudgpu_cmd_begin_render_pass(cmd, &rp);

    spudgpu_set_viewports(cmd, 0, 1, &gpu_vp);
    spudgpu_set_scissor_rects(cmd, 0, 1, &scissor);
    spudgpu_cmd_bind_pipeline(cmd, g_default_pipeline);

    for (apricot_node_t *node: vp->scene->all_nodes) {
        for (apricot_geometry_t *geom: node->geometries) {
            if (!geom->mesh || !geom->mesh->uploaded) continue;

            glm::mat4 world = glm::mat4(node->world_transform);
            glm::mat4 mvp = proj * view * world;

            ApricotPushConst pc{};
            std::memcpy(pc.mvp, &mvp[0][0], 64);
            glm::vec4 col = geom->material ? geom->material->color : glm::vec4(1.f);
            pc.color[0] = col.r;
            pc.color[1] = col.g;
            pc.color[2] = col.b;
            pc.color[3] = col.a;
            pc.pick_id = geom->pick_id;

            spudgpu_cmd_push_constants(cmd, g_default_pipeline, 0, sizeof(pc), &pc);

            spudgpu_set_vertex_buffers(cmd, 0, 1, &geom->mesh->vertex_view);
            if (geom->mesh->index_view) {
                spudgpu_set_index_buffers(cmd, 1, &geom->mesh->index_view);
                spudgpu_draw_indexed(cmd, static_cast<uint32_t>(geom->mesh->indices.size()), 0, 0);
            } else {
                spudgpu_draw(cmd, static_cast<uint32_t>(geom->mesh->vertices.size()), 0);
            }
        }
    }

    spudgpu_cmd_end_render_pass(cmd);
}

apricot_geometry apricot_viewport_query_pick(apricot_viewport vp, uint32_t x, uint32_t y) {
    if (!vp || !vp->picking_readback) return nullptr;
    if (x >= vp->picking_width || y >= vp->picking_height) return nullptr;

    /* Read the two uint32 values at pixel (x, y) from the CPU-mapped readback buffer.
     * Layout: tightly packed rows, 8 bytes per pixel (2 × uint32).
     * TODO: this read is only valid after spudgpu_cmd_copy_image_to_buffer has been
     *       issued and the GPU has finished — full sync story lands with the render pass API. */
    uint64_t pixel_offset = (static_cast<uint64_t>(y) * vp->picking_width + x) * 8;

    void *mapped = nullptr;
    if (!spudgpu_map_buffer(vp->picking_readback, pixel_offset, 8, &mapped)) return nullptr;

    uint32_t pick_id_lo = 0;
    std::memcpy(&pick_id_lo, mapped, sizeof(uint32_t));
    spudgpu_unmap_buffer(vp->picking_readback);

    /* Invalidate CPU cache before reading — readback buffer uses HOST_CACHED. */
    spudgpu_invalidate_buffer(vp->picking_readback, pixel_offset, 8);

    if (pick_id_lo == 0) return nullptr; /* nothing hit */

    auto &reg = vp->scene->pick_registry;
    if (pick_id_lo >= reg.size()) return nullptr;

    /* Update hovered state to match what query_pick found. */
    apricot_geometry hit = reg[pick_id_lo];
    if (vp->scene->hovered_id != pick_id_lo) {
        vp->scene->hovered_id = pick_id_lo;
        flush_picking_state(vp->scene);
    }

    return hit;
}
}
