#include "cad/apcadinference.h"
#include "apcad_internal.hpp"

#include <cmath>
#include <vector>

/*
 * Implemented entirely through apcadmesh.h's public query API (vertex/edge
 * queries, apcad_mesh_raycast) -- never through apcad_mesh's internal
 * storage, which is private to apcadmesh.cpp's translation unit by design
 * (see that file's header comment). Keeping this file at arm's length from
 * the internals is what lets apcad_mesh's own representation keep changing
 * without this file having to track it.
 */

namespace {

apcad_inference_hit no_hit() {
	// Not just `apcad_inference_hit{}` -- APCAD_INVALID_ID is 0xFFFFFFFF,
	// not 0, so a zero-initialized struct would leave vertex/edge/face
	// looking like id 0 (a *real* id) rather than "none".
	apcad_inference_hit h{};
	h.type   = APCAD_INFERENCE_NONE;
	h.vertex = APCAD_INVALID_ID;
	h.edge   = APCAD_INVALID_ID;
	h.face   = APCAD_INVALID_ID;
	return h;
}

float distance_point_to_ray(
    glm::vec3 point,
    glm::vec3 ray_origin,
    glm::vec3 ray_dir) {
	float s = glm::dot(point - ray_origin, ray_dir);
	if (s < 0.0f)
		s = 0.0f; // a ray, not a full line -- nothing "behind" the cursor is a candidate
	glm::vec3 p_ray = ray_origin + ray_dir * s;
	return glm::length(p_ray - point);
}

/* Closest approach between the ray (s >= 0) and the infinite line
 * (line_point + t*line_dir, line_dir normalized). Returns the point ON THE
 * LINE (what a caller should snap to) and the distance between the two at
 * that closest approach. Falls back to projecting the ray origin onto the
 * line when the two are (near-)parallel, rather than failing -- a
 * degenerate-but-still-answerable case, not an error. */
void ray_vs_line_closest(
    glm::vec3 ray_origin,
    glm::vec3 ray_dir,
    glm::vec3 line_point,
    glm::vec3 line_dir,
    glm::vec3 &out_point_on_line,
    float &out_distance) {
	glm::vec3 w0 = ray_origin - line_point;
	float b      = glm::dot(ray_dir, line_dir);
	float denom  = 1.0f - b * b;
	float s, t;
	if (std::fabs(denom) < 1e-6f) {
		s = 0.0f;
		t = glm::dot(ray_origin - line_point, line_dir);
	} else {
		float d = glm::dot(ray_dir, w0);
		float e = glm::dot(line_dir, w0);
		s       = (b * e - d) / denom;
		t       = (e - b * d) / denom;
	}
	if (s < 0.0f) {
		s = 0.0f;
		t = glm::dot(ray_origin - line_point, line_dir);
	}
	glm::vec3 p_ray   = ray_origin + ray_dir * s;
	glm::vec3 p_line  = line_point + line_dir * t;
	out_point_on_line = p_line;
	out_distance      = glm::length(p_ray - p_line);
}

/* Closest point on the SEGMENT [seg_a, seg_b] (not the infinite line) to
 * the ray. Built on ray_vs_line_closest, then clamped to the segment and
 * re-projected -- an approximation (clamping without jointly
 * re-optimizing the ray parameter against the clamped point) rather than
 * a fully general closest-segment-to-ray solve, which is more machinery
 * than an inference snap radius check needs. */
void closest_segment_to_ray(
    glm::vec3 ray_origin,
    glm::vec3 ray_dir,
    glm::vec3 seg_a,
    glm::vec3 seg_b,
    glm::vec3 &out_point,
    float &out_distance) {
	glm::vec3 full = seg_b - seg_a;
	float seg_len  = glm::length(full);
	if (seg_len < 1e-9f) {
		out_point    = seg_a;
		out_distance = distance_point_to_ray(seg_a, ray_origin, ray_dir);
		return;
	}
	glm::vec3 seg_dir = full / seg_len;
	glm::vec3 p_on_line;
	float unused_dist;
	ray_vs_line_closest(ray_origin, ray_dir, seg_a, seg_dir, p_on_line, unused_dist);

	float t      = glm::dot(p_on_line - seg_a, seg_dir);
	t            = glm::clamp(t, 0.0f, seg_len);
	out_point    = seg_a + seg_dir * t;
	out_distance = distance_point_to_ray(out_point, ray_origin, ray_dir);
}

int inference_tier(APCAD_INFERENCE_TYPE type) {
	switch (type) {
	case APCAD_INFERENCE_ENDPOINT:
		return 0;
	case APCAD_INFERENCE_MIDPOINT:
		return 1;
	case APCAD_INFERENCE_AXIS_X:
	case APCAD_INFERENCE_AXIS_Y:
	case APCAD_INFERENCE_AXIS_Z:
	case APCAD_INFERENCE_PARALLEL:
	case APCAD_INFERENCE_PERPENDICULAR:
		return 2;
	case APCAD_INFERENCE_ON_EDGE:
		return 3;
	case APCAD_INFERENCE_ON_FACE:
		return 4;
	case APCAD_INFERENCE_ON_GROUND:
		return 5;
	default:
		return 6;
	}
}

} // namespace

extern "C" {

bool apcad_mesh_infer(
    apcad_mesh mesh,
    apcad_ray ray,
    const ApriVec3 *from,
    float snap_radius,
    apcad_inference_hit *out_hit) {
	if (!mesh || !out_hit)
		return false;

	glm::vec3 ray_origin_raw = to_glm(ray.origin);
	glm::vec3 ray_dir_raw    = to_glm(ray.direction);
	float ray_len            = glm::length(ray_dir_raw);
	if (ray_len < 1e-12f)
		return false;
	glm::vec3 ray_origin = ray_origin_raw;
	glm::vec3 ray_dir    = ray_dir_raw / ray_len;

	bool have_best           = false;
	apcad_inference_hit best = no_hit();
	float best_score         = 0.0f;

	auto consider = [&](apcad_inference_hit candidate, float score) {
		if (!have_best) {
			best       = candidate;
			best_score = score;
			have_best  = true;
			return;
		}
		int ct = inference_tier(candidate.type);
		int bt = inference_tier(best.type);
		if (ct < bt || (ct == bt && score < best_score)) {
			best       = candidate;
			best_score = score;
		}
	};

	// ENDPOINT -- every alive vertex.
	uint32_t vertex_range = apcad_mesh_vertex_id_range(mesh);
	for (apcad_vertex_id v = 0; v < vertex_range; ++v) {
		if (!apcad_mesh_vertex_exists(mesh, v))
			continue;
		glm::vec3 pos = to_glm(apcad_mesh_vertex_position(mesh, v));
		float dist    = distance_point_to_ray(pos, ray_origin, ray_dir);
		if (dist > snap_radius)
			continue;
		apcad_inference_hit hit = no_hit();
		hit.type                = APCAD_INFERENCE_ENDPOINT;
		hit.position            = from_glm(pos);
		hit.vertex              = v;
		consider(hit, dist);
	}

	// MIDPOINT and ON_EDGE -- every alive edge.
	uint32_t edge_range = apcad_mesh_edge_id_range(mesh);
	for (apcad_edge_id e = 0; e < edge_range; ++e) {
		if (!apcad_mesh_edge_exists(mesh, e))
			continue;
		apcad_vertex_id va, vb;
		apcad_mesh_edge_vertices(mesh, e, &va, &vb);
		glm::vec3 pa = to_glm(apcad_mesh_vertex_position(mesh, va));
		glm::vec3 pb = to_glm(apcad_mesh_vertex_position(mesh, vb));

		glm::vec3 mid  = (pa + pb) * 0.5f;
		float mid_dist = distance_point_to_ray(mid, ray_origin, ray_dir);
		if (mid_dist <= snap_radius) {
			apcad_inference_hit hit = no_hit();
			hit.type                = APCAD_INFERENCE_MIDPOINT;
			hit.position            = from_glm(mid);
			hit.edge                = e;
			consider(hit, mid_dist);
		}

		glm::vec3 seg_point;
		float seg_dist;
		closest_segment_to_ray(ray_origin, ray_dir, pa, pb, seg_point, seg_dist);
		if (seg_dist <= snap_radius) {
			apcad_inference_hit hit = no_hit();
			hit.type                = APCAD_INFERENCE_ON_EDGE;
			hit.position            = from_glm(seg_point);
			hit.edge                = e;
			consider(hit, seg_dist);
		}
	}

	// AXIS_*/PARALLEL/PERPENDICULAR -- only meaningful relative to an
	// already-placed `from` point.
	if (from) {
		glm::vec3 f = to_glm(*from);

		glm::vec3 axes[3]                  = {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)};
		APCAD_INFERENCE_TYPE axis_types[3] = {APCAD_INFERENCE_AXIS_X, APCAD_INFERENCE_AXIS_Y, APCAD_INFERENCE_AXIS_Z};
		for (int i = 0; i < 3; ++i) {
			glm::vec3 point_on_line;
			float dist;
			ray_vs_line_closest(ray_origin, ray_dir, f, axes[i], point_on_line, dist);
			if (dist > snap_radius)
				continue;
			apcad_inference_hit hit = no_hit();
			hit.type                = axis_types[i];
			hit.position            = from_glm(point_on_line);
			consider(hit, dist);
		}

		for (apcad_edge_id e = 0; e < edge_range; ++e) {
			if (!apcad_mesh_edge_exists(mesh, e))
				continue;
			apcad_vertex_id va, vb;
			apcad_mesh_edge_vertices(mesh, e, &va, &vb);
			glm::vec3 pa       = to_glm(apcad_mesh_vertex_position(mesh, va));
			glm::vec3 pb       = to_glm(apcad_mesh_vertex_position(mesh, vb));
			glm::vec3 edge_vec = pb - pa;
			float edge_len     = glm::length(edge_vec);
			if (edge_len < 1e-9f)
				continue;
			glm::vec3 edge_dir = edge_vec / edge_len;

			glm::vec3 point_on_line;
			float dist;
			ray_vs_line_closest(ray_origin, ray_dir, f, edge_dir, point_on_line, dist);
			if (dist <= snap_radius) {
				apcad_inference_hit hit = no_hit();
				hit.type                = APCAD_INFERENCE_PARALLEL;
				hit.position            = from_glm(point_on_line);
				hit.edge                = e;
				consider(hit, dist);
			}

			// Approximation: "perpendicular to this edge" is really a whole
			// PLANE through `from` (normal = edge_dir) in unconstrained 3D,
			// not a single line -- true SketchUp-style perpendicular
			// inference leans on screen-space/camera context this module
			// doesn't have. This substitutes the single line through `from`
			// obtained by projecting the ray's own direction to be
			// perpendicular to edge_dir, which is stable and always
			// computable but is a real simplification, not the exact
			// SketchUp behavior -- flagging rather than hiding that.
			glm::vec3 perp_raw = ray_dir - glm::dot(ray_dir, edge_dir) * edge_dir;
			float perp_len     = glm::length(perp_raw);
			if (perp_len > 1e-6f) {
				glm::vec3 perp_dir = perp_raw / perp_len;
				glm::vec3 perp_point_on_line;
				float perp_dist;
				ray_vs_line_closest(ray_origin, ray_dir, f, perp_dir, perp_point_on_line, perp_dist);
				if (perp_dist <= snap_radius) {
					apcad_inference_hit hit = no_hit();
					hit.type                = APCAD_INFERENCE_PERPENDICULAR;
					hit.position            = from_glm(perp_point_on_line);
					hit.edge                = e;
					consider(hit, perp_dist);
				}
			}
		}
	}

	// ON_FACE -- second-to-last fallback, via the raw mesh raycast.
	apcad_raycast_hit raw_hit{};
	if (apcad_mesh_raycast(mesh, ray, &raw_hit)) {
		apcad_inference_hit hit = no_hit();
		hit.type                = APCAD_INFERENCE_ON_FACE;
		hit.position            = raw_hit.position;
		hit.face                = raw_hit.face_index;
		// Distance-to-ray is ~0 by construction (it's an exact hit point on
		// the ray); the score only matters for tie-breaking within a tier,
		// and ON_FACE isn't the lowest tier anymore (ON_GROUND is below
		// it), so any finite score is fine here.
		consider(hit, 0.0f);
	}

	// ON_GROUND -- absolute last resort: intersect with the local Y=0
	// reference plane. This is what lets a Line tool place its very first
	// point on a mesh with no vertices/edges/faces at all yet, where every
	// candidate above has nothing to work with. Only fails (no candidate
	// added) if the ray is itself (near-)parallel to that plane, e.g.
	// looking exactly along the horizon.
	if (std::fabs(ray_dir.y) > 1e-6f) {
		float t = -ray_origin.y / ray_dir.y;
		if (t >= 0.0f) {
			glm::vec3 ground_point  = ray_origin + ray_dir * t;
			apcad_inference_hit hit = no_hit();
			hit.type                = APCAD_INFERENCE_ON_GROUND;
			hit.position            = from_glm(ground_point);
			consider(hit, 0.0f);
		}
	}

	if (!have_best)
		return false;
	*out_hit = best;
	return true;
}

} // extern "C"
