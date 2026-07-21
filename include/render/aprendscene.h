#ifndef APREND_SCENE_H
#define APREND_SCENE_H

#include "aprendbuffers.h"
#include "aprendframes.h"
#include "aprimath.h"
#include <spudgpu.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aprend_scene_t *aprend_scene;
typedef struct aprend_node_t *aprend_node;
typedef struct aprend_geometry_t *aprend_geometry;
typedef struct aprend_camera_t *aprend_camera;
typedef struct aprend_mesh_t *aprend_mesh;
typedef struct aprend_material_t *aprend_material;
typedef struct aprend_light_t *aprend_light;
typedef struct aprend_viewport_t *aprend_viewport;

/*
 * Renderer module lifecycle  —  per-instance, optional
 *
 * Builds Aprend's built-in debug shader pipeline (see aprend_material for why
 * it's the only shader Aprend ships) on the given instance's device. Not
 * required — geometries whose materials supply their own pipeline don't need
 * this to have run. Safe to skip entirely if every material is custom.
 */

bool aprend_renderer_init(aprend_instance instance);
void aprend_renderer_shutdown(aprend_instance instance);

/*
 * Scene  —  the universe root
 * Owns all nodes and geometry. Destroying the scene destroys everything in it.
 */

aprend_scene aprend_scene_create(aprend_instance instance);
void aprend_scene_destroy(aprend_scene scene);
void aprend_scene_update(
    aprend_scene scene,
    float delta_time);

/*
 * Node  —  spatial container
 * Has a local transform. Propagates world transforms to children.
 */

aprend_node aprend_node_create(
    aprend_scene scene,
    const char *name);
void aprend_node_destroy(aprend_node node);
void aprend_node_attach(
    aprend_node parent,
    aprend_node child);
void aprend_node_detach(aprend_node node);

void aprend_node_set_translation(
    aprend_node node,
    ApriDVec3 pos);
void aprend_node_set_rotation(
    aprend_node node,
    ApriQuat rot);
void aprend_node_set_scale(
    aprend_node node,
    ApriVec3 scale);

ApriDVec3 aprend_node_get_world_translation(aprend_node node);
ApriMat4 aprend_node_get_world_transform(aprend_node node);

/*
 * Geometry  —  leaf node
 * Carries a mesh and material. Attaches to a node to enter the scene graph.
 * Has no children — use a node parent if grouping is needed.
 */

aprend_geometry aprend_geometry_create(
    aprend_scene scene,
    const char *name);
void aprend_geometry_destroy(aprend_geometry geom);
void aprend_geometry_attach_to(
    aprend_geometry geom,
    aprend_node parent);
void aprend_geometry_detach(aprend_geometry geom);
void aprend_geometry_set_mesh(
    aprend_geometry geom,
    aprend_mesh mesh);
void aprend_geometry_set_material(
    aprend_geometry geom,
    aprend_material mat);

/*
 * Mesh  —  vertex and index data
 * Call aprend_mesh_upload() to push data to the GPU via SpudGPU.
 */

// Per-binding stride descriptor
typedef struct aprend_vertex_binding {
	uint32_t binding;            // which data stream
	aprend_buffer_layout layout; // describes the attributes in this binding
} aprend_vertex_binding;

aprend_mesh aprend_mesh_create(aprend_instance instance);
void aprend_mesh_destroy(aprend_mesh mesh);
void aprend_mesh_set_vertex_bindings(
    aprend_mesh mesh,
    const aprend_vertex_binding *bindings,
    uint32_t binding_count);
void aprend_mesh_set_vertex_binding_data(
    aprend_mesh mesh,
    uint32_t binding,
    uint32_t vertex_count,
    const void *data);
void aprend_mesh_set_indices(
    aprend_mesh mesh,
    APREND_INDEX_STRIDE stride,
    uint32_t index_count,
    const void *data);
void aprend_mesh_upload(aprend_mesh mesh);
bool aprend_mesh_is_uploaded(aprend_mesh mesh);

/*
 * Material  —  a shader pipeline plus its per-draw parameters.
 *
 * Aprend has no predefined shaders or materials of its own — it only builds
 * a real SpudGPU graphics pipeline from caller-supplied SPIR-V and pipeline
 * state. aprend_material_desc mirrors spudgpu_shader_pipeline_desc (see
 * spudgpu.h) field-for-field except that the shader stages are raw SPIR-V
 * bytecode rather than pre-created spudgpu_shader_module handles — Aprend
 * creates the modules, builds the pipeline, and discards the modules again,
 * so the caller never touches spudgpu_shader_module at all.
 *
 * Geometries with no material (or a material with no pipeline) fall back to
 * Aprend's built-in debug shader — see aprend_renderer_init. That fallback
 * is the only shader Aprend ships; everything else is Apcad's (or whichever
 * consumer's) responsibility.
 *
 * Only vertex + fragment stages are supported for now; geometry/tessellation
 * stages aren't exposed here since nothing in this codebase exercises them
 * yet.
 */

typedef struct aprend_material_desc {
	const void *vertex_spirv;
	uint64_t vertex_spirv_size;
	const char *vertex_entry_point; /* NULL defaults to "main" */

	const void *fragment_spirv;
	uint64_t fragment_spirv_size;
	const char *fragment_entry_point; /* NULL defaults to "main" */

	spudgpu_vertex_attribute_desc vertex_attributes[SPUDGPU_MAX_VERTEX_ATTRIBUTES];
	uint32_t vertex_attribute_count;
	spudgpu_vertex_binding_desc vertex_bindings[SPUDGPU_MAX_VERTEX_BINDINGS];
	uint32_t vertex_binding_count;

	SPUDGPU_PRIMITIVE_TOPOLOGY primitive_topology;
	SPUDGPU_CULL_MODE cull_mode;
	bool front_face_ccw;
	bool wireframe;

	bool depth_test_enable;
	bool depth_write_enable;
	SPUDGPU_COMPARE_OP depth_compare_op;

	spudgpu_blend_attachment_desc blend_attachment;

	/// Must match the format of whatever framebuffer/viewport this material
	/// will actually render into (see spudgpu_cmd_begin_rendering).
	SPUDGPU_FORMAT color_attachment_format;
	/// SPUDGPU_FORMAT_UNKNOWN for no depth attachment.
	SPUDGPU_FORMAT depth_format;

	void *descriptor_set_layouts[SPUDGPU_MAX_DESCRIPTOR_SET_LAYOUTS];
	uint32_t descriptor_set_layout_count;
	spudgpu_push_constant_range_desc push_constant_ranges[SPUDGPU_MAX_PUSH_CONSTANT_RANGES];
	uint32_t push_constant_range_count;
} aprend_material_desc;

aprend_material aprend_material_create(
    aprend_instance instance,
    const aprend_material_desc *desc);
void aprend_material_destroy(aprend_material mat);
spudgpu_shader_pipeline aprend_material_get_spudgpu_pipeline(aprend_material mat);
void aprend_material_set_color(
    aprend_material mat,
    ApriVec4 color);
void aprend_material_set_float(
    aprend_material mat,
    const char *param,
    float value);
void aprend_material_set_vec4(
    aprend_material mat,
    const char *param,
    ApriVec4 value);

aprend_camera aprend_camera_create(void);
void aprend_camera_destroy(aprend_camera cam);
void aprend_camera_set_perspective(
    aprend_camera cam,
    float fov_y,
    float aspect,
    float near_z,
    float far_z);
void aprend_camera_set_orthographic(
    aprend_camera cam,
    float width,
    float height,
    float near_z,
    float far_z);
void aprend_camera_set_position(
    aprend_camera cam,
    ApriDVec3 pos);
void aprend_camera_set_rotation(
    aprend_camera cam,
    ApriQuat rot);
void aprend_camera_look_at(
    aprend_camera cam,
    ApriDVec3 eye,
    ApriDVec3 target,
    ApriVec3 up);
ApriMat4 aprend_camera_get_view(aprend_camera cam);
ApriMat4 aprend_camera_get_projection(aprend_camera cam);

// Lights attach to nodes so they move with the scene graph.

typedef uint32_t APREND_LIGHT_TYPE;

enum { APREND_LIGHT_AMBIENT = 0, APREND_LIGHT_DIRECTIONAL = 1, APREND_LIGHT_POINT = 2, APREND_LIGHT_SPOT = 3 };

aprend_light aprend_light_create(APREND_LIGHT_TYPE type);
void aprend_light_destroy(aprend_light light);
void aprend_light_attach_to(
    aprend_light light,
    aprend_node node);
void aprend_light_detach(aprend_light light);
void aprend_light_set_color(
    aprend_light light,
    ApriVec4 color);
void aprend_light_set_intensity(
    aprend_light light,
    float intensity);
void aprend_light_set_range(
    aprend_light light,
    float range); /* point / spot */
void aprend_light_set_spot_angle(
    aprend_light light,
    float degrees); /* spot only */

/* ============================================================
   Viewport  —  render target + camera

     aprend_viewport_create_offscreen — renders to an internally-managed
                                         off-screen image; use
                                         aprend_viewport_get_color_image_view()
                                         to register it with ImGui or other
                                         compositors.

   Aprend does not do picking — see apcadraycast.h for CPU ray-vs-geometry
   picking against ApCAD solids, which needs no GPU pass or readback.
   ============================================================ */

/* Off-screen variant: creates a (width × height) BGRA8_UNORM render target.
 * Call aprend_viewport_record() each frame instead of aprend_viewport_render(). */
aprend_viewport aprend_viewport_create_offscreen(
    aprend_scene scene,
    aprend_camera cam,
    uint32_t width,
    uint32_t height);

void aprend_viewport_destroy(aprend_viewport vp);

void aprend_viewport_resize(
    aprend_viewport vp,
    uint32_t width,
    uint32_t height);

void aprend_viewport_set_rect(
    aprend_viewport vp,
    float x,
    float y,
    float w,
    float h); /* normalized 0-1 */
void aprend_viewport_render(aprend_viewport vp);

/* Off-screen only: records the scene render pass into an externally-provided
 * command list.  The caller must:
 *   1. Transition the color image to COLOR_ATTACHMENT_OPTIMAL before this call.
 *   2. Transition it to SHADER_READ_ONLY after this call (before ImGui sampling).
 *   3. Handle submit and present themselves.
 * No-op for swap-chain viewports. */
void aprend_viewport_record(
    aprend_viewport vp,
    spudgpu_command_list cmd);

/* Off-screen accessors — returns NULL for swap-chain viewports. */
spudgpu_image aprend_viewport_get_color_image(aprend_viewport vp);
spudgpu_image_view aprend_viewport_get_color_image_view(aprend_viewport vp);

/* The framebuffer backing the viewport's color target. Owned by the
 * viewport — do not destroy it directly, and don't hold onto it across an
 * aprend_viewport_resize (the attachments are recreated in place, but a
 * future implementation could reallocate the framebuffer itself). */
aprend_framebuffer aprend_viewport_get_framebuffer(aprend_viewport vp);

#ifdef __cplusplus
}
#endif

#endif // APREND_SCENE_H
