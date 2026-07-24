
#ifndef APCAD_INFERENCE_H
#define APCAD_INFERENCE_H

#include "aprimath.h"
#include "cad/apcadmesh.h"
#include "cad/apcadraycast.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Inference  —  SketchUp-style semantic snapping
 *
 * DRAFT for review — not yet implemented. This is what makes drawing feel
 * "smart": while a Line (or Push/Pull, or Move) is tracking the cursor, it
 * doesn't just ask "where does this ray hit the mesh" (apcad_solid_raycast/
 * apcad_mesh already answer that) — it asks "is there a nearby point with
 * *meaning* — an endpoint, a midpoint, this edge, this face, this axis
 * relative to where I started — that the cursor should snap to instead of
 * its raw hit point." That semantic answer, plus which kind of relationship
 * it is, is what drives the colored inference cues (SketchUp's red/green/
 * blue axis highlight, magenta "parallel to edge", cyan "perpendicular")
 * that make the tool communicate what it's about to do before the user
 * commits. Drawing entirely without this still works (raw raycast hit
 * points), it just doesn't feel like SketchUp.
 *
 * Operates in the mesh's own local space, same convention as
 * apcad_solid_raycast / apcad_ray_to_local -- callers transform the
 * world-space cursor ray into mesh-local space before calling in, and the
 * returned position back to world space afterward.
 */

typedef enum {
	APCAD_INFERENCE_NONE = 0,
	APCAD_INFERENCE_ENDPOINT, /* snapped to an existing vertex */
	APCAD_INFERENCE_MIDPOINT, /* snapped to an edge's midpoint */
	APCAD_INFERENCE_ON_EDGE,  /* snapped to closest point on an edge, not its endpoints/midpoint */
	APCAD_INFERENCE_ON_FACE,  /* snapped to the ray's hit point on a face, no stronger inference available */
	APCAD_INFERENCE_AXIS_X,   /* `from` is set and the cursor is near the world/local X axis through it */
	APCAD_INFERENCE_AXIS_Y,
	APCAD_INFERENCE_AXIS_Z,
	APCAD_INFERENCE_PARALLEL,      /* `from` is set and the cursor direction is near-parallel to a nearby edge */
	APCAD_INFERENCE_PERPENDICULAR, /* likewise, near-perpendicular to a nearby edge */
	/* Last-resort fallback: ray intersected with the local Y=0 reference
	 * plane (matching the Y-up, extrude-from-y=0 convention documented in
	 * apcadmesh.h/apcadsolid.h). Only ever returned when nothing else in
	 * the mesh qualifies -- in particular, this is what lets a Line tool
	 * place its very first point on a completely empty mesh, where there
	 * are no vertices/edges/faces yet for anything above to find. */
	APCAD_INFERENCE_ON_GROUND,
} APCAD_INFERENCE_TYPE;

typedef struct apcad_inference_hit {
	APCAD_INFERENCE_TYPE type;
	ApriVec3 position;      /* local-space snapped point -- what the calling tool should actually use */
	apcad_vertex_id vertex; /* valid for ENDPOINT, else APCAD_INVALID_ID */
	apcad_edge_id edge;     /* valid for MIDPOINT/ON_EDGE/PARALLEL/PERPENDICULAR (the edge being related to), else APCAD_INVALID_ID */
	apcad_face_id face;     /* valid for ON_FACE, else APCAD_INVALID_ID */
} apcad_inference_hit;

/* Finds the single best inference for a cursor ray against `mesh`.
 *
 * `from`: the already-placed first endpoint of the segment being drawn
 * (e.g. Line tool's click-1 point), or NULL if there isn't one yet.
 * AXIS_* / PARALLEL / PERPENDICULAR results only ever come back when `from` is
 * non-NULL, since those are relationships between two points, not
 * properties of one -- a bare cursor hover has nothing to be parallel to.
 *
 * `snap_radius`: local-space distance a candidate must fall within to be
 * considered at all (caller converts a screen-space pixel radius to
 * local-space using the current camera distance, same idea as any
 * other screen-space-constant-size UI affordance).
 *
 * Priority when multiple candidates qualify: ENDPOINT > MIDPOINT >
 * AXIS_* / PARALLEL / PERPENDICULAR (from-relative) > ON_EDGE > ON_FACE >
 * ON_GROUND. This ordering is SketchUp's own and is load-bearing for the
 * tool to feel right -- e.g. a midpoint sitting exactly on an edge must
 * win over plain on-edge, and an endpoint must win over an axis line
 * merely passing near it. Don't reorder without a reason.
 *
 * Returns false (out_hit untouched) only if the ray hits nothing at all --
 * not even the ON_GROUND fallback, which only fails if the ray is itself
 * (near-)parallel to the ground plane (e.g. looking exactly along the
 * horizon) with nothing else in the mesh to fall back to first. Preferring
 * some fallback over "no inference" wherever the geometry allows it is
 * deliberate, matching SketchUp always offering *something* to snap to. */
bool apcad_mesh_infer(
    apcad_mesh mesh,
    apcad_ray ray,
    const ApriVec3 *from,
    float snap_radius,
    apcad_inference_hit *out_hit);

#ifdef __cplusplus
}
#endif

#endif // APCAD_INFERENCE_H
