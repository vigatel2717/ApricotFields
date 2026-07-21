#ifndef APCAD_INTERNAL_HPP
#define APCAD_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/cad/.
 * Defines the concrete struct behind apcad_solid, and shared helpers used by
 * both apcadsolid.cpp (construction/tessellation) and apcadraycast.cpp
 * (picking) so the two never drift out of sync on face-normal math. */

#include "cad/apcadsolid.h"

#include <glm/glm.hpp>
#include <vector>

inline glm::vec3 to_glm(const ApriVec3 &v) { return glm::vec3(v.x, v.y, v.z); }
inline ApriVec3 from_glm(const glm::vec3 &v) { return {v.x, v.y, v.z}; }

/* A single planar convex polygon, wound CCW as seen from outside the solid.
 * Indices refer into apcad_solid_t::vertices. */
struct apcad_face {
	std::vector<uint32_t> indices;
};

typedef struct apcad_solid_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	std::vector<ApriVec3> vertices;
	std::vector<apcad_face> faces;
} apcad_solid_t;

/* Newell's method — robust face normal for a planar (or near-planar) n-gon,
 * consistent with the CCW-outward winding used throughout this module. */
inline glm::vec3 apcad_face_normal(const apcad_solid_t &solid, const apcad_face &face) {
	glm::vec3 normal(0.0f);
	uint32_t count = (uint32_t)face.indices.size();
	for (uint32_t i = 0; i < count; ++i) {
		const ApriVec3 &a = solid.vertices[face.indices[i]];
		const ApriVec3 &b = solid.vertices[face.indices[(i + 1) % count]];
		normal.x += (a.y - b.y) * (a.z + b.z);
		normal.y += (a.z - b.z) * (a.x + b.x);
		normal.z += (a.x - b.x) * (a.y + b.y);
	}
	return glm::normalize(normal);
}

#endif // APCAD_INTERNAL_HPP
