#ifndef APRICOT_RENDERER_H
#define APRICOT_RENDERER_H

#include "../apricot_math.h"
#include "../../spudlib/include/spudgpu.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* ============================================================
   Opaque Handles
   ============================================================ */

typedef struct apricot_scene_t*    apricot_scene;
typedef struct apricot_node_t*     apricot_node;
typedef struct apricot_geometry_t* apricot_geometry;
typedef struct apricot_camera_t*   apricot_camera;
typedef struct apricot_mesh_t*     apricot_mesh;
typedef struct apricot_material_t* apricot_material;
typedef struct apricot_light_t*    apricot_light;
typedef struct apricot_viewport_t* apricot_viewport;


/* ============================================================
   Module Lifecycle
   ============================================================ */

bool apricot_renderer_init(spudgpu_device device);
void apricot_renderer_shutdown(void);


/* ============================================================
   Scene  —  the universe root
   Owns all nodes and geometry. Destroying the scene destroys everything in it.
   ============================================================ */

apricot_scene apricot_scene_create(void);
void          apricot_scene_destroy(apricot_scene scene);
void          apricot_scene_update(apricot_scene scene, float delta_time);


/* ============================================================
   Node  —  spatial container
   Has a local transform. Propagates world transforms to children.
   ============================================================ */

apricot_node apricot_node_create(apricot_scene scene, const char *name);
void         apricot_node_destroy(apricot_node node);
void         apricot_node_attach(apricot_node parent, apricot_node child);
void         apricot_node_detach(apricot_node node);

void         apricot_node_set_translation(apricot_node node, ApriDVec3 pos);
void         apricot_node_set_rotation(apricot_node node, ApriQuat rot);
void         apricot_node_set_scale(apricot_node node, ApriVec3 scale);

ApriDVec3    apricot_node_get_world_translation(apricot_node node);
ApriMat4     apricot_node_get_world_transform(apricot_node node);


/* ============================================================
   Geometry  —  leaf node
   Carries a mesh and material. Attaches to a node to enter the scene graph.
   Has no children — use a node parent if grouping is needed.
   ============================================================ */

apricot_geometry apricot_geometry_create(apricot_scene scene, const char *name);
void             apricot_geometry_destroy(apricot_geometry geom);
void             apricot_geometry_attach_to(apricot_geometry geom, apricot_node parent);
void             apricot_geometry_detach(apricot_geometry geom);
void             apricot_geometry_set_mesh(apricot_geometry geom, apricot_mesh mesh);
void             apricot_geometry_set_material(apricot_geometry geom, apricot_material mat);


/* ============================================================
   Mesh  —  vertex and index data
   Call apricot_mesh_upload() to push data to the GPU via SpudGPU.
   ============================================================ */

typedef struct {
    ApriVec3 position;
    ApriVec3 normal;
    ApriVec2 uv;
    ApriVec4 color;
} ApricotVertex;

apricot_mesh apricot_mesh_create(void);
void         apricot_mesh_destroy(apricot_mesh mesh);
void         apricot_mesh_set_vertices(apricot_mesh mesh, const ApricotVertex *vertices, uint32_t count);
void         apricot_mesh_set_indices(apricot_mesh mesh, const uint32_t *indices, uint32_t count);
void         apricot_mesh_upload(apricot_mesh mesh);


/* ============================================================
   Material  —  shader + parameters
   ============================================================ */

apricot_material apricot_material_create(const char *shader_name);
void             apricot_material_destroy(apricot_material mat);
void             apricot_material_set_color(apricot_material mat, ApriVec4 color);
void             apricot_material_set_float(apricot_material mat, const char *param, float value);
void             apricot_material_set_vec4(apricot_material mat, const char *param, ApriVec4 value);


/* ============================================================
   Camera
   ============================================================ */

apricot_camera apricot_camera_create(void);
void           apricot_camera_destroy(apricot_camera cam);
void           apricot_camera_set_perspective(apricot_camera cam, float fov_y, float aspect, float near_z, float far_z);
void           apricot_camera_set_orthographic(apricot_camera cam, float width, float height, float near_z, float far_z);
void           apricot_camera_set_position(apricot_camera cam, ApriDVec3 pos);
void           apricot_camera_set_rotation(apricot_camera cam, ApriQuat rot);
void           apricot_camera_look_at(apricot_camera cam, ApriDVec3 eye, ApriDVec3 target, ApriVec3 up);
ApriMat4       apricot_camera_get_view(apricot_camera cam);
ApriMat4       apricot_camera_get_projection(apricot_camera cam);


/* ============================================================
   Light
   Lights attach to nodes so they move with the scene graph.
   ============================================================ */

typedef uint32_t APRICOT_LIGHT_TYPE;

enum {
    APRICOT_LIGHT_AMBIENT     = 0,
    APRICOT_LIGHT_DIRECTIONAL = 1,
    APRICOT_LIGHT_POINT       = 2,
    APRICOT_LIGHT_SPOT        = 3
};

apricot_light apricot_light_create(APRICOT_LIGHT_TYPE type);
void          apricot_light_destroy(apricot_light light);
void          apricot_light_attach_to(apricot_light light, apricot_node node);
void          apricot_light_detach(apricot_light light);
void          apricot_light_set_color(apricot_light light, ApriVec4 color);
void          apricot_light_set_intensity(apricot_light light, float intensity);
void          apricot_light_set_range(apricot_light light, float range);        /* point / spot */
void          apricot_light_set_spot_angle(apricot_light light, float degrees); /* spot only */


/* ============================================================
   Picking
   The scene maintains a pick_registry mapping compact uint32 pick IDs
   to geometry handles. The viewport renders an offscreen R32G32_UINT
   texture (one pick ID per pixel) before the main pass. The CPU reads
   one pixel at the cursor position to resolve hover/selection.

   Pixel format: uvec2(pick_id, 0)
     pick_id 0  = nothing hit (clear value)
     pick_id >0 = index into the scene's pick_registry
   ============================================================ */

/* Contents of the picking state uniform buffer bound to every real shader.
 * The real fragment shader compares its own pick_id against these two values
 * to decide whether to render as hovered or selected. */
typedef struct {
    uint32_t hovered_id;   /* pick_id currently under cursor, 0 = none  */
    uint32_t selected_id;  /* pick_id of last clicked geometry, 0 = none */
    uint32_t cursor_x;     /* cursor screen position in pixels            */
    uint32_t cursor_y;
} ApricotPickingState;

/* Update cursor position — called by the front-end on every mouse-move event.
 * Internally updates the picking state UBO so the real shader highlights hover. */
void apricot_scene_set_cursor(apricot_scene scene, uint32_t x, uint32_t y);

/* Explicitly set the selected geometry. Pass NULL to clear selection. */
void apricot_scene_set_selected(apricot_scene scene, apricot_geometry geom);

/* Query current hover and selection state. */
apricot_geometry apricot_scene_get_hovered(apricot_scene scene);
apricot_geometry apricot_scene_get_selected(apricot_scene scene);


/* ============================================================
   Viewport  —  render target + camera

   Two creation paths:
     apricot_viewport_create          — renders directly to a swap chain image.
     apricot_viewport_create_offscreen — renders to an internally-managed
                                         off-screen image; use
                                         apricot_viewport_get_color_image_view()
                                         to register it with ImGui or other
                                         compositors.

   Internally runs a picking pass first, then the main pass.
   ============================================================ */

apricot_viewport apricot_viewport_create(apricot_scene scene, apricot_camera cam, spudgpu_swap_chain swap_chain);

/* Off-screen variant: creates a (width × height) BGRA8_UNORM render target.
 * Call apricot_viewport_record() each frame instead of apricot_viewport_render(). */
apricot_viewport apricot_viewport_create_offscreen(
    apricot_scene scene, apricot_camera cam,
    uint32_t width, uint32_t height);

void             apricot_viewport_destroy(apricot_viewport vp);

/* Resize an off-screen viewport.  The caller MUST guarantee the GPU is idle
 * (e.g. vkDeviceWaitIdle) before calling this, and MUST re-register the new
 * color image view with any external consumer (ImGui, etc.) afterward via
 * apricot_viewport_get_color_image_view().  No-op for swap-chain viewports
 * or when the dimensions are unchanged. */
void             apricot_viewport_resize(apricot_viewport vp, uint32_t width, uint32_t height);

void             apricot_viewport_set_rect(apricot_viewport vp, float x, float y, float w, float h); /* normalized 0-1 */
void             apricot_viewport_render(apricot_viewport vp);

/* Off-screen only: records the scene render pass into an externally-provided
 * command list.  The caller must:
 *   1. Transition the color image to COLOR_ATTACHMENT_OPTIMAL before this call.
 *   2. Transition it to SHADER_READ_ONLY after this call (before ImGui sampling).
 *   3. Handle submit and present themselves.
 * No-op for swap-chain viewports. */
void             apricot_viewport_record(apricot_viewport vp, spudgpu_command_list cmd);

/* Off-screen accessors — returns NULL for swap-chain viewports. */
spudgpu_image      apricot_viewport_get_color_image(apricot_viewport vp);
spudgpu_image_view apricot_viewport_get_color_image_view(apricot_viewport vp);

/* Read the geometry under a pixel position from the last picking pass.
 * Returns NULL if nothing was hit or the picking pass has not run yet.
 * Call this after apricot_viewport_render() / apricot_viewport_record(), not before. */
apricot_geometry apricot_viewport_query_pick(apricot_viewport vp, uint32_t x, uint32_t y);


#ifdef __cplusplus
}
#endif

#endif /* APRICOT_RENDERER_H */
