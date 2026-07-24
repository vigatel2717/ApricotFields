#include "cad/apcadraycast.h"
#include "apcad_internal.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

// --------------------------
// PUBLIC API
extern "C" {

bool apcad_solid_raycast(
    apcad_solid solid,
    apcad_ray ray,
    apcad_raycast_hit *out_hit) {
	if (!solid)
		return false;

	glm::vec3 origin = to_glm(ray.origin);
	glm::vec3 dir    = glm::normalize(to_glm(ray.direction));

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

bool apcad_mesh_raycast(
    apcad_mesh mesh,
    apcad_ray ray,
    apcad_raycast_hit *out_hit) {
	if (!mesh)
		return false;

	glm::vec3 origin = to_glm(ray.origin);
	glm::vec3 dir    = glm::normalize(to_glm(ray.direction));

	float best_t            = std::numeric_limits<float>::max();
	apcad_face_id best_face = APCAD_INVALID_ID;
	glm::vec3 best_normal   = glm::vec3(0.0f);
	glm::vec3 best_point    = glm::vec3(0.0f);

	uint32_t face_range = apcad_mesh_face_id_range(mesh);
	for (apcad_face_id f = 0; f < face_range; ++f) {
		if (!apcad_mesh_face_exists(mesh, f))
			continue;
		uint32_t count = apcad_mesh_face_edge_count(mesh, f); // == vertex loop length
		if (count < 3)
			continue;

		glm::vec3 normal      = to_glm(apcad_mesh_face_normal(mesh, f));
		glm::vec3 plane_point = to_glm(apcad_mesh_vertex_position(mesh, apcad_mesh_face_vertex(mesh, f, 0)));

		float denom = glm::dot(dir, normal);
		if (std::fabs(denom) < 1e-8f)
			continue; /* ray parallel to the face's plane */

		float t = glm::dot(plane_point - origin, normal) / denom;
		if (t < 0.0f || t >= best_t)
			continue; /* behind the ray origin, or not closer than the current best hit */

		glm::vec3 point = origin + dir * t;

		/* Same convex-polygon inside test (and same reasoning for the
		 * epsilon) as apcad_solid_raycast above -- kept identical rather
		 * than shared, since the two operate over unrelated vertex-loop
		 * representations (a flat index array vs. apcad_mesh's id-based
		 * query API). */
		bool inside = true;
		for (uint32_t i = 0; i < count && inside; ++i) {
			glm::vec3 a        = to_glm(apcad_mesh_vertex_position(mesh, apcad_mesh_face_vertex(mesh, f, i)));
			glm::vec3 b        = to_glm(apcad_mesh_vertex_position(mesh, apcad_mesh_face_vertex(mesh, f, (i + 1) % count)));
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

	if (best_face == APCAD_INVALID_ID)
		return false;

	if (out_hit) {
		out_hit->distance   = best_t;
		out_hit->position   = from_glm(best_point);
		out_hit->normal     = from_glm(best_normal);
		out_hit->face_index = best_face;
	}
	return true;
}

apcad_ray apcad_camera_unproject_ray(
    aprend_camera cam,
    ApriDVec3 eye_world,
    float ndc_x,
    float ndc_y) {
	ApriMat4 proj_m         = aprend_camera_get_projection(cam);
	ApriMat4 view_m         = aprend_camera_get_view(cam);
	glm::mat4 inv_view_proj = glm::inverse(to_glm(proj_m) * to_glm(view_m));

	glm::vec4 far_world = inv_view_proj * glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);
	far_world /= far_world.w;

	glm::vec3 origin = glm::vec3(to_glm(eye_world));
	glm::vec3 dir    = glm::normalize(glm::vec3(far_world) - origin);

	return apcad_ray{from_glm(origin), from_glm(dir)};
}

apcad_ray apcad_ray_to_local(
    apcad_ray world_ray,
    ApriMat4 node_world_transform) {
	glm::mat4 inv_world = glm::inverse(to_glm(node_world_transform));

	glm::vec3 local_origin = glm::vec3(inv_world * glm::vec4(to_glm(world_ray.origin), 1.0f));
	glm::vec3 local_dir    = glm::normalize(glm::vec3(inv_world * glm::vec4(to_glm(world_ray.direction), 0.0f)));

	return apcad_ray{from_glm(local_origin), from_glm(local_dir)};
}

ApriVec3 apcad_local_point_to_world(
    ApriVec3 local_point,
    ApriMat4 node_world_transform) {
	glm::vec3 world_point = glm::vec3(to_glm(node_world_transform) * glm::vec4(to_glm(local_point), 1.0f));
	return from_glm(world_point);
}

} // Extern "C"
