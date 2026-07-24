#include "cad/apcadtools.h"
#include "apcad_internal.hpp"
#include "cad/apcadraycast.h"

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// --------------------------
// Shared vector-math helpers (no host/UI dependency)

// Ray-vs-plane intersection. Returns false if the ray is (near) parallel to
// the plane.
static bool ray_plane_intersect(
    glm::vec3 ray_origin,
    glm::vec3 ray_dir,
    glm::vec3 plane_point,
    glm::vec3 plane_normal,
    glm::vec3 &out_point) {
	float denom = glm::dot(ray_dir, plane_normal);
	if (fabsf(denom) < 1e-6f)
		return false;
	float t   = glm::dot(plane_point - ray_origin, plane_normal) / denom;
	out_point = ray_origin + ray_dir * t;
	return true;
}

// Closest point ON THE LINE (line_point + t*line_dir) to the given ray --
// i.e. where the cursor ray comes nearest to a world-space axis line. Used
// for axis-locked dragging: unlike ray-vs-plane, this stays well-behaved
// from any viewing angle, including nearly edge-on to what would otherwise
// be the constraint plane.
static glm::vec3 ray_line_closest_point(
    glm::vec3 ray_origin,
    glm::vec3 ray_dir,
    glm::vec3 line_point,
    glm::vec3 line_dir) {
	glm::vec3 r = ray_origin - line_point;
	float a     = glm::dot(ray_dir, ray_dir);
	float b     = glm::dot(ray_dir, line_dir);
	float c     = glm::dot(line_dir, line_dir);
	float d     = glm::dot(ray_dir, r);
	float e     = glm::dot(line_dir, r);
	float denom = a * c - b * b;
	float t     = (fabsf(denom) < 1e-6f) ? 0.0f : (a * e - b * d) / denom;
	return line_point + line_dir * t;
}

static glm::vec3 axis_dir_for_lock(
    int axis_lock,
    glm::vec3 free_axis) {
	if (axis_lock == 1)
		return glm::vec3(1.0f, 0.0f, 0.0f);
	if (axis_lock == 2)
		return glm::vec3(0.0f, 1.0f, 0.0f);
	if (axis_lock == 3)
		return glm::vec3(0.0f, 0.0f, 1.0f);
	return free_axis;
}

// --------------------------
// Move tool

// Move tool: click-drag the object around. Free drag moves it on a plane
// facing the camera; an axis lock constrains it to a single world axis
// instead. Whichever mode is active, `offset` is the constant vector from
// the projected cursor point to the object's position, (re-)established
// once whenever a drag starts or the axis lock changes, so neither ever
// causes a jump.
typedef struct apcad_move_tool_t {
	bool dragging      = false;
	int last_axis_lock = -1; // -1 = "not yet established" (forces reference setup)
	glm::vec3 plane_point{0.0f};
	glm::vec3 plane_normal{0.0f};
	glm::vec3 line_point{0.0f};
	glm::vec3 offset{0.0f};
} apcad_move_tool_t;

extern "C" {

apcad_move_tool apcad_move_tool_create(void) {
	apcad_move_tool_t *result = (apcad_move_tool_t *)malloc(sizeof(apcad_move_tool_t));
	if (result)
		result = new (result) apcad_move_tool_t();
	return result;
}

void apcad_move_tool_destroy(apcad_move_tool tool) {
	if (tool) {
		tool->~apcad_move_tool_t();
		free(tool);
	}
}

bool apcad_move_tool_is_dragging(apcad_move_tool tool) {
	return tool ? tool->dragging : false;
	// return tool && tool->dragging;
}

void apcad_move_tool_reset(apcad_move_tool tool) {
	if (!tool)
		return;
	tool->dragging       = false;
	tool->last_axis_lock = -1;
}

bool apcad_move_tool_update(
    apcad_move_tool tool,
    apcad_mesh mesh,
    aprend_node node,
    const apcad_tool_input *input) {
	if (!tool || !input)
		return false;

	// Start a drag: the primary action becoming active while the cursor is
	// over the object.
	if (!tool->dragging && input->item_active && input->have_ray && mesh) {
		ApriMat4 node_world_m = aprend_node_get_world_transform(node);
		apcad_ray world_ray{input->ray_origin, input->ray_dir};
		apcad_ray local_ray = apcad_ray_to_local(world_ray, node_world_m);
		apcad_raycast_hit hit{};
		if (apcad_mesh_raycast(mesh, local_ray, &hit)) {
			tool->dragging       = true;
			tool->last_axis_lock = -1; // force reference setup below, this call
		}
	}

	if (!input->item_active)
		tool->dragging = false;

	if (!tool->dragging || !input->have_ray)
		return false;

	glm::vec3 ray_origin = to_glm(input->ray_origin);
	glm::vec3 ray_dir    = to_glm(input->ray_dir);

	ApriDVec3 node_pos_d = aprend_node_get_world_translation(node);
	glm::vec3 node_pos   = glm::vec3(to_glm(node_pos_d));

	int axis_lock      = input->axis_lock;
	glm::vec3 axis_dir = axis_dir_for_lock(axis_lock, glm::vec3(0.0f));

	if (axis_lock != tool->last_axis_lock) {
		// (Re-)establish the reference for the new mode from the object's
		// CURRENT position, so switching modes mid-drag never jumps.
		if (axis_lock == 0) {
			glm::vec3 forward = to_glm(input->camera_forward);

			tool->plane_point  = node_pos;
			tool->plane_normal = forward;

			glm::vec3 hit;
			tool->offset = ray_plane_intersect(ray_origin, ray_dir, node_pos, forward, hit) ? (node_pos - hit) : glm::vec3(0.0f);
		} else {
			tool->line_point  = node_pos;
			glm::vec3 closest = ray_line_closest_point(ray_origin, ray_dir, node_pos, axis_dir);
			tool->offset      = node_pos - closest;
		}
		tool->last_axis_lock = axis_lock;
	}

	glm::vec3 new_pos;
	if (axis_lock == 0) {
		glm::vec3 hit;
		if (!ray_plane_intersect(ray_origin, ray_dir, tool->plane_point, tool->plane_normal, hit))
			return false; // ray (near-)parallel to the plane this frame -- leave the object where it is
		new_pos = hit + tool->offset;
	} else {
		glm::vec3 closest = ray_line_closest_point(ray_origin, ray_dir, tool->line_point, axis_dir);
		new_pos           = closest + tool->offset;
	}

	aprend_node_set_translation(node, from_glm(glm::dvec3(new_pos)));
	return true;
}

} // extern "C"

// --------------------------
// Rotate tool

// Rotate tool: a true 3-click SketchUp-style protractor rather than a
// click-drag like Move. Click 1 places the pivot and freezes the rotation
// axis; click 2 sets the zero-degree reference direction; the object then
// live-rotates (and orbits around the pivot) as the cursor sweeps until
// click 3 commits. A cancel request restores the transform captured at
// click 1.
typedef struct apcad_rotate_tool_t {
	APCAD_ROTATE_STAGE stage = APCAD_ROTATE_STAGE_IDLE;
	bool prev_item_active    = false; // for edge-detecting each of the 3 clicks
	glm::vec3 center{0.0f};
	glm::vec3 axis{0.0f, 1.0f, 0.0f};
	glm::vec3 reference_dir{1.0f, 0.0f, 0.0f};
	glm::quat start_rotation{1.0f, 0.0f, 0.0f, 0.0f};
	glm::dvec3 start_translation{0.0};
} apcad_rotate_tool_t;

extern "C" {

apcad_rotate_tool apcad_rotate_tool_create(void) {
	apcad_rotate_tool_t *result = (apcad_rotate_tool_t *)malloc(sizeof(apcad_rotate_tool_t));
	if (result)
		result = new (result) apcad_rotate_tool_t();
	return result;
}

void apcad_rotate_tool_destroy(apcad_rotate_tool tool) {
	if (tool) {
		tool->~apcad_rotate_tool_t();
		free(tool);
	}
}

APCAD_ROTATE_STAGE apcad_rotate_tool_get_stage(apcad_rotate_tool tool) { return tool ? tool->stage : APCAD_ROTATE_STAGE_IDLE; }

void apcad_rotate_tool_reset(apcad_rotate_tool tool) {
	if (!tool)
		return;
	tool->stage            = APCAD_ROTATE_STAGE_IDLE;
	tool->prev_item_active = false;
}

bool apcad_rotate_tool_update(
    apcad_rotate_tool tool,
    apcad_mesh mesh,
    aprend_node node,
    const apcad_rotate_input *input) {
	if (!tool || !input)
		return false;

	// Edge-detect off item_active's rising edge -- tracked even while
	// suspended so a click held through a suspension doesn't read as a
	// fresh click the instant suspension ends (see apcad_rotate_input's
	// doc comment).
	bool click_edge        = input->item_active && !tool->prev_item_active;
	tool->prev_item_active = input->item_active;

	if (input->suspended)
		return false;

	if (input->cancel_requested && tool->stage != APCAD_ROTATE_STAGE_IDLE) {
		aprend_node_set_rotation(node, from_glm(tool->start_rotation));
		aprend_node_set_translation(node, from_glm(tool->start_translation));
		tool->stage = APCAD_ROTATE_STAGE_IDLE;
	}

	if (!input->have_ray)
		return false;

	glm::vec3 ray_origin = to_glm(input->ray_origin);
	glm::vec3 ray_dir    = to_glm(input->ray_dir);

	if (tool->stage == APCAD_ROTATE_STAGE_IDLE) {
		// Click 1 must land on the object -- there's no ground-plane/grid
		// inference to fall back on, unlike real SketchUp.
		if (!click_edge || !mesh)
			return false;

		ApriMat4 node_world_m = aprend_node_get_world_transform(node);
		apcad_ray world_ray{input->ray_origin, input->ray_dir};
		apcad_ray local_ray = apcad_ray_to_local(world_ray, node_world_m);
		apcad_raycast_hit hit{};
		if (!apcad_mesh_raycast(mesh, local_ray, &hit))
			return false;

		ApriVec3 world_hit = apcad_local_point_to_world(hit.position, node_world_m);
		tool->center       = to_glm(world_hit);
		// Frozen for the whole operation, not re-evaluated if the host's
		// camera orbits mid-rotate (axis_lock == 0 case: input->camera_forward
		// is whatever the host reports at this instant).
		tool->axis = axis_dir_for_lock(input->axis_lock, to_glm(input->camera_forward));

		tool->start_rotation    = to_glm(aprend_node_get_rotation(node));
		tool->start_translation = to_glm(aprend_node_get_world_translation(node));

		tool->stage = APCAD_ROTATE_STAGE_CENTER_SET;
		return false;
	}

	glm::vec3 plane_hit;
	if (!ray_plane_intersect(ray_origin, ray_dir, tool->center, tool->axis, plane_hit))
		return false; // ray (near-)parallel to the rotation plane this frame

	glm::vec3 dir_vec = plane_hit - tool->center;
	if (glm::length(dir_vec) < 1e-5f)
		return false; // cursor projects too close to the pivot to define a direction
	glm::vec3 current_dir = glm::normalize(dir_vec);

	if (tool->stage == APCAD_ROTATE_STAGE_CENTER_SET) {
		// Click 2: lock in the zero-degree reference direction. Moving the
		// cursor before this click doesn't rotate anything yet, matching
		// SketchUp's protractor (only orienting the not-yet-placed
		// reference line).
		if (!click_edge)
			return false;
		tool->reference_dir = current_dir;
		tool->stage         = APCAD_ROTATE_STAGE_REFERENCE_SET;
		return false;
	}

	// REFERENCE_SET: live-sweep the angle between the reference direction
	// and the current cursor direction every frame. Click 3 (commit) is a
	// no-op beyond stopping -- this call's applied rotation is already the
	// final result.
	float angle            = atan2f(glm::dot(glm::cross(tool->reference_dir, current_dir), tool->axis), glm::dot(tool->reference_dir, current_dir));
	glm::quat delta        = glm::angleAxis(angle, tool->axis);
	glm::quat new_rotation = glm::normalize(delta * tool->start_rotation);

	// Orbits the object's position around the pivot too (not just spinning
	// it in place), since tool->center can be off the object's own origin
	// (it's the click-1 raycast hit point, e.g. a corner).
	glm::vec3 start_offset = glm::vec3(tool->start_translation - glm::dvec3(tool->center));
	glm::vec3 new_pos      = tool->center + delta * start_offset;

	aprend_node_set_rotation(node, from_glm(new_rotation));
	aprend_node_set_translation(node, from_glm(glm::dvec3(new_pos)));

	if (click_edge)
		tool->stage = APCAD_ROTATE_STAGE_IDLE;

	return true;
}

} // extern "C"
