
#ifndef APCAD_TOOLS_H
#define APCAD_TOOLS_H

#include "aprimath.h"
#include "cad/apcadmesh.h"
#include "render/aprendscene.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Interactive tools — Move (click-drag) and Rotate (3-click protractor).
 *
 * Deliberately independent of any UI framework: every per-frame input is
 * an abstract signal (a world-space ray, a couple of booleans, an
 * axis-lock enum) rather than anything SDL/ImGui/platform-specific. A host
 * supplies these signals however its own input model produces them --
 * mouse-drag, Apple Pencil, whatever -- and the tool logic itself never
 * changes. The host still owns all actual input handling (reading events,
 * deciding which tool is active, drawing feedback) -- these tools only
 * receive intent and apply the resulting transform to `node`.
 *
 * axis_lock convention (shared by both tools): 0 = free, 1 = world X,
 * 2 = world Y, 3 = world Z.
 */

typedef struct apcad_move_tool_t *apcad_move_tool;

typedef struct {
	bool item_active;        /* the primary action (e.g. left mouse button) is held this frame */
	bool have_ray;            /* whether ray_origin/ray_dir are valid this frame */
	ApriVec3 ray_origin;      /* world space */
	ApriVec3 ray_dir;         /* world space, normalized */
	int axis_lock;
	ApriVec3 camera_forward;  /* free-drag (axis_lock == 0) plane normal */
} apcad_tool_input;

apcad_move_tool apcad_move_tool_create(void);
void apcad_move_tool_destroy(apcad_move_tool tool);

/* Call once per frame while Move is the active tool. `mesh` is only used
 * to hit-test a drag START (so a drag can't begin off-object, via
 * apcad_mesh_raycast); once dragging, only ray math is used. Applies the
 * new translation directly to `node`. Returns true if the node moved this
 * frame. */
bool apcad_move_tool_update(
    apcad_move_tool tool,
    apcad_mesh mesh,
    aprend_node node,
    const apcad_tool_input *input);

bool apcad_move_tool_is_dragging(apcad_move_tool tool);

/* Clears drag state. Call when deactivating Move (switching tools) so a
 * later re-activation doesn't resume a stale drag. Simply not calling
 * apcad_move_tool_update for a frame (e.g. to pause during some other
 * gesture) is enough to freeze a drag in place -- that's not a reset. */
void apcad_move_tool_reset(apcad_move_tool tool);


typedef struct apcad_rotate_tool_t *apcad_rotate_tool;

typedef enum {
	APCAD_ROTATE_STAGE_IDLE = 0,
	APCAD_ROTATE_STAGE_CENTER_SET = 1,
	APCAD_ROTATE_STAGE_REFERENCE_SET = 2,
} APCAD_ROTATE_STAGE;

typedef struct apcad_rotate_input {
	bool item_active;         /* the primary action is held this frame -- edges (clicks) are detected internally */
	bool cancel_requested;    /* one-shot: true on exactly the frame a cancel (e.g. right-click/Esc) happens */
	/* True while the tool should track item_active for edge-detection
	 * continuity only, without acting on it or advancing its stage this
	 * frame -- e.g. while the host has temporarily repurposed the same
	 * input for something else (a camera pan, say). Without this, a click
	 * that was already held down through the suspension would read as a
	 * fresh click the instant suspension ends. */
	bool suspended;
	bool have_ray;
	ApriVec3 ray_origin;
	ApriVec3 ray_dir;
	int axis_lock;
	ApriVec3 camera_forward;  /* free rotation axis (axis_lock == 0), frozen at click 1 */
} apcad_rotate_input;

apcad_rotate_tool apcad_rotate_tool_create(void);
void apcad_rotate_tool_destroy(apcad_rotate_tool tool);

/* Call once per frame while Rotate is the active tool. Drives the
 * Idle -> CenterSet -> ReferenceSet -> Idle (commit) state machine and
 * applies rotation/translation directly to `node`. Returns true if node's
 * transform changed this frame. */
bool apcad_rotate_tool_update(
    apcad_rotate_tool tool,
    apcad_mesh mesh,
    aprend_node node,
    const apcad_rotate_input *input);

APCAD_ROTATE_STAGE apcad_rotate_tool_get_stage(apcad_rotate_tool tool);

/* Clears the operation back to Idle without restoring node's transform.
 * Call when deactivating Rotate (switching tools) -- for canceling an
 * in-progress operation back to its start transform, use
 * apcad_rotate_input::cancel_requested instead. */
void apcad_rotate_tool_reset(apcad_rotate_tool tool);

#ifdef __cplusplus
}
#endif

#endif // APCAD_TOOLS_H
