
#ifndef APREND_ORBIT_CAMERA_H
#define APREND_ORBIT_CAMERA_H

#include "aprimath.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Orbit camera — a SketchUp/Blender-style rig: yaw/pitch/distance around a
 * pannable target, driven by raw accumulated drag/scroll deltas rather than
 * anything SDL/ImGui/platform-specific. A host reads its own input however
 * it likes, decides which gesture (orbit vs. pan vs. zoom) each drag belongs
 * to -- that routing is host UX policy, not something this object should
 * hardcode -- and hands over the resulting per-frame pixel/tick deltas.
 *
 * A separate type from aprend_camera by design: aprend_camera is a bare
 * projectable transform (perspective or orthographic, position + rotation),
 * with no notion of how that transform came to be. This is one specific way
 * of *driving* one (spherical orbit + pan target) -- other control schemes
 * (fly-through, a fixed plan/elevation view) would be their own sibling
 * types rather than modes bolted onto this one, the same way Move/Rotate/
 * Line/Push-Pull (apcadtools.h/apcaddraw.h) are separate tool types rather
 * than one tool with a mode enum.
 *
 * Not thread-safe: create/update/get_* must all happen on the same thread
 * (a host with a separate render thread should own and drive this only from
 * there -- see aprend_orbit_camera_input's own comment for why the host's
 * other threads never need to touch it directly).
 */

typedef struct aprend_orbit_camera_t *aprend_orbit_camera;

typedef struct aprend_orbit_camera_desc {
	float yaw;          /* radians, around world Y, measured from +Z toward +X */
	float pitch;        /* radians, elevation above the XZ plane */
	float distance;     /* units from the orbit target */
	ApriDVec3 target;   /* orbit pivot */

	/* Feel/tuning -- host-owned policy, not a fixed constant baked into
	 * this object, so different hosts (or a future settings panel) can
	 * tune it without touching the update math itself. */
	float orbit_sensitivity; /* radians per pixel of orbit_dx/dy */
	float pan_sensitivity;   /* world units per pixel per unit of distance */
	float zoom_step;         /* multiplicative distance change per zoom_ticks of 1.0 */
	float pitch_limit;       /* radians; clamps pitch to +/- this, short of the poles */
	float min_distance;
	float max_distance;
} aprend_orbit_camera_desc;

aprend_orbit_camera aprend_orbit_camera_create(const aprend_orbit_camera_desc *desc);
void aprend_orbit_camera_destroy(aprend_orbit_camera cam);

/* Raw accumulated pixel/tick deltas since the last update call -- already
 * routed by the host (e.g. middle-drag vs. a modifier+left-drag is an
 * Erethal UX choice, not this object's concern), but otherwise unscaled:
 * this object applies orbit_sensitivity/pan_sensitivity/zoom_step itself,
 * so the host never multiplies by them and never clamps pitch/distance
 * itself either. A field left at 0 simply means "nothing happened" this
 * frame for that gesture -- orbit and pan can never be simultaneously
 * nonzero from a single host that gates them on mutually exclusive input
 * (e.g. Erethal's middle-drag vs. H+left-drag), but nothing here requires
 * that; both are applied independently if both are nonzero. */
typedef struct aprend_orbit_camera_input {
	float orbit_dx, orbit_dy;
	float pan_dx, pan_dy;
	float zoom_ticks;
} aprend_orbit_camera_input;

/* Applies this frame's input to the camera's internal yaw/pitch/distance/
 * target and recomputes its eye position. Call once per frame from
 * whichever thread owns this camera (see the type's own comment). */
void aprend_orbit_camera_update(aprend_orbit_camera cam, const aprend_orbit_camera_input *input);

ApriDVec3 aprend_orbit_camera_get_eye_world(aprend_orbit_camera cam);
ApriDVec3 aprend_orbit_camera_get_target(aprend_orbit_camera cam);
/* normalize(target - eye) -- the free (no axis lock) drag-plane/rotation
 * axis Move/Rotate use, and the tool-agnostic notion of "which way is the
 * camera facing" generally. */
ApriVec3 aprend_orbit_camera_get_forward(aprend_orbit_camera cam);
float aprend_orbit_camera_get_distance(aprend_orbit_camera cam);

#ifdef __cplusplus
}
#endif

#endif // APREND_ORBIT_CAMERA_H
