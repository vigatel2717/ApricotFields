//
// Apricot Renderer — C++ backend implementation
// Implements aprendscene.h using GLM for math and SpudGPU for GPU calls.
//

#include "aprend_internal.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>

/* ============================================================
   Internal helpers
   ============================================================ */

static spudgpu_shader_module load_spirv(
    spudgpu_device device,
    SPUDGPU_SHADER_STAGE stage,
    const char *path) {
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
	desc.stage                = stage;
	desc.spirv_code           = code;
	desc.spirv_size           = sz;
	spudgpu_shader_module mod = nullptr;
	SPUDRESULT r              = spudgpu_create_shader_module(device, &desc, &mod);
	free(code);
	if (r != SPUD_SUCCESS)
		return nullptr;
	else
		return mod;
}

/* Push constant block sent per-geometry draw:
 *   mat4  mvp      (offset 0,  64 bytes)
 *   vec4  color    (offset 64, 16 bytes)
 */
struct ApricotPushConst {
	float mvp[16];
	float color[4];
};

/* Depth format for every offscreen viewport's depth attachment, and for
 * instance->default_pipeline's depth state. Custom materials that want to
 * participate in depth test/write against this must declare a matching
 * depth_format (see aprend_material_desc) — same requirement as the
 * push-constant layout below. No stencil: nothing here uses it. */
static constexpr SPUDGPU_FORMAT k_offscreen_depth_format = SPUDGPU_FORMAT_D32_FLOAT;

/* ============================================================
   Internal helpers
   ============================================================ */

static void recompute_camera_matrices(aprend_camera_t *cam) {
	glm::vec3 pos    = glm::vec3(cam->position);
	glm::mat4 rot    = glm::mat4_cast(cam->rotation);
	cam->view_matrix = glm::transpose(rot) * glm::translate(glm::mat4(1.f), -pos);

	if (cam->projection_type == ApricotProjectionType::Perspective) {
		cam->proj_matrix = glm::perspective(cam->fov_y, cam->aspect, cam->near_z, cam->far_z);
	} else {
		float hw         = cam->ortho_width * 0.5f;
		float hh         = cam->ortho_height * 0.5f;
		cam->proj_matrix = glm::ortho(-hw, hw, -hh, hh, cam->near_z, cam->far_z);
	}

	cam->proj_matrix[1][1] *= -1.f; /* Vulkan Y flip */
	cam->dirty = false;
}

static void update_node_recursive(
    aprend_node_t *node,
    const glm::dmat4 &parent_world) {
	glm::dmat4 local = glm::translate(glm::dmat4(1.0), node->local_translation) * glm::dmat4(glm::mat4_cast(node->local_rotation)) *
	                   glm::scale(glm::dmat4(1.0), glm::dvec3(node->local_scale));

	node->world_transform = parent_world * local;

	for (aprend_node_t *child : node->children)
		update_node_recursive(child, node->world_transform);
}

aprend_material_t::~aprend_material_t() { spudgpu_destroy_shader_pipeline(this->pipeline); }

extern "C" {
/* ============================================================
   Module lifecycle
   ============================================================ */

bool aprend_renderer_init(aprend_instance instance) {
	if (!instance)
		return false;

	spudgpu_device device = instance->desc.device;

	/* Default pipeline — flat-color, ApricotVertex layout, BGRA8_UNORM target. */
	instance->default_vertex_shader   = load_spirv(device, SPUDGPU_SHADER_STAGE_VERTEX, "shaders/apricot_default.vert.spv");
	instance->default_fragment_shader = load_spirv(device, SPUDGPU_SHADER_STAGE_FRAGMENT, "shaders/apricot_default.frag.spv");
	if (!instance->default_vertex_shader || !instance->default_fragment_shader)
		return false;

	spudgpu_shader_pipeline_desc pd{};
	pd.vertex_module        = instance->default_vertex_shader;
	pd.vertex_entry_point   = "main";
	pd.fragment_module      = instance->default_fragment_shader;
	pd.fragment_entry_point = "main";

	pd.vertex_bindings[0].binding      = 0;
	pd.vertex_bindings[0].stride       = sizeof(APREND_DEFAULT_VERTEX);
	pd.vertex_bindings[0].per_instance = false;
	pd.vertex_binding_count            = 1;

	pd.vertex_attributes[0]   = {0, 0, SPUDGPU_FORMAT_R32G32B32_FLOAT, offsetof(APREND_DEFAULT_VERTEX, position)};
	pd.vertex_attributes[1]   = {1, 0, SPUDGPU_FORMAT_R32G32B32_FLOAT, offsetof(APREND_DEFAULT_VERTEX, normal)};
	pd.vertex_attributes[2]   = {2, 0, SPUDGPU_FORMAT_R32G32_FLOAT, offsetof(APREND_DEFAULT_VERTEX, uv)};
	pd.vertex_attributes[3]   = {3, 0, SPUDGPU_FORMAT_R32G32B32A32_FLOAT, offsetof(APREND_DEFAULT_VERTEX, color)};
	pd.vertex_attribute_count = 4;

	pd.primitive_topology = SPUDGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	pd.cull_mode          = SPUDGPU_CULL_MODE_BACK;
	pd.front_face_ccw     = false; /* GLM Y-flip inverts winding: world-CCW becomes screen-CW */
	pd.wireframe          = false;
	pd.depth_test_enable  = true;
	pd.depth_write_enable = true;
	pd.depth_compare_op   = SPUDGPU_COMPARE_OP_LESS;
	pd.depth_format       = k_offscreen_depth_format;

	pd.blend_attachment.blend_enable           = false;
	pd.blend_attachment.src_color_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
	pd.blend_attachment.dst_color_blend_factor = SPUDGPU_BLEND_FACTOR_ZERO;
	pd.blend_attachment.color_blend_op         = SPUDGPU_BLEND_OP_ADD;
	pd.blend_attachment.src_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ONE;
	pd.blend_attachment.dst_alpha_blend_factor = SPUDGPU_BLEND_FACTOR_ZERO;
	pd.blend_attachment.alpha_blend_op         = SPUDGPU_BLEND_OP_ADD;

	pd.color_attachment_format = SPUDGPU_FORMAT_B8G8R8A8_UNORM;

	pd.push_constant_ranges[0].stage_flags = SPUDGPU_SHADER_STAGE_VERTEX | SPUDGPU_SHADER_STAGE_FRAGMENT;
	pd.push_constant_ranges[0].offset      = 0;
	pd.push_constant_ranges[0].size        = sizeof(ApricotPushConst);
	pd.push_constant_range_count           = 1;

	SPUDRESULT r = spudgpu_create_shader_pipeline(device, &pd, &instance->default_pipeline);
	if (r != SPUD_SUCCESS)
		return false;
	return instance->default_pipeline != nullptr;
}

void aprend_renderer_shutdown(aprend_instance instance) {
	if (!instance)
		return;

	spudgpu_destroy_shader_pipeline(instance->default_pipeline);
	spudgpu_destroy_shader_module(instance->default_vertex_shader);
	spudgpu_destroy_shader_module(instance->default_fragment_shader);

	instance->default_pipeline        = nullptr;
	instance->default_vertex_shader   = nullptr;
	instance->default_fragment_shader = nullptr;
}

/* ============================================================
   Scene
   ============================================================ */

aprend_scene aprend_scene_create(aprend_instance instance) {
	if (!instance)
		return nullptr;

	aprend_scene_t *scene = (aprend_scene_t *)malloc(sizeof(aprend_scene_t));
	if (!scene)
		return nullptr;
	scene           = new (scene) aprend_scene_t();
	scene->instance = instance;
	return scene;
}

void aprend_scene_destroy(aprend_scene scene) {
	if (!scene)
		return;

	for (auto *light : scene->all_lights) {
		light->~aprend_light_t();
		free(light);
	}
	for (auto *geom : scene->all_geometries) {
		geom->~aprend_geometry_t();
		free(geom);
	}
	for (auto *node : scene->all_nodes) {
		node->~aprend_node_t();
		free(node);
	}

	scene->~aprend_scene_t();
	free(scene);
}

void aprend_scene_update(
    aprend_scene scene,
    float /*delta_time*/) {
	if (!scene)
		return;

	for (aprend_node_t *node : scene->all_nodes) {
		if (node->parent == nullptr)
			update_node_recursive(node, glm::dmat4(1.0));
	}
}

/* ============================================================
   Node
   ============================================================ */

aprend_node aprend_node_create(
    aprend_scene scene,
    const char *name) {
	if (!scene)
		return nullptr;

	aprend_node_t *node = (aprend_node_t *)malloc(sizeof(aprend_node_t));
	if (!node)
		return nullptr;
	node        = new (node) aprend_node_t();
	node->name  = name ? name : "";
	node->scene = scene;
	scene->all_nodes.push_back(node);
	return node;
}

void aprend_node_destroy(aprend_node node) {
	if (!node)
		return;
	aprend_node_detach(node);
	for (aprend_node_t *child : node->children)
		child->parent = nullptr;
	if (node->scene) {
		auto &nodes = node->scene->all_nodes;
		nodes.erase(std::remove(nodes.begin(), nodes.end(), node), nodes.end());
	}
	node->~aprend_node_t();
	free(node);
}

void aprend_node_attach(
    aprend_node parent,
    aprend_node child) {
	if (!parent || !child)
		return;
	if (child->parent == parent)
		return;
	aprend_node_detach(child);
	parent->children.push_back(child);
	child->parent = parent;
}

void aprend_node_detach(aprend_node node) {
	if (!node)
		return;
	if (!node->parent)
		return;
	auto &s = node->parent->children;
	s.erase(std::remove(s.begin(), s.end(), node), s.end());
	node->parent = nullptr;
}

void aprend_node_set_translation(
    aprend_node node,
    ApriDVec3 pos) {
	if (!node)
		return;
	node->local_translation = to_glm(pos);
}

void aprend_node_set_rotation(
    aprend_node node,
    ApriQuat rot) {
	if (!node)
		return;
	node->local_rotation = to_glm(rot);
}

void aprend_node_set_scale(
    aprend_node node,
    ApriVec3 scale) {
	if (!node)
		return;
	node->local_scale = to_glm(scale);
}

ApriDVec3 aprend_node_get_translation(aprend_node node) {
	if (!node)
		return {0.0, 0.0, 0.0};
	return {node->local_translation.x, node->local_translation.y, node->local_translation.z};
}

ApriQuat aprend_node_get_rotation(aprend_node node) {
	if (!node)
		return {0.f, 0.f, 0.f, 1.f};
	return from_glm(node->local_rotation);
}

ApriVec3 aprend_node_get_scale(aprend_node node) {
	if (!node)
		return {1.f, 1.f, 1.f};
	return from_glm(node->local_scale);
}

ApriDVec3 aprend_node_get_world_translation(aprend_node node) {
	if (!node)
		return {0.0, 0.0, 0.0};
	glm::dvec3 t = glm::dvec3(node->world_transform[3]);
	return {t.x, t.y, t.z};
}

ApriMat4 aprend_node_get_world_transform(aprend_node node) {
	if (!node)
		return from_glm(glm::mat4(1.f));
	return from_glm(glm::mat4(node->world_transform));
}

/* ============================================================
   Geometry
   ============================================================ */

aprend_geometry aprend_geometry_create(
    aprend_scene scene,
    const char *name) {
	if (!scene)
		return nullptr;

	aprend_geometry_t *geom = (aprend_geometry_t *)malloc(sizeof(aprend_geometry_t));
	geom                    = new (geom) aprend_geometry_t();
	geom->name              = name ? name : "";

	scene->all_geometries.push_back(geom);
	return geom;
}

void aprend_geometry_destroy(aprend_geometry geom) {
	if (!geom)
		return;
	aprend_geometry_detach(geom);

	/* NOTE: the scene pointer isn't stored on geometry, so the caller is expected
	 * to have already removed it from scene->all_geometries before this call,
	 * or this is called from aprend_scene_destroy which handles it. */

	geom->~aprend_geometry_t();
	free(geom);
}

void aprend_geometry_attach_to(
    aprend_geometry geom,
    aprend_node parent) {
	if (!geom || !parent)
		return;
	aprend_geometry_detach(geom);
	parent->geometries.push_back(geom);
	geom->parent_node = parent;
}

void aprend_geometry_detach(aprend_geometry geom) {
	if (!geom)
		return;
	if (!geom->parent_node)
		return;
	auto &list = geom->parent_node->geometries;
	list.erase(std::remove(list.begin(), list.end(), geom), list.end());
	geom->parent_node = nullptr;
}

void aprend_geometry_set_mesh(
    aprend_geometry geom,
    aprend_mesh mesh) {
	if (!geom)
		return;
	geom->mesh = mesh;
}

void aprend_geometry_set_material(
    aprend_geometry geom,
    aprend_material mat) {
	if (!geom)
		return;
	geom->material = mat;
}

/* ============================================================
   Mesh
   ============================================================ */

aprend_mesh aprend_mesh_create(aprend_instance instance) {
	if (!instance)
		return nullptr;
	aprend_mesh_t *result = (aprend_mesh_t *)malloc(sizeof(aprend_mesh_t));
	if (!result)
		return nullptr;
	result           = new (result) aprend_mesh_t();
	result->instance = instance;
	return result;
}

void aprend_mesh_destroy(aprend_mesh mesh) {
	if (!mesh)
		return;
#if _DEBUG
	free(mesh->debug_name);
#endif
	for (size_t i = 0; i < mesh->vertex_binding_data.size(); i++)
		free(mesh->vertex_binding_data[i]);
	free(mesh->indices);

	for (aprend_vertex_buffer vb : mesh->vertex_buffers)
		aprend_vertex_buffer_destroy(vb);
	aprend_index_buffer_destroy(mesh->index_buffer);
	aprend_buffer_context_destroy(mesh->vertex_buffer_ctx);

	mesh->~aprend_mesh_t();
	free(mesh);
}
void aprend_mesh_set_vertex_bindings(
    aprend_mesh mesh,
    const aprend_vertex_binding *bindings,
    uint32_t binding_count) {
	if (!mesh)
		return;
	if (mesh->uploaded)
		return;
	mesh->vertex_bindings.assign(bindings, bindings + binding_count);
	for (size_t i = 0; i < mesh->vertex_binding_data.size(); ++i)
		free(mesh->vertex_binding_data[i]);
	mesh->vertex_binding_data.resize((size_t)binding_count);
}
void aprend_mesh_set_vertex_binding_data(
    aprend_mesh mesh,
    uint32_t binding,
    uint32_t vertex_count,
    const void *data) {
	if (!(mesh && data && vertex_count))
		return;
	for (size_t i = 0; i < mesh->vertex_bindings.size(); ++i) {
		const aprend_vertex_binding &b = mesh->vertex_bindings[i];
		if (b.binding == binding) {
			uint32_t vertex_stride = aprend_buffer_layout_get_total_size(&b.layout);
			uint64_t data_size     = vertex_count * vertex_stride;
			void *bd               = malloc(data_size);
			memcpy(bd, data, data_size);
			mesh->vertex_binding_data[i] = bd;
		}
	}
	mesh->vertex_count = vertex_count;
}
void aprend_mesh_set_indices(
    aprend_mesh mesh,
    APREND_INDEX_STRIDE stride,
    uint32_t index_count,
    const void *data) {
	if (!(mesh && data && index_count))
		return;
	mesh->index_stride = stride;
	mesh->indices      = malloc(index_count * stride);
	memcpy(mesh->indices, data, index_count * stride);
	mesh->index_count = index_count;
}
void aprend_mesh_upload(aprend_mesh mesh) {
	if (!mesh)
		return;
	if (!mesh->instance || mesh->uploaded)
		return;
	if (!(mesh->vertex_bindings.size() && mesh->vertex_count && mesh->indices))
		return;

	mesh->vertex_buffers.reserve(mesh->vertex_bindings.size());
	for (size_t i = 0; i < mesh->vertex_bindings.size(); ++i) {
		void *data = (i < mesh->vertex_binding_data.size()) ? mesh->vertex_binding_data[i] : nullptr;
		if (!data)
			goto failedattempt;

		aprend_vertex_buffer vb = aprend_vertex_buffer_create(mesh->instance, &mesh->vertex_bindings[i].layout, mesh->vertex_count, data);
		if (!vb)
			goto failedattempt;
		mesh->vertex_buffers.emplace_back(vb);
	}

	mesh->index_buffer = aprend_index_buffer_create(mesh->instance, mesh->index_stride, mesh->index_count, mesh->indices);
	if (!mesh->index_buffer)
		goto failedattempt;

	mesh->uploaded = true;
	return;
failedattempt:
	for (aprend_vertex_buffer vb : mesh->vertex_buffers)
		aprend_vertex_buffer_destroy(vb);
	mesh->vertex_buffers.clear();
}
bool aprend_mesh_is_uploaded(aprend_mesh mesh) { return mesh ? mesh->uploaded : false; }

/* ============================================================
   Material
   ============================================================ */

aprend_material aprend_material_create(
    aprend_instance instance,
    const aprend_material_desc *desc) {
	if (!instance || !desc)
		return nullptr;
	if (!(desc->vertex_spirv && desc->vertex_spirv_size && desc->fragment_spirv && desc->fragment_spirv_size))
		return nullptr;

	spudgpu_device device = instance->desc.device;

	spudgpu_shader_module_desc vert_mod_desc{};
	vert_mod_desc.stage      = SPUDGPU_SHADER_STAGE_VERTEX;
	vert_mod_desc.spirv_code = desc->vertex_spirv;
	vert_mod_desc.spirv_size = desc->vertex_spirv_size;

	spudgpu_shader_module vert_module = nullptr;
	if (spudgpu_create_shader_module(device, &vert_mod_desc, &vert_module) != SPUD_SUCCESS)
		return nullptr;

	spudgpu_shader_module_desc frag_mod_desc{};
	frag_mod_desc.stage      = SPUDGPU_SHADER_STAGE_FRAGMENT;
	frag_mod_desc.spirv_code = desc->fragment_spirv;
	frag_mod_desc.spirv_size = desc->fragment_spirv_size;

	spudgpu_shader_module frag_module = nullptr;
	if (spudgpu_create_shader_module(device, &frag_mod_desc, &frag_module) != SPUD_SUCCESS) {
		spudgpu_destroy_shader_module(vert_module);
		return nullptr;
	}

	spudgpu_shader_pipeline_desc pd{};
	pd.vertex_module        = vert_module;
	pd.vertex_entry_point   = desc->vertex_entry_point;
	pd.fragment_module      = frag_module;
	pd.fragment_entry_point = desc->fragment_entry_point;

	memcpy(pd.vertex_attributes, desc->vertex_attributes, sizeof(pd.vertex_attributes));
	pd.vertex_attribute_count = desc->vertex_attribute_count;
	memcpy(pd.vertex_bindings, desc->vertex_bindings, sizeof(pd.vertex_bindings));
	pd.vertex_binding_count = desc->vertex_binding_count;

	pd.primitive_topology = desc->primitive_topology;
	pd.cull_mode          = desc->cull_mode;
	pd.front_face_ccw     = desc->front_face_ccw;
	pd.wireframe          = desc->wireframe;

	pd.depth_test_enable  = desc->depth_test_enable;
	pd.depth_write_enable = desc->depth_write_enable;
	pd.depth_compare_op   = desc->depth_compare_op;

	pd.blend_attachment = desc->blend_attachment;

	pd.color_attachment_format = desc->color_attachment_format;
	pd.depth_format            = desc->depth_format;

	memcpy(pd.descriptor_set_layouts, desc->descriptor_set_layouts, sizeof(pd.descriptor_set_layouts));
	pd.descriptor_set_layout_count = desc->descriptor_set_layout_count;
	memcpy(pd.push_constant_ranges, desc->push_constant_ranges, sizeof(pd.push_constant_ranges));
	pd.push_constant_range_count = desc->push_constant_range_count;

	spudgpu_shader_pipeline pipeline = nullptr;
	SPUDRESULT sr                    = spudgpu_create_shader_pipeline(device, &pd, &pipeline);

	// Modules are only needed at pipeline-creation time — the pipeline has
	// linked whatever it needs from them by the time this call returns.
	spudgpu_destroy_shader_module(vert_module);
	spudgpu_destroy_shader_module(frag_module);

	if (sr != SPUD_SUCCESS) {
		printf("%s", spudresult_str(sr));
		return nullptr;
	}

	aprend_material_t *mat = (aprend_material_t *)malloc(sizeof(aprend_material_t));
	if (!mat) {
		spudgpu_destroy_shader_pipeline(pipeline);
		return nullptr;
	}
	mat           = new (mat) aprend_material_t();
	mat->pipeline = pipeline;
	return mat;
}

void aprend_material_destroy(aprend_material mat) {
	if (!mat)
		return;
	mat->~aprend_material_t();
	free(mat);
}

spudgpu_shader_pipeline aprend_material_get_spudgpu_pipeline(aprend_material mat) { return mat ? mat->pipeline : nullptr; }

void aprend_material_set_color(
    aprend_material mat,
    ApriVec4 color) {
	if (!mat)
		return;
	mat->color = to_glm(color);
}

void aprend_material_set_float(
    aprend_material mat,
    const char *param,
    float value) {
	if (!mat || !param)
		return;
	mat->float_params[param] = value;
}

void aprend_material_set_vec4(
    aprend_material mat,
    const char *param,
    ApriVec4 value) {
	if (!mat || !param)
		return;
	mat->vec4_params[param] = to_glm(value);
}

aprend_camera aprend_camera_create(void) {
	aprend_camera_t *result = (aprend_camera_t *)malloc(sizeof(aprend_camera_t));
	result                  = new (result) aprend_camera_t();
	return result;
}

void aprend_camera_destroy(aprend_camera cam) {
	if (!cam)
		return;
	cam->~aprend_camera_t();
	free(cam);
}

void aprend_camera_set_perspective(
    aprend_camera cam,
    float fov_y,
    float aspect,
    float near_z,
    float far_z) {
	if (!cam)
		return;
	cam->projection_type = ApricotProjectionType::Perspective;
	cam->fov_y           = fov_y;
	cam->aspect          = aspect;
	cam->near_z          = near_z;
	cam->far_z           = far_z;
	cam->dirty           = true;
}

void aprend_camera_set_orthographic(
    aprend_camera cam,
    float width,
    float height,
    float near_z,
    float far_z) {
	if (!cam)
		return;
	cam->projection_type = ApricotProjectionType::Orthographic;
	cam->ortho_width     = width;
	cam->ortho_height    = height;
	cam->near_z          = near_z;
	cam->far_z           = far_z;
	cam->dirty           = true;
}

void aprend_camera_set_position(
    aprend_camera cam,
    ApriDVec3 pos) {
	if (!cam)
		return;
	cam->position = to_glm(pos);
	cam->dirty    = true;
}

void aprend_camera_set_rotation(
    aprend_camera cam,
    ApriQuat rot) {
	if (!cam)
		return;
	cam->rotation = to_glm(rot);
	cam->dirty    = true;
}

void aprend_camera_look_at(
    aprend_camera cam,
    ApriDVec3 eye,
    ApriDVec3 target,
    ApriVec3 up) {
	if (!cam)
		return;
	/* Convert look direction into rotation quaternion so recompute_camera_matrices
	 * stays consistent even when set_perspective is called after this. */
	glm::vec3 f   = glm::normalize(glm::vec3(to_glm(target)) - glm::vec3(to_glm(eye)));
	glm::vec3 s   = glm::normalize(glm::cross(f, to_glm(up)));
	glm::vec3 u   = glm::cross(s, f);
	cam->rotation = glm::quat_cast(glm::mat3(s, u, -f));
	cam->position = to_glm(eye);
	/* Recompute everything (including projection if dirty) then override view
	 * with the precise lookAt result to avoid any quat round-trip error. */
	cam->dirty = true;
	recompute_camera_matrices(cam);
	cam->view_matrix = glm::lookAt(glm::vec3(to_glm(eye)), glm::vec3(to_glm(target)), to_glm(up));
}

ApriMat4 aprend_camera_get_view(aprend_camera cam) {
	if (!cam)
		return from_glm(glm::mat4(1.f));
	if (cam->dirty)
		recompute_camera_matrices(cam);
	return from_glm(cam->view_matrix);
}

ApriMat4 aprend_camera_get_projection(aprend_camera cam) {
	if (!cam)
		return from_glm(glm::mat4(1.f));
	if (cam->dirty)
		recompute_camera_matrices(cam);
	return from_glm(cam->proj_matrix);
}

/* ============================================================
   Light
   ============================================================ */

aprend_light aprend_light_create(APREND_LIGHT_TYPE type) {
	aprend_light_t *light = (aprend_light_t *)malloc(sizeof(aprend_light_t));
	if (!light)
		return nullptr;
	light       = new (light) aprend_light_t();
	light->type = type;
	return light;
}

void aprend_light_destroy(aprend_light light) {
	if (!light)
		return;
	aprend_light_detach(light);
	light->~aprend_light_t();
	free(light);
}

void aprend_light_attach_to(
    aprend_light light,
    aprend_node node) {
	if (!light || !node)
		return;
	aprend_light_detach(light);
	node->lights.push_back(light);
	light->node = node;
}

void aprend_light_detach(aprend_light light) {
	if (!light)
		return;
	if (!light->node)
		return;
	auto &list = light->node->lights;
	list.erase(std::remove(list.begin(), list.end(), light), list.end());
	light->node = nullptr;
}

void aprend_light_set_color(
    aprend_light light,
    ApriVec4 color) {
	if (light)
		light->color = to_glm(color);
}
void aprend_light_set_intensity(
    aprend_light light,
    float intensity) {
	if (light)
		light->intensity = intensity;
}
void aprend_light_set_range(
    aprend_light light,
    float range) {
	if (light)
		light->range = range;
}
void aprend_light_set_spot_angle(
    aprend_light light,
    float degrees) {
	if (light)
		light->spot_angle = degrees;
}

/* ============================================================
   Viewport
   ============================================================ */

/* Create/destroy the BGRA8_UNORM color + depth render target used by
 * off-screen viewports, backed by aprend_framebuffer instead of a
 * hand-rolled image/view pair — same resource shape (single mip, single
 * layer, DEVICE_LOCAL), just built through the shared framebuffer machinery
 * so it gets real clear/resize support instead of duplicating it. */
static bool create_offscreen_color_target(aprend_viewport_t *vp) {
	aprend_framebuffer_texture_spec color_spec{};
	color_spec.format          = SPUDGPU_FORMAT_B8G8R8A8_UNORM;
	color_spec.shader_resource = true; // sampled by ImGui after rendering
	color_spec.use_for_gui     = true;

	aprend_framebuffer_desc fb_desc{};
	fb_desc.width            = vp->render_width;
	fb_desc.height           = vp->render_height;
	fb_desc.attachments      = &color_spec;
	fb_desc.attachment_count = 1;
	fb_desc.depth_format     = k_offscreen_depth_format;
	fb_desc.sample_count     = 1;

	vp->framebuffer       = aprend_framebuffer_create(vp->instance, &fb_desc);
	vp->depth_image_fresh = true; // depth attachment starts in UNDEFINED layout
	return vp->framebuffer != nullptr;
}

static void destroy_offscreen_color_target(aprend_viewport_t *vp) {
	aprend_framebuffer_destroy(vp->framebuffer);
	vp->framebuffer = nullptr;
}

void aprend_viewport_destroy(aprend_viewport vp) {
	if (!vp)
		return;
	aprend_framebuffer_destroy(vp->framebuffer);
	vp->~aprend_viewport_t();
	free(vp);
}

aprend_viewport aprend_viewport_create_offscreen(
    aprend_scene scene,
    aprend_camera cam,
    uint32_t width,
    uint32_t height) {
	if (!scene || !cam)
		return nullptr;
	if (!scene->instance || width == 0 || height == 0)
		return nullptr;

	aprend_viewport_t *vp = (aprend_viewport_t *)malloc(sizeof(aprend_viewport_t));
	if (!vp)
		return nullptr;
	vp                = new (vp) aprend_viewport_t();
	vp->instance      = scene->instance;
	vp->scene         = scene;
	vp->camera        = cam;
	vp->is_offscreen  = true;
	vp->render_width  = width;
	vp->render_height = height;

	if (!create_offscreen_color_target(vp))
		printf("apricot: create_offscreen_color_target failed for viewport (%ux%u)\n", width, height);

	return vp;
}

void aprend_viewport_resize(
    aprend_viewport vp,
    uint32_t width,
    uint32_t height) {
	if (!vp)
		return;
	if (!vp->is_offscreen || width == 0 || height == 0)
		return;
	if (vp->render_width == width && vp->render_height == height)
		return;

	vp->render_width  = width;
	vp->render_height = height;

	if (!aprend_framebuffer_resize(vp->framebuffer, width, height))
		printf("apricot: aprend_framebuffer_resize failed for viewport (%ux%u)\n", width, height);
	vp->depth_image_fresh = true; // resize recreated the depth attachment -> back to UNDEFINED layout
}

spudgpu_image aprend_viewport_get_color_image(aprend_viewport vp) {
	if (!vp)
		return nullptr;
	if (!vp->is_offscreen)
		return nullptr;
	return aprend_texture2d_get_spudgpu_image(aprend_framebuffer_get_color_attachment_texture(vp->framebuffer, 0));
}

spudgpu_image_view aprend_viewport_get_color_image_view(aprend_viewport vp) {
	if (!vp)
		return nullptr;
	if (!vp->is_offscreen)
		return nullptr;
	return aprend_texture2d_get_spudgpu_image_view(aprend_framebuffer_get_color_attachment_texture(vp->framebuffer, 0));
}

aprend_framebuffer aprend_viewport_get_framebuffer(aprend_viewport vp) {
	if (!vp)
		return nullptr;
	if (!vp->is_offscreen)
		return nullptr;
	return vp->framebuffer;
}

void aprend_viewport_record(
    aprend_viewport vp,
    spudgpu_command_list cmd) {
	if (!vp || !cmd)
		return;
	if (!vp->is_offscreen)
		return;

	if (vp->camera->dirty)
		recompute_camera_matrices(vp->camera);

	glm::mat4 view = vp->camera->view_matrix;
	glm::mat4 proj = vp->camera->proj_matrix;

	SPUDGPU_VIEWPORT gpu_vp{};
	gpu_vp.x        = 0.f;
	gpu_vp.y        = 0.f;
	gpu_vp.width    = static_cast<float>(vp->render_width);
	gpu_vp.height   = static_cast<float>(vp->render_height);
	gpu_vp.minDepth = 0.f;
	gpu_vp.maxDepth = 1.f;

	SPUDGPU_SCISSOR_RECT scissor{0.f, 0.f, static_cast<float>(vp->render_width), static_cast<float>(vp->render_height)};

	spudgpu_color_attachment_desc color{};
	color.image_view     = aprend_texture2d_get_spudgpu_image_view(aprend_framebuffer_get_color_attachment_texture(vp->framebuffer, 0));
	color.load_op        = SPUDGPU_LOAD_OP_CLEAR;
	color.store_op       = SPUDGPU_STORE_OP_STORE;
	color.clear_color[0] = 0.10f;
	color.clear_color[1] = 0.10f;
	color.clear_color[2] = 0.15f;
	color.clear_color[3] = 1.0f;

	// Must not call begin/end_rendering unless the color view is valid — an
	// end_rendering with no matching begin_rendering (e.g. framebuffer still
	// being (re)created) is invalid D3D12 command-list state and can wedge
	// the GPU instead of just skipping the frame.
	if (!color.image_view)
		return;

	// Depth attachment is entirely internal to Aprend — unlike the color
	// target, nothing outside this call ever samples it, so (unlike
	// aprend_viewport_get_color_image et al.) there's no caller-facing
	// barrier contract for it. It only ever needs one UNDEFINED ->
	// DEPTH_STENCIL_ATTACHMENT_OPTIMAL transition (right after creation or
	// a resize); every frame after that it's already sitting in that layout
	// from the end of the previous render pass, so no further barrier is
	// needed at all.
	aprend_texture2d depth_tex    = aprend_framebuffer_get_depth_attachment_texture(vp->framebuffer);
	spudgpu_image_view depth_view = depth_tex ? aprend_texture2d_get_spudgpu_image_view(depth_tex) : nullptr;
	if (depth_view && vp->depth_image_fresh) {
		spudgpu_cmd_image_barrier(
		    cmd, aprend_texture2d_get_spudgpu_image(depth_tex), SPUDGPU_IMAGE_LAYOUT_UNDEFINED, SPUDGPU_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		vp->depth_image_fresh = false;
	}

	spudgpu_rendering_begin_desc rp{};
	rp.color_attachments      = &color;
	rp.color_attachment_count = 1;
	rp.width                  = vp->render_width;
	rp.height                 = vp->render_height;
	if (depth_view) {
		rp.depth_attachment.image_view       = depth_view;
		rp.depth_attachment.depth_load_op    = SPUDGPU_LOAD_OP_CLEAR;
		rp.depth_attachment.depth_store_op   = SPUDGPU_STORE_OP_STORE;
		rp.depth_attachment.stencil_load_op  = SPUDGPU_LOAD_OP_DONT_CARE;
		rp.depth_attachment.stencil_store_op = SPUDGPU_STORE_OP_DONT_CARE;
		rp.depth_attachment.clear_depth      = 1.0f;
		rp.depth_attachment.clear_stencil    = 0;
	}
	spudgpu_cmd_begin_rendering(cmd, &rp);

	spudgpu_set_viewports(cmd, 0, 1, &gpu_vp);
	spudgpu_set_scissor_rects(cmd, 0, 1, &scissor);

	for (aprend_node_t *node : vp->scene->all_nodes) {
		for (aprend_geometry_t *geom : node->geometries) {
			if (!geom->mesh)
				continue;
			if (!geom->mesh->uploaded)
				continue;

			// Aprend ships no materials of its own — a geometry draws with
			// its material's pipeline if it has one, otherwise falls back to
			// the built-in debug pipeline (if aprend_renderer_init ran). A
			// geometry with neither simply can't be drawn.
			spudgpu_shader_pipeline bound_pipeline = (geom->material && geom->material->pipeline) ? geom->material->pipeline : vp->instance->default_pipeline;
			if (!bound_pipeline)
				continue;
			spudgpu_cmd_bind_pipeline(cmd, bound_pipeline);

			glm::mat4 world = glm::mat4(node->world_transform);
			glm::mat4 mvp   = proj * view * world;

			// NOTE: this push-constant layout (mat4 mvp + vec4 color) is only
			// guaranteed to match instance->default_pipeline. Custom materials
			// must currently declare a compatible push-constant range at this
			// exact layout to receive per-draw data at all — there's no
			// generic per-material push-constant/uniform delivery yet.
			ApricotPushConst pc{};
			memcpy(pc.mvp, &mvp[0][0], 64);
			glm::vec4 col = geom->material ? geom->material->color : glm::vec4(1.f);
			pc.color[0]   = col.r;
			pc.color[1]   = col.g;
			pc.color[2]   = col.b;
			pc.color[3]   = col.a;

			spudgpu_cmd_push_constants(cmd, bound_pipeline, 0, sizeof(pc), &pc);

			uint32_t binding_count = (uint32_t)geom->mesh->vertex_buffers.size();
			if (binding_count > SPUDGPU_MAX_VERTEX_BINDINGS)
				binding_count = SPUDGPU_MAX_VERTEX_BINDINGS;
			spudgpu_buffer_view vbvs[SPUDGPU_MAX_VERTEX_BINDINGS];
			for (uint32_t i = 0; i < binding_count; ++i)
				vbvs[i] = aprend_vertex_buffer_get_spudgpu_buffer_view(geom->mesh->vertex_buffers[i]);
			spudgpu_set_vertex_buffers(cmd, 0, binding_count, vbvs);

			if (geom->mesh->index_buffer) {
				spudgpu_buffer_view ibv = aprend_index_buffer_get_spudgpu_buffer_view(geom->mesh->index_buffer);
				spudgpu_set_index_buffer(cmd, ibv);
				spudgpu_draw_indexed(cmd, geom->mesh->index_count, 0, 0);
			} else {
				spudgpu_draw(cmd, geom->mesh->vertex_count, 0);
			}
		}
	}

	spudgpu_cmd_end_rendering(cmd);
}
}
