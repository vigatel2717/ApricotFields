#include "cad/apcaddraw.h"
#include "apcad_internal.hpp"
#include "cad/apcadraycast.h"

#include <cmath>
#include <cstdlib>
#include <new>
#include <vector>

namespace {

apcad_inference_hit no_hit() {
	// See apcadinference.cpp's own no_hit() -- APCAD_INVALID_ID isn't 0,
	// so this can't be a plain zero-initialization.
	apcad_inference_hit h{};
	h.type   = APCAD_INFERENCE_NONE;
	h.vertex = APCAD_INVALID_ID;
	h.edge   = APCAD_INVALID_ID;
	h.face   = APCAD_INVALID_ID;
	return h;
}

/* Hard axis constraint for the Line tool's axis_lock, once a start point
 * exists: closest approach between the ray and the infinite line through
 * `from` along one of the local X/Y/Z axes. Always succeeds (a lock is a
 * forced constraint, not a proximity-gated inference) -- duplicated from
 * the equivalent skew-line math in apcadinference.cpp rather than shared,
 * same tradeoff apcadtools.cpp already accepts for its own private
 * ray_plane_intersect/ray_line_closest_point helpers. */
apcad_inference_hit axis_locked_hit(
    ApriVec3 from,
    glm::vec3 ray_origin,
    glm::vec3 ray_dir,
    int axis_lock) {
	glm::vec3 f   = to_glm(from);
	glm::vec3 dir = axis_lock == 1 ? glm::vec3(1, 0, 0) : axis_lock == 2 ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1);

	glm::vec3 w0 = ray_origin - f;
	float b      = glm::dot(ray_dir, dir);
	float denom  = 1.0f - b * b;
	float t;
	if (std::fabs(denom) < 1e-6f) {
		t = glm::dot(ray_origin - f, dir);
	} else {
		float s = 0.0f;
		float d = glm::dot(ray_dir, w0);
		float e = glm::dot(dir, w0);
		s       = (b * e - d) / denom;
		if (s < 0.0f)
			s = 0.0f;
		(void)s;
		t = (e - b * d) / denom;
	}
	glm::vec3 point_on_axis = f + dir * t;

	apcad_inference_hit hit = no_hit();
	hit.type                = axis_lock == 1 ? APCAD_INFERENCE_AXIS_X : axis_lock == 2 ? APCAD_INFERENCE_AXIS_Y : APCAD_INFERENCE_AXIS_Z;
	hit.position            = from_glm(point_on_axis);
	return hit;
}

} // namespace

// --------------------------
// Line tool

typedef struct apcad_line_tool_t {
	bool has_start        = false;
	bool prev_item_active = false;
	ApriVec3 start_position{};
} apcad_line_tool_t;

extern "C" {

apcad_line_tool apcad_line_tool_create(void) {
	apcad_line_tool_t *result = (apcad_line_tool_t *)malloc(sizeof(apcad_line_tool_t));
	if (result)
		result = new (result) apcad_line_tool_t();
	return result;
}

void apcad_line_tool_destroy(apcad_line_tool tool) {
	if (tool) {
		tool->~apcad_line_tool_t();
		free(tool);
	}
}

void apcad_line_tool_reset(apcad_line_tool tool) {
	if (!tool)
		return;
	tool->has_start        = false;
	tool->prev_item_active = false;
}

bool apcad_line_tool_update(
    apcad_line_tool tool,
    apcad_mesh mesh,
    const apcad_line_input *input,
    apcad_inference_hit *out_preview_hit) {
	if (!tool || !mesh || !input)
		return false;

	// Edge-detect off item_active's rising edge, same convention Rotate
	// uses (apcadtools.cpp) -- clicks are one-shot events, holding the
	// button is not.
	bool click_edge        = input->item_active && !tool->prev_item_active;
	tool->prev_item_active = input->item_active;

	if (input->cancel_requested) {
		tool->has_start = false;
		if (out_preview_hit)
			*out_preview_hit = no_hit();
		return false;
	}

	if (!input->have_ray) {
		if (out_preview_hit)
			*out_preview_hit = no_hit();
		return false;
	}

	glm::vec3 ray_origin_raw = to_glm(input->ray_origin);
	glm::vec3 ray_dir_raw    = to_glm(input->ray_dir);
	float ray_len            = glm::length(ray_dir_raw);
	if (ray_len < 1e-12f) {
		if (out_preview_hit)
			*out_preview_hit = no_hit();
		return false;
	}
	glm::vec3 ray_origin = ray_origin_raw;
	glm::vec3 ray_dir    = ray_dir_raw / ray_len;

	const ApriVec3 *from = tool->has_start ? &tool->start_position : nullptr;

	apcad_inference_hit hit;
	bool have_hit;
	if (from && input->axis_lock != 0) {
		// A lock is a hard constraint -- bypass apcad_mesh_infer's
		// proximity-gated candidates entirely rather than merely
		// preferring the locked axis among them.
		hit      = axis_locked_hit(*from, ray_origin, ray_dir, input->axis_lock);
		have_hit = true;
	} else {
		apcad_ray ray{input->ray_origin, input->ray_dir};
		have_hit = apcad_mesh_infer(mesh, ray, from, input->snap_radius, &hit);
	}

	if (out_preview_hit)
		*out_preview_hit = have_hit ? hit : no_hit();

	if (!click_edge || !have_hit)
		return false;

	if (!tool->has_start) {
		// Click 1: record the (already-snapped) start position, but don't
		// weld a vertex into the mesh yet -- if the chain gets cancelled
		// before a segment ever commits, this way there's no orphaned
		// vertex left behind to clean up (apcad_mesh has no "erase
		// vertex" call for exactly that reason: welding is meant to be
		// paired with an edge that actually uses it).
		tool->start_position = hit.position;
		tool->has_start      = true;
		return false;
	}

	// Click 2+: commit the segment, then immediately re-arm from the same
	// endpoint so repeated clicks chain into a polyline for free.
	float weld_eps          = input->snap_radius;
	apcad_vertex_id v_start = apcad_mesh_weld_vertex(mesh, tool->start_position, weld_eps);
	apcad_vertex_id v_end   = apcad_mesh_weld_vertex(mesh, hit.position, weld_eps);

	tool->start_position = hit.position;

	if (v_start == v_end)
		return false; // clicked (within weld_eps of) the same point twice -- no degenerate zero-length edge

	apcad_edge_id new_edge = apcad_mesh_add_edge(mesh, v_start, v_end);
	return new_edge != APCAD_INVALID_ID;
}

} // extern "C"

// --------------------------
// Push/Pull tool

// Offsets a face's own vertex loop along its normal and wires it back to
// the original boundary with new side edges -- turning a flat face into a
// volume. Scope note, not an oversight: this always builds a NEW slab, even
// if the clicked face is itself an already-extruded cap from an earlier
// Push/Pull. apcad_mesh carries no "this face used to be a flat sketch vs.
// this face is a pulled cap" metadata, so this implementation doesn't
// attempt SketchUp's "pull the same cap again just moves it" special case
// -- repeated pulls on the same face stack additional slabs instead of
// adjusting one. A real fix needs either that metadata or a topological
// test ("is every one of this face's edges shared with a side wall"), left
// as a follow-up.
typedef struct apcad_pushpull_tool_t {
	bool dragging         = false;
	bool geometry_created = false;
	apcad_face_id face    = APCAD_INVALID_ID;
	glm::vec3 normal{0.0f, 1.0f, 0.0f};
	glm::vec3 axis_point{0.0f};
	std::vector<apcad_vertex_id> base_verts;
	std::vector<glm::vec3> base_positions; // positions at drag start, so repositioning is always relative to the start, not accumulated per-frame drift
	std::vector<apcad_vertex_id> cap_verts;
} apcad_pushpull_tool_t;

extern "C" {

apcad_pushpull_tool apcad_pushpull_tool_create(void) {
	apcad_pushpull_tool_t *result = (apcad_pushpull_tool_t *)malloc(sizeof(apcad_pushpull_tool_t));
	if (result)
		result = new (result) apcad_pushpull_tool_t();
	return result;
}

void apcad_pushpull_tool_destroy(apcad_pushpull_tool tool) {
	if (tool) {
		tool->~apcad_pushpull_tool_t();
		free(tool);
	}
}

bool apcad_pushpull_tool_is_dragging(apcad_pushpull_tool tool) { return tool ? tool->dragging : false; }

void apcad_pushpull_tool_reset(apcad_pushpull_tool tool) {
	if (!tool)
		return;
	tool->dragging         = false;
	tool->geometry_created = false;
	tool->face             = APCAD_INVALID_ID;
	tool->base_verts.clear();
	tool->base_positions.clear();
	tool->cap_verts.clear();
}

bool apcad_pushpull_tool_update(
    apcad_pushpull_tool tool,
    apcad_mesh mesh,
    const apcad_pushpull_input *input) {
	if (!tool || !mesh || !input)
		return false;

	// Start a drag: the primary action becoming active while the cursor is
	// over a face with at least a triangle's worth of edges.
	if (!tool->dragging && input->item_active && input->have_ray) {
		apcad_ray ray{input->ray_origin, input->ray_dir};
		apcad_raycast_hit hit{};
		if (apcad_mesh_raycast(mesh, ray, &hit)) {
			uint32_t count = apcad_mesh_face_edge_count(mesh, hit.face_index);
			if (count >= 3) {
				tool->dragging         = true;
				tool->geometry_created = false;
				tool->face             = hit.face_index;
				tool->normal           = to_glm(apcad_mesh_face_normal(mesh, tool->face));
				tool->axis_point       = to_glm(hit.position);
				tool->base_verts.clear();
				tool->base_positions.clear();
				for (uint32_t i = 0; i < count; ++i) {
					apcad_vertex_id v = apcad_mesh_face_vertex(mesh, tool->face, i);
					tool->base_verts.push_back(v);
					tool->base_positions.push_back(to_glm(apcad_mesh_vertex_position(mesh, v)));
				}
				tool->cap_verts.clear();
			}
		}
	}

	if (!input->item_active) {
		tool->dragging = false;
		return false;
	}

	if (!tool->dragging || !input->have_ray)
		return false;

	glm::vec3 ray_origin_raw = to_glm(input->ray_origin);
	glm::vec3 ray_dir_raw    = to_glm(input->ray_dir);
	float ray_len            = glm::length(ray_dir_raw);
	if (ray_len < 1e-12f)
		return false;
	glm::vec3 ray_origin = ray_origin_raw;
	glm::vec3 ray_dir    = ray_dir_raw / ray_len;

	// Same skew-line closest-approach reduction apcad_move_tool_update
	// uses for its axis-locked case (apcadtools.cpp), with the face's own
	// normal standing in for the locked axis -- t is the signed distance
	// travelled along that normal from the drag's starting point.
	glm::vec3 w0 = ray_origin - tool->axis_point;
	float b      = glm::dot(ray_dir, tool->normal);
	float denom  = 1.0f - b * b;
	if (std::fabs(denom) < 1e-6f)
		return false; // ray (near-)parallel to the drag axis this frame -- leave geometry where it is
	float d_ray_dot = glm::dot(ray_dir, w0);
	float e_line    = glm::dot(tool->normal, w0);
	float t         = (e_line - b * d_ray_dot) / denom;

	if (!tool->geometry_created) {
		const float k_creation_threshold = 1e-4f;
		if (std::fabs(t) < k_creation_threshold)
			return false; // hasn't dragged far enough yet to commit to an extrusion

		uint32_t count = (uint32_t)tool->base_verts.size();
		tool->cap_verts.resize(count);
		for (uint32_t i = 0; i < count; ++i) {
			ApriVec3 p         = from_glm(tool->base_positions[i] + tool->normal * t);
			tool->cap_verts[i] = apcad_mesh_weld_vertex(mesh, p, 1e-5f);
		}
		// New cap loop edges plus the side edges connecting each original
		// vertex to its offset counterpart. Each side quad (base[i],
		// base[i+1], cap[i+1], cap[i]) is planar by construction (a
		// straight sweep along a single normal), so apcad_mesh_add_edge's
		// own auto-face detection finds the cap face and every side face
		// on its own once all of their edges exist -- no manual face
		// construction needed here.
		for (uint32_t i = 0; i < count; ++i) {
			apcad_mesh_add_edge(mesh, tool->cap_verts[i], tool->cap_verts[(i + 1) % count]);
			apcad_mesh_add_edge(mesh, tool->base_verts[i], tool->cap_verts[i]);
		}

		// The original clicked face is now enclosed by the new cap + side
		// walls -- the volume between base and cap is theirs to bound, not
		// its. Left alive, it stays a real face sitting inside the new
		// solid (its own auto-face detection never touches it: the new
		// side edges each have one endpoint -- a cap vertex -- that was
		// never part of this face's loop, so apcad_mesh_add_edge's own
		// "erase a face when both new endpoints already lie on it" check
		// never fires for it). Erasing it explicitly is the fix; its edges
		// stay untouched, still bounding the new side walls on their other
		// side.
		apcad_mesh_erase_face(mesh, tool->face);

		tool->geometry_created = true;
		return true;
	}

	// Geometry already exists for this drag -- just reposition the cap.
	for (size_t i = 0; i < tool->cap_verts.size(); ++i) {
		ApriVec3 p = from_glm(tool->base_positions[i] + tool->normal * t);
		apcad_mesh_move_vertex(mesh, tool->cap_verts[i], p);
	}
	return true;
}

} // extern "C"
