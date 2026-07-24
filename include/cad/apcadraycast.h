
#ifndef APCAD_RAYCAST_H
#define APCAD_RAYCAST_H

#include "aprimath.h"
#include "cad/apcadmesh.h"
#include "cad/apcadsolid.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CPU ray-vs-solid picking.
 *
 * This is deliberately not a GPU pick-ID buffer: a solid's face count is
 * small enough that testing a ray against every face directly is cheap
 * (no render pass, no readback, no per-frame cost), and CAD interaction
 * needs the actual 3D hit point/face/normal for snapping and measuring,
 * which a screen-space pixel ID can never give you.
 *
 * Operates entirely in the solid's own local space (the same space its
 * vertices are stored in) — this module has no concept of world transforms,
 * nodes, or scenes. To pick against a placed entity, transform the
 * world-space ray into that entity's local space (the inverse of its
 * node's world transform) before calling apcad_solid_raycast, and transform
 * the resulting hit position/normal back to world space afterward.
 */

typedef struct apcad_ray {
	ApriVec3 origin;
	ApriVec3 direction; /* caller must pass a normalized direction */
} apcad_ray;

typedef struct apcad_raycast_hit {
	float distance;      /* along the ray, in local-space units */
	ApriVec3 position;   /* local-space hit point */
	ApriVec3 normal;     /* outward face normal at the hit point */
	uint32_t face_index; /* index into the solid's face list */
} apcad_raycast_hit;

/* Tests the ray against every face of `solid` and returns the closest hit
 * in front of the ray origin. No acceleration structure — brute force over
 * a single solid's faces is the right call at this scale; a BVH is a
 * separate future concern once there's a scene of many solids to test
 * against at once, not something this per-solid query needs. */
bool apcad_solid_raycast(apcad_solid solid, apcad_ray ray, apcad_raycast_hit *out_hit);

/* Same contract as apcad_solid_raycast, against an apcad_mesh instead --
 * `face_index` on a hit is an apcad_face_id. Implemented purely through
 * apcadmesh.h's public query API (face_vertex/vertex_position/
 * face_normal), never through apcad_mesh's internal storage, so it can't
 * drift out of sync with how apcadmesh.cpp actually lays out a face.
 * Non-triangular faces are fan-triangulated the same way
 * apcad_mesh_tessellate does, inheriting the same convex-face assumption
 * (see apcad_mesh_add_edge's "known gaps" comment in apcadmesh.h). This is
 * the foundation apcad_mesh_infer's ON_FACE fallback and the Push/Pull
 * tool's "which face did the drag start on" test are both built on (see
 * apcadinference.h / apcaddraw.h). */
bool apcad_mesh_raycast(apcad_mesh mesh, apcad_ray ray, apcad_raycast_hit *out_hit);

/*
 * Ray construction / space conversion helpers.
 *
 * These exist so no host (SDL+ImGui, SwiftUI, whatever) has to re-derive
 * cursor->world-ray unprojection itself. That math has exactly one place a
 * convention bug can hide -- the NDC.y direction -- and it already caused a
 * real, hard-to-diagnose bug once when it lived in a host app instead of
 * here (see apcad_camera_unproject_ray's own comment below).
 */

/* Builds a world-space ray from a cursor position given in NDC space.
 *
 * ndc_x, ndc_y: in [-1, 1]. Convention: Y increases DOWNWARD, matching
 * aprend_camera_get_projection's matrix, which already carries a
 * Vulkan-specific proj[1][1] *= -1 correction (necessary because Vulkan's
 * rasterizer expects NDC.y opposite to vanilla OpenGL). That correction
 * puts the matrix's own NDC convention in Vulkan's native direction --
 * same direction screen pixels increase in -- so do NOT flip Y again
 * before calling this; the flip already happened upstream, and doing it
 * twice double-corrects for something already fixed.
 *
 * eye_world: the camera's current world-space eye position. Passed
 * explicitly rather than queried from `cam`, since aprend_camera tracks
 * only its view/projection matrices, not a separately queryable eye point.
 */
apcad_ray apcad_camera_unproject_ray(aprend_camera cam, ApriDVec3 eye_world, float ndc_x, float ndc_y);

/* Transforms a world-space ray into node_world_transform's local space --
 * the inverse of that matrix applied to origin (as a point) and direction
 * (as a vector). apcad_solid_raycast only understands local space; this is
 * how a caller gets a world-space ray into a shape to test it against a
 * placed entity. */
apcad_ray apcad_ray_to_local(apcad_ray world_ray, ApriMat4 node_world_transform);

/* Inverse of apcad_ray_to_local, for a hit point rather than a ray: local
 * space -> world space. Needed wherever a raycast_hit's position has to be
 * recorded in world space (e.g. a rotation pivot) rather than re-derived
 * from local space every frame. */
ApriVec3 apcad_local_point_to_world(ApriVec3 local_point, ApriMat4 node_world_transform);

#ifdef __cplusplus
}
#endif

#endif // APCAD_RAYCAST_H
