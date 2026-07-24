
#ifndef APCAD_DRAW_H
#define APCAD_DRAW_H

#include "aprimath.h"
#include "cad/apcadinference.h"
#include "cad/apcadmesh.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Drawing tools — Line and Push/Pull.
 *
 * DRAFT for review — not yet implemented. Deliberately a separate header
 * from apcadtools.h: Move/Rotate there transform an already-placed
 * apcad_solid via its aprend_node, whereas these two author an apcad_mesh's
 * topology directly and have no node/placement concept of their own -- a
 * mesh is worked on in its own local space until whatever host-level
 * "place this in the scene" step wraps it in a node. Same host-agnostic
 * input-signal convention as apcadtools.h otherwise: no SDL/ImGui/platform
 * types, just rays and booleans.
 */

// --------------------------
// Line tool

typedef struct apcad_line_tool_t *apcad_line_tool;

typedef struct {
	bool item_active;      /* primary action held this frame; clicks are edge-detected internally, same as Rotate */
	bool cancel_requested; /* one-shot: Esc/right-click this frame */
	bool have_ray;
	ApriVec3 ray_origin; /* mesh-local space (see apcadinference.h) */
	ApriVec3 ray_dir;
	int axis_lock;      /* 0 = free/inferred, 1 = local X, 2 = local Y, 3 = local Z -- same convention as apcadtools.h */
	float snap_radius;  /* local-space inference radius passed straight through to apcad_mesh_infer -- see its own doc comment for how to derive this from a screen-space pixel radius. Also reused as the weld epsilon when a segment commits, since both are "how close counts as the same point" at the same scale. */
} apcad_line_input;

apcad_line_tool apcad_line_tool_create(void);
void apcad_line_tool_destroy(apcad_line_tool tool);

/* Call once per frame while Line is active. Click 1 places the start point
 * (snapped via apcad_mesh_infer); the tool then previews an edge to the
 * current cursor position (also snapped, using click-1's point as `from`
 * so axis/parallel/perpendicular inference is available) until click 2
 * commits it via apcad_mesh_add_edge -- and immediately re-arms from that
 * same endpoint, so a host that just keeps calling this across repeated
 * clicks gets a chained polyline for free, matching SketchUp's Line tool.
 * `cancel_requested` ends the chain without committing the in-progress
 * segment; it does not undo already-committed ones.
 *
 * `out_preview_hit`, if non-NULL, always receives the current frame's best
 * inference result (even before click 1, and every frame while previewing)
 * so the host can draw the inference cue -- this is the tool's only path
 * for that feedback; it has no other query surface. Returns true the frame
 * a segment is actually committed to `mesh` (i.e. mesh topology changed),
 * not merely on every frame the preview updates. */
bool apcad_line_tool_update(
    apcad_line_tool tool,
    apcad_mesh mesh,
    const apcad_line_input *input,
    apcad_inference_hit *out_preview_hit);

/* Clears state back to "no start point placed". Call when deactivating
 * Line (switching tools); mirrors apcad_move_tool_reset/
 * apcad_rotate_tool_reset. */
void apcad_line_tool_reset(apcad_line_tool tool);

// --------------------------
// Push/Pull tool

typedef struct apcad_pushpull_tool_t *apcad_pushpull_tool;

typedef struct {
	bool item_active;
	bool have_ray;
	ApriVec3 ray_origin; /* mesh-local space */
	ApriVec3 ray_dir;
} apcad_pushpull_input;

apcad_pushpull_tool apcad_pushpull_tool_create(void);
void apcad_pushpull_tool_destroy(apcad_pushpull_tool tool);

/* Call once per frame while Push/Pull is active. A drag can only START by
 * landing on an existing planar face (raycast against `mesh`); once
 * dragging, motion is reduced to the same ray-vs-line closest-point used by
 * apcad_move_tool_update's axis-locked case, with the face's own normal
 * standing in for the locked axis, so the face only ever moves along its
 * normal.
 *
 * First drag on a given face: offsets that face's loop by the drag
 * distance and generates new side edges/faces connecting it back to the
 * original boundary -- turning a flat face into a volume, or adding to an
 * existing one. Dragging a face that's already an extruded cap (not the
 * original coplanar loop) instead just moves that cap and its side edges,
 * so repeated pushes/pulls on the same face adjust the same volume rather
 * than stacking new ones, matching SketchUp. Dragging inward past the
 * opposite face is intentionally unclamped in this design -- treat a
 * self-intersecting result as a follow-up geometric-kernel concern (real
 * boolean support), not something this tool's job to prevent.
 *
 * Returns true if `mesh` changed this frame. */
bool apcad_pushpull_tool_update(
    apcad_pushpull_tool tool,
    apcad_mesh mesh,
    const apcad_pushpull_input *input);

bool apcad_pushpull_tool_is_dragging(apcad_pushpull_tool tool);

/* Mirrors apcad_move_tool_reset: clears drag state on tool switch. */
void apcad_pushpull_tool_reset(apcad_pushpull_tool tool);

#ifdef __cplusplus
}
#endif

#endif // APCAD_DRAW_H
