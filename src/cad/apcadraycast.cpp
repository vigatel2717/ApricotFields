#include "cad/apcadraycast.h"
#include "apcad_internal.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

bool apcad_solid_raycast(apcad_solid solid, apcad_ray ray, apcad_raycast_hit *out_hit) {
	if (!solid)
		return false;

	glm::vec3 origin = to_glm(ray.origin);
	glm::vec3 dir     = glm::normalize(to_glm(ray.direction));

	float best_t          = std::numeric_limits<float>::max();
	uint32_t best_face    = UINT32_MAX;
	glm::vec3 best_normal = glm::vec3(0.0f);
	glm::vec3 best_point  = glm::vec3(0.0f);

	for (uint32_t f = 0; f < (uint32_t)solid->faces.size(); ++f) {
		const apcad_face &face = solid->faces[f];
		uint32_t count         = (uint32_t)face.indices.size();
		if (count < 3)
			continue;

		glm::vec3 normal      = apcad_face_normal(*solid, face);
		glm::vec3 plane_point = to_glm(solid->vertices[face.indices[0]]);

		float denom = glm::dot(dir, normal);
		if (std::fabs(denom) < 1e-8f)
			continue; /* ray parallel to the face's plane */

		float t = glm::dot(plane_point - origin, normal) / denom;
		if (t < 0.0f || t >= best_t)
			continue; /* behind the ray origin, or not closer than the current best hit */

		glm::vec3 point = origin + dir * t;

		/* Point-in-convex-polygon test: faces are wound CCW as seen from
		 * outside (outward normal), so the hit point is inside iff it's on
		 * the inward side of every edge. A small negative tolerance is
		 * required here: a ray aimed exactly at a shared vertex/edge (e.g.
		 * a cylinder cap seam) computes as *slightly* outside due to
		 * floating-point round-off in every adjacent face's own test, and
		 * without slack the point falls through all of them and the ray
		 * incorrectly passes through to a farther face instead of hitting
		 * here — this scales the tolerance by the edge length since the
		 * cross product's magnitude does too. */
		bool inside = true;
		for (uint32_t i = 0; i < count && inside; ++i) {
			glm::vec3 a        = to_glm(solid->vertices[face.indices[i]]);
			glm::vec3 b        = to_glm(solid->vertices[face.indices[(i + 1) % count]]);
			glm::vec3 edge     = b - a;
			glm::vec3 to_point = point - a;
			float epsilon      = 1e-5f * glm::length(edge);
			if (glm::dot(glm::cross(edge, to_point), normal) < -epsilon)
				inside = false;
		}
		if (!inside)
			continue;

		best_t      = t;
		best_face   = f;
		best_normal = normal;
		best_point  = point;
	}

	if (best_face == UINT32_MAX)
		return false;

	if (out_hit) {
		out_hit->distance   = best_t;
		out_hit->position   = from_glm(best_point);
		out_hit->normal     = from_glm(best_normal);
		out_hit->face_index = best_face;
	}
	return true;
}
