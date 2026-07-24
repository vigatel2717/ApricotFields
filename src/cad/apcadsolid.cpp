#include "cad/apcadsolid.h"
#include "apcad_internal.hpp"

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

namespace {

constexpr float k_two_pi = 6.28318530717958647692f;
constexpr float k_pi     = 3.14159265358979323846f;

static inline apcad_solid_t *solid_alloc() {
	apcad_solid_t *solid = (apcad_solid_t *)malloc(sizeof(apcad_solid_t));
	if (solid)
		solid = new (solid) apcad_solid_t();
	return solid;
}

void add_face(
    apcad_solid_t &solid,
    const ApriVec3 *points,
    uint32_t count) {
	apcad_face face;
	uint32_t base = (uint32_t)solid.vertices.size();
	face.indices.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		solid.vertices.push_back(points[i]);
		face.indices.push_back(base + i);
	}
	solid.faces.push_back(std::move(face));
}

/* Parallelogram face: corner, corner+u, corner+u+v, corner+v.
 * Callers pick u/v so that cross(u, v) is the outward normal. */
void add_quad(
    apcad_solid_t &solid,
    ApriVec3 corner,
    ApriVec3 u,
    ApriVec3 v) {
	glm::vec3 c = to_glm(corner), gu = to_glm(u), gv = to_glm(v);
	ApriVec3 pts[4] = {
	    from_glm(c),
	    from_glm(c + gu),
	    from_glm(c + gu + gv),
	    from_glm(c + gv),
	};
	add_face(solid, pts, 4);
}

} // namespace

// -------------------------
// PUBLIC API
extern "C" {

apcad_solid apcad_box_create(
    float width,
    float height,
    float depth) {
	apcad_solid_t *solid = solid_alloc();
	if (!solid)
		return nullptr;

	float hw = width * 0.5f, hh = height * 0.5f, hd = depth * 0.5f;

	/* Each face: a corner plus two edge vectors (u, v) chosen so
	 * cross(u, v) points outward. */
	add_quad(*solid, {hw, -hh, -hd}, {0, height, 0}, {0, 0, depth});  /* +X */
	add_quad(*solid, {-hw, -hh, -hd}, {0, 0, depth}, {0, height, 0}); /* -X */
	add_quad(*solid, {-hw, hh, -hd}, {0, 0, depth}, {width, 0, 0});   /* +Y */
	add_quad(*solid, {-hw, -hh, -hd}, {width, 0, 0}, {0, 0, depth});  /* -Y */
	add_quad(*solid, {-hw, -hh, hd}, {width, 0, 0}, {0, height, 0});  /* +Z */
	add_quad(*solid, {-hw, -hh, -hd}, {0, height, 0}, {width, 0, 0}); /* -Z */

	return solid;
}

apcad_solid apcad_cylinder_create(
    float radius,
    float height,
    uint32_t segments) {
	if (segments < 3)
		segments = 3;

	apcad_solid_t *solid = solid_alloc();
	if (!solid)
		return nullptr;

	float half_h = height * 0.5f;
	std::vector<ApriVec3> bottom_rim(segments), top_rim(segments);
	for (uint32_t i = 0; i < segments; ++i) {
		float theta   = (float)i / (float)segments * k_two_pi;
		float x       = radius * cosf(theta);
		float z       = radius * sinf(theta);
		bottom_rim[i] = {x, -half_h, z};
		top_rim[i]    = {x, half_h, z};
	}

	/* Rim traversal (increasing theta) is CCW as seen from above (+Y
	 * looking down), so the bottom cap uses it directly (outward -Y) and
	 * the top cap reverses it (outward +Y) — same rule as
	 * apcad_extrude_create's bottom/top faces. */
	add_face(*solid, bottom_rim.data(), segments);
	std::vector<ApriVec3> top_rim_rev(top_rim.rbegin(), top_rim.rend());
	add_face(*solid, top_rim_rev.data(), segments);

	for (uint32_t i = 0; i < segments; ++i) {
		uint32_t j = (i + 1) % segments;
		ApriVec3 u = {0, height, 0};
		ApriVec3 v = {bottom_rim[j].x - bottom_rim[i].x, 0, bottom_rim[j].z - bottom_rim[i].z};
		add_quad(*solid, bottom_rim[i], u, v);
	}

	return solid;
}

apcad_solid apcad_sphere_create(
    float radius,
    uint32_t segments,
    uint32_t rings) {
	if (segments < 3)
		segments = 3;
	if (rings < 2)
		rings = 2;

	apcad_solid_t *solid = solid_alloc();
	if (!solid)
		return nullptr;

	/* pts[i][j]: i = latitude ring (0 = north pole, rings = south pole), j = longitude. */
	std::vector<std::vector<ApriVec3>> pts(rings + 1, std::vector<ApriVec3>(segments));
	for (uint32_t i = 0; i <= rings; ++i) {
		float phi         = (float)i / (float)rings * k_pi;
		float y           = radius * cosf(phi);
		float ring_radius = radius * sinf(phi);
		for (uint32_t j = 0; j < segments; ++j) {
			float theta = (float)j / (float)segments * k_two_pi;
			pts[i][j]   = {ring_radius * cosf(theta), y, ring_radius * sinf(theta)};
		}
	}

	for (uint32_t i = 0; i < rings; ++i) {
		uint32_t i1          = i + 1;
		bool north_pole_band = (i == 0);
		bool south_pole_band = (i1 == rings);

		for (uint32_t j = 0; j < segments; ++j) {
			uint32_t j1 = (j + 1) % segments;

			if (north_pole_band) {
				/* pts[0][*] all coincide at the pole — only one non-degenerate triangle. */
				ApriVec3 tri[3] = {pts[i][j], pts[i1][j1], pts[i1][j]};
				add_face(*solid, tri, 3);
			} else if (south_pole_band) {
				ApriVec3 tri[3] = {pts[i][j], pts[i][j1], pts[i1][j]};
				add_face(*solid, tri, 3);
			} else {
				ApriVec3 quad[4] = {pts[i][j], pts[i][j1], pts[i1][j1], pts[i1][j]};
				add_face(*solid, quad, 4);
			}
		}
	}

	return solid;
}

apcad_solid apcad_extrude_create(
    const ApriVec2 *polygon,
    uint32_t point_count,
    float height) {
	if (!polygon || point_count < 3)
		return nullptr;

	apcad_solid_t *solid = solid_alloc();
	if (!solid)
		return nullptr;

	std::vector<ApriVec3> bottom(point_count), top(point_count);
	for (uint32_t i = 0; i < point_count; ++i) {
		bottom[i] = {polygon[i].x, 0.0f, polygon[i].y};
		top[i]    = {polygon[i].x, height, polygon[i].y};
	}

	/* Input is CCW as seen from above -> direct order is outward -Y
	 * (bottom face), reversed order is outward +Y (top face). */
	add_face(*solid, bottom.data(), point_count);
	std::vector<ApriVec3> top_rev(top.rbegin(), top.rend());
	add_face(*solid, top_rev.data(), point_count);

	for (uint32_t i = 0; i < point_count; ++i) {
		uint32_t j = (i + 1) % point_count;
		ApriVec3 u = {0, height, 0};
		ApriVec3 v = {bottom[j].x - bottom[i].x, 0, bottom[j].z - bottom[i].z};
		add_quad(*solid, bottom[i], u, v);
	}

	return solid;
}

void apcad_solid_destroy(apcad_solid solid) {
	if (solid) {
		solid->~apcad_solid_t();
		free(solid);
	}
}

void apcad_solid_tessellate(
    apcad_solid solid,
    aprend_mesh mesh) {
	if (!solid || !mesh)
		return;

	std::vector<APREND_DEFAULT_VERTEX> vertices;
	std::vector<uint32_t> indices;

	for (const apcad_face &face : solid->faces) {
		uint32_t count = (uint32_t)face.indices.size();
		if (count < 3)
			continue;

		glm::vec3 normal = apcad_face_normal(*solid, face);

		uint32_t base = (uint32_t)vertices.size();
		for (uint32_t i = 0; i < count; ++i) {
			const ApriVec3 &p = solid->vertices[face.indices[i]];
			APREND_DEFAULT_VERTEX v{};
			v.position[0] = p.x;
			v.position[1] = p.y;
			v.position[2] = p.z;
			v.normal[0]   = normal.x;
			v.normal[1]   = normal.y;
			v.normal[2]   = normal.z;
			v.uv[0]       = 0.0f;
			v.uv[1]       = 0.0f;
			v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
			vertices.push_back(v);
		}

		/* Fan triangulation — faces are required to be convex. */
		for (uint32_t i = 1; i + 1 < count; ++i) {
			indices.push_back(base);
			indices.push_back(base + i);
			indices.push_back(base + i + 1);
		}
	}

	std::vector<aprend_buffer_element> elements(4);
	elements[0] = {"POSITION", APREND_BUFFER_ELEMENT_TYPE_VEC3, sizeof(float) * 3, (uint32_t)offsetof(APREND_DEFAULT_VERTEX, position)};
	elements[1] = {"NORMAL", APREND_BUFFER_ELEMENT_TYPE_VEC3, sizeof(float) * 3, (uint32_t)offsetof(APREND_DEFAULT_VERTEX, normal)};
	elements[2] = {"UV", APREND_BUFFER_ELEMENT_TYPE_VEC2, sizeof(float) * 2, (uint32_t)offsetof(APREND_DEFAULT_VERTEX, uv)};
	elements[3] = {"COLOR", APREND_BUFFER_ELEMENT_TYPE_VEC4, sizeof(float) * 4, (uint32_t)offsetof(APREND_DEFAULT_VERTEX, color)};

	aprend_vertex_binding binding{};
	binding.binding         = 0;
	binding.layout.elements = elements.data();
	binding.layout.count    = (uint32_t)elements.size();

	aprend_mesh_set_vertex_bindings(mesh, &binding, 1);
	aprend_mesh_set_vertex_binding_data(mesh, 0, (uint32_t)vertices.size(), vertices.data());
	aprend_mesh_set_indices(mesh, APREND_INDEX_STRIDE_UINT32, (uint32_t)indices.size(), indices.data());
	aprend_mesh_upload(mesh);
}

} // Extern "C"
