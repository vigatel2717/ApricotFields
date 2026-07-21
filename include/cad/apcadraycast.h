
#ifndef APCAD_RAYCAST_H
#define APCAD_RAYCAST_H

#include "aprimath.h"
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

typedef struct {
	ApriVec3 origin;
	ApriVec3 direction; /* caller must pass a normalized direction */
} apcad_ray;

typedef struct {
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

#ifdef __cplusplus
}
#endif

#endif // APCAD_RAYCAST_H
