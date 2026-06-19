#ifndef APREND_SCENE_H
#define APREND_SCENE_H

#include "aprimath.h"
#include "aprendbuffers.h"
#include "spudgpu.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* ============================================================
      Opaque Handles
      ============================================================ */

   typedef struct aprend_scene_t *aprend_scene;
   typedef struct aprend_node_t *aprend_node;
   typedef struct aprend_geometry_t *aprend_geometry;
   typedef struct aprend_camera_t *aprend_camera;
   typedef struct aprend_mesh_t *aprend_mesh;
   typedef struct aprend_material_t *aprend_material;
   typedef struct aprend_light_t *aprend_light;
   typedef struct aprend_viewport_t *aprend_viewport;

   /* ============================================================
      Scene  —  the universe root
      Owns all nodes and geometry. Destroying the scene destroys everything in it.
      ============================================================ */

   aprend_scene aprend_scene_create(aprend_instance instance);
   void aprend_scene_destroy(aprend_scene scene);
   void aprend_scene_update(aprend_scene scene, float delta_time);

   /* ============================================================
      Node  —  spatial container
      Has a local transform. Propagates world transforms to children.
      ============================================================ */

   aprend_node aprend_node_create(aprend_scene scene, const char *name);
   void aprend_node_destroy(aprend_node node);
   void aprend_node_attach(aprend_node parent, aprend_node child);
   void aprend_node_detach(aprend_node node);

   void aprend_node_set_translation(aprend_node node, ApriDVec3 pos);
   void aprend_node_set_rotation(aprend_node node, ApriQuat rot);
   void aprend_node_set_scale(aprend_node node, ApriVec3 scale);

   ApriDVec3 aprend_node_get_world_translation(aprend_node node);
   ApriMat4 aprend_node_get_world_transform(aprend_node node);

   /* ============================================================
      Geometry  —  leaf node
      Carries a mesh and material. Attaches to a node to enter the scene graph.
      Has no children — use a node parent if grouping is needed.
      ============================================================ */

   aprend_geometry aprend_geometry_create(aprend_scene scene, const char *name);
   void aprend_geometry_destroy(aprend_geometry geom);
   void aprend_geometry_attach_to(aprend_geometry geom, aprend_node parent);
   void aprend_geometry_detach(aprend_geometry geom);
   void aprend_geometry_set_mesh(aprend_geometry geom, aprend_mesh mesh);
   void aprend_geometry_set_material(aprend_geometry geom, aprend_material mat);

   /* ============================================================
      Mesh  —  vertex and index data
      Call aprend_mesh_upload() to push data to the GPU via SpudGPU.
      ============================================================ */

   // Per-binding stride descriptor
   typedef struct aprend_vertex_binding
   {
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

   /* ============================================================
      Material  —  shader + parameters
      ============================================================ */

   aprend_material aprend_material_create(const char *shader_name);
   void aprend_material_destroy(aprend_material mat);
   void aprend_material_set_color(aprend_material mat, ApriVec4 color);
   void aprend_material_set_float(aprend_material mat, const char *param, float value);
   void aprend_material_set_vec4(aprend_material mat, const char *param, ApriVec4 value);

   /* ============================================================
      Camera
      ============================================================ */

   aprend_camera aprend_camera_create(void);
   void aprend_camera_destroy(aprend_camera cam);
   void aprend_camera_set_perspective(aprend_camera cam, float fov_y, float aspect, float near_z, float far_z);
   void aprend_camera_set_orthographic(aprend_camera cam, float width, float height, float near_z, float far_z);
   void aprend_camera_set_position(aprend_camera cam, ApriDVec3 pos);
   void aprend_camera_set_rotation(aprend_camera cam, ApriQuat rot);
   void aprend_camera_look_at(aprend_camera cam, ApriDVec3 eye, ApriDVec3 target, ApriVec3 up);
   ApriMat4 aprend_camera_get_view(aprend_camera cam);
   ApriMat4 aprend_camera_get_projection(aprend_camera cam);

   /* ============================================================
      Light
      Lights attach to nodes so they move with the scene graph.
      ============================================================ */

   typedef uint32_t APRICOT_LIGHT_TYPE;

   enum
   {
      APRICOT_LIGHT_AMBIENT = 0,
      APRICOT_LIGHT_DIRECTIONAL = 1,
      APRICOT_LIGHT_POINT = 2,
      APRICOT_LIGHT_SPOT = 3
   };

   aprend_light aprend_light_create(APRICOT_LIGHT_TYPE type);
   void aprend_light_destroy(aprend_light light);
   void aprend_light_attach_to(aprend_light light, aprend_node node);
   void aprend_light_detach(aprend_light light);
   void aprend_light_set_color(aprend_light light, ApriVec4 color);
   void aprend_light_set_intensity(aprend_light light, float intensity);
   void aprend_light_set_range(aprend_light light, float range);        /* point / spot */
   void aprend_light_set_spot_angle(aprend_light light, float degrees); /* spot only */

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
   typedef struct
   {
      uint32_t hovered_id;  /* pick_id currently under cursor, 0 = none  */
      uint32_t selected_id; /* pick_id of last clicked geometry, 0 = none */
      uint32_t cursor_x;    /* cursor screen position in pixels            */
      uint32_t cursor_y;
   } ApricotPickingState;

   /* Update cursor position — called by the front-end on every mouse-move event.
    * Internally updates the picking state UBO so the real shader highlights hover. */
   void aprend_scene_set_cursor(aprend_scene scene, uint32_t x, uint32_t y);

   /* Explicitly set the selected geometry. Pass NULL to clear selection. */
   void aprend_scene_set_selected(aprend_scene scene, aprend_geometry geom);

   /* Query current hover and selection state. */
   aprend_geometry aprend_scene_get_hovered(aprend_scene scene);
   aprend_geometry aprend_scene_get_selected(aprend_scene scene);

   /* ============================================================
      Viewport  —  render target + camera

      Two creation paths:
        aprend_viewport_create          — renders directly to a swap chain image.
        aprend_viewport_create_offscreen — renders to an internally-managed
                                            off-screen image; use
                                            aprend_viewport_get_color_image_view()
                                            to register it with ImGui or other
                                            compositors.

      Internally runs a picking pass first, then the main pass.
      ============================================================ */

   aprend_viewport aprend_viewport_create(aprend_scene scene, aprend_camera cam, spudgpu_swap_chain swap_chain);

   /* Off-screen variant: creates a (width × height) BGRA8_UNORM render target.
    * Call aprend_viewport_record() each frame instead of aprend_viewport_render(). */
   aprend_viewport aprend_viewport_create_offscreen(
       aprend_scene scene, aprend_camera cam,
       uint32_t width, uint32_t height);

   void aprend_viewport_destroy(aprend_viewport vp);

   /* Resize an off-screen viewport.  The caller MUST guarantee the GPU is idle
    * (e.g. vkDeviceWaitIdle) before calling this, and MUST re-register the new
    * color image view with any external consumer (ImGui, etc.) afterward via
    * aprend_viewport_get_color_image_view().  No-op for swap-chain viewports
    * or when the dimensions are unchanged. */
   void aprend_viewport_resize(aprend_viewport vp, uint32_t width, uint32_t height);

   void aprend_viewport_set_rect(aprend_viewport vp, float x, float y, float w, float h); /* normalized 0-1 */
   void aprend_viewport_render(aprend_viewport vp);

   /* Off-screen only: records the scene render pass into an externally-provided
    * command list.  The caller must:
    *   1. Transition the color image to COLOR_ATTACHMENT_OPTIMAL before this call.
    *   2. Transition it to SHADER_READ_ONLY after this call (before ImGui sampling).
    *   3. Handle submit and present themselves.
    * No-op for swap-chain viewports. */
   void aprend_viewport_record(aprend_viewport vp, spudgpu_command_list cmd);

   /* Off-screen accessors — returns NULL for swap-chain viewports. */
   spudgpu_image aprend_viewport_get_color_image(aprend_viewport vp);
   spudgpu_image_view aprend_viewport_get_color_image_view(aprend_viewport vp);

   /* Read the geometry under a pixel position from the last picking pass.
    * Returns NULL if nothing was hit or the picking pass has not run yet.
    * Call this after aprend_viewport_render() / aprend_viewport_record(), not before. */
   aprend_geometry aprend_viewport_query_pick(aprend_viewport vp, uint32_t x, uint32_t y);

#ifdef __cplusplus
}
#endif

#endif // APREND_SCENE_H
