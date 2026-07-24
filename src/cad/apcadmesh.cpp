#include "cad/apcadmesh.h"
#include "apcad_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>
#include <unordered_map>
#include <vector>

/*
 * See apcad_mesh_add_edge's doc comment in apcadmesh.h for the auto-face
 * algorithm this file implements (candidate planes -> coplanar subgraph ->
 * dangling-edge pruning -> tightest-turn trace -> validate/dedupe). This
 * file is the reference implementation of that description; if the two
 * ever disagree, this comment block is wrong, not the header.
 */

// -------------------------
// Concrete storage. Id == index; erased elements are tombstoned (alive =
// false) rather than physically removed, so ids issued once never dangle
// or get reused -- required by apcadmesh.h's "ids may have gaps, iterate
// 0..range and skip dead ones" contract.

struct apcad_vertex_rec {
	bool alive = false;
	ApriVec3 position{};
	std::vector<apcad_edge_id> edges;
};

struct apcad_edge_rec {
	bool alive = false;
	bool is_construction  = false;
	apcad_vertex_id v0    = APCAD_INVALID_ID;
	apcad_vertex_id v1    = APCAD_INVALID_ID;
	apcad_face_id faces[2] = {APCAD_INVALID_ID, APCAD_INVALID_ID};
};

struct apcad_face_rec {
	bool alive = false;
	std::vector<apcad_vertex_id> loop_verts; // loop_verts[i] -> loop_verts[i+1] via loop_edges[i]
	std::vector<apcad_edge_id> loop_edges;
};

typedef struct apcad_mesh_t {
	std::vector<apcad_vertex_rec> vertices;
	std::vector<apcad_edge_rec> edges;
	std::vector<apcad_face_rec> faces;
} apcad_mesh_t;

namespace {

// Plane containment / collinearity tolerances. Both are absolute rather
// than relative to model scale -- fine while every consumer is small
// hand-drawn geometry; a real scale-relative epsilon is a follow-up once
// this is exercised at architectural scale.
constexpr float k_plane_epsilon      = 1e-4f;
constexpr float k_collinear_epsilon  = 1e-6f;
constexpr float k_two_pi             = 6.28318530717958647692f;

static inline apcad_mesh_t *mesh_alloc() {
	apcad_mesh_t *mesh = (apcad_mesh_t *)malloc(sizeof(apcad_mesh_t));
	if (mesh)
		mesh = new (mesh) apcad_mesh_t();
	return mesh;
}

bool vertex_alive(const apcad_mesh_t &mesh, apcad_vertex_id v) {
	return v < mesh.vertices.size() && mesh.vertices[v].alive;
}
bool edge_alive(const apcad_mesh_t &mesh, apcad_edge_id e) {
	return e < mesh.edges.size() && mesh.edges[e].alive;
}
bool face_alive(const apcad_mesh_t &mesh, apcad_face_id f) {
	return f < mesh.faces.size() && mesh.faces[f].alive;
}

apcad_vertex_id edge_other_vertex(const apcad_edge_rec &e, apcad_vertex_id from) {
	return e.v0 == from ? e.v1 : e.v0;
}

/* Search from `a`'s (shorter, typically) incident-edge list rather than
 * scanning every edge in the mesh. */
apcad_edge_id find_edge(const apcad_mesh_t &mesh, apcad_vertex_id a, apcad_vertex_id b) {
	for (apcad_edge_id eid : mesh.vertices[a].edges) {
		const apcad_edge_rec &e = mesh.edges[eid];
		if ((e.v0 == a && e.v1 == b) || (e.v0 == b && e.v1 == a))
			return eid;
	}
	return APCAD_INVALID_ID;
}

/* Newell's method -- identical to apcad_face_normal in apcad_internal.hpp,
 * just walking a mesh face's vertex-id loop instead of a solid's index
 * list. Kept as a separate copy rather than sharing that helper: solid and
 * mesh faces are indexed through unrelated types (uint32_t indices into a
 * flat vertex array vs. apcad_vertex_id into a tombstoned table), so
 * templating or overloading it would buy less than it costs to read. */
glm::vec3 mesh_face_normal_glm(const apcad_mesh_t &mesh, const apcad_face_rec &face) {
	glm::vec3 normal(0.0f);
	uint32_t count = (uint32_t)face.loop_verts.size();
	for (uint32_t i = 0; i < count; ++i) {
		glm::vec3 a = to_glm(mesh.vertices[face.loop_verts[i]].position);
		glm::vec3 b = to_glm(mesh.vertices[face.loop_verts[(i + 1) % count]].position);
		normal.x += (a.y - b.y) * (a.z + b.z);
		normal.y += (a.z - b.z) * (a.x + b.x);
		normal.z += (a.x - b.x) * (a.y + b.y);
	}
	float len = glm::length(normal);
	return len > 1e-12f ? (normal / len) : normal;
}

void erase_face_internal(apcad_mesh_t *mesh, apcad_face_id fid) {
	if (!face_alive(*mesh, fid))
		return;
	apcad_face_rec &f = mesh->faces[fid];
	for (apcad_edge_id eid : f.loop_edges) {
		apcad_edge_rec &e = mesh->edges[eid];
		if (e.faces[0] == fid)
			e.faces[0] = APCAD_INVALID_ID;
		else if (e.faces[1] == fid)
			e.faces[1] = APCAD_INVALID_ID;
	}
	f.alive = false;
	f.loop_verts.clear();
	f.loop_edges.clear();
}

/* Adjacency restricted to one candidate plane -- vertex id -> list of
 * (neighbor, connecting edge). Rebuilt fresh per candidate plane per call;
 * these graphs are small (one sketch's worth of coplanar edges), so no
 * persistent acceleration structure is warranted yet. */
using PlaneAdjacency = std::unordered_map<apcad_vertex_id, std::vector<std::pair<apcad_vertex_id, apcad_edge_id>>>;

PlaneAdjacency build_coplanar_adjacency(const apcad_mesh_t &mesh, glm::vec3 plane_point, glm::vec3 plane_normal) {
	PlaneAdjacency adj;
	for (apcad_edge_id eid = 0; eid < (apcad_edge_id)mesh.edges.size(); ++eid) {
		const apcad_edge_rec &e = mesh.edges[eid];
		if (!e.alive || e.is_construction)
			continue;
		glm::vec3 p0 = to_glm(mesh.vertices[e.v0].position);
		glm::vec3 p1 = to_glm(mesh.vertices[e.v1].position);
		if (fabsf(glm::dot(p0 - plane_point, plane_normal)) > k_plane_epsilon)
			continue;
		if (fabsf(glm::dot(p1 - plane_point, plane_normal)) > k_plane_epsilon)
			continue;
		adj[e.v0].push_back({e.v1, eid});
		adj[e.v1].push_back({e.v0, eid});
	}
	return adj;
}

/* Strips any vertex left with fewer than 2 coplanar edges, repeatedly --
 * such a vertex (a dangling stub, e.g. a half-drawn diagonal or a
 * construction line poking into the region) can never lie on a closed
 * loop, so removing it up front means the trace below never has to
 * consider -- or backtrack out of -- a dead end. */
void prune_dangling(PlaneAdjacency &adj) {
	bool changed = true;
	while (changed) {
		changed = false;
		for (auto it = adj.begin(); it != adj.end();) {
			if (it->second.size() >= 2) {
				++it;
				continue;
			}
			apcad_vertex_id dead = it->first;
			for (auto &kv : adj) {
				if (kv.first == dead)
					continue;
				auto &neighbors = kv.second;
				neighbors.erase(
				    std::remove_if(neighbors.begin(), neighbors.end(),
				        [dead](const std::pair<apcad_vertex_id, apcad_edge_id> &p) { return p.first == dead; }),
				    neighbors.end());
			}
			it      = adj.erase(it);
			changed = true;
		}
	}
}

void make_plane_basis(glm::vec3 normal, glm::vec3 &out_u, glm::vec3 &out_v) {
	glm::vec3 n      = glm::normalize(normal);
	glm::vec3 helper = (fabsf(n.x) < 0.9f) ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
	out_u            = glm::normalize(glm::cross(helper, n));
	out_v            = glm::cross(n, out_u);
}

float angle_in_plane(glm::vec3 dir, glm::vec3 u, glm::vec3 v) {
	return atan2f(glm::dot(dir, v), glm::dot(dir, u));
}

/* Smallest positive counterclockwise rotation to get from `from_angle` to
 * `to_angle`, in (0, 2*pi]. */
float ccw_delta(float from_angle, float to_angle) {
	float d = to_angle - from_angle;
	while (d <= 0.0f)
		d += k_two_pi;
	while (d > k_two_pi)
		d -= k_two_pi;
	return d;
}

/* The tightest-turn walk itself: starting along (start -> second), at each
 * vertex always continue via whichever remaining coplanar edge is the
 * smallest CCW rotation from the direction just arrived from -- "next
 * spoke around the wheel." Returns the closed loop's vertices (start not
 * repeated at the end) on success, or an empty vector if the walk dead-
 * ends, self-intersects, or exceeds the safety cap without closing (the
 * cap guards against malformed/non-manifold input; it is not how this
 * algorithm finds loops -- that's the deterministic rule above). */
std::vector<apcad_vertex_id> trace_face_loop(
    const apcad_mesh_t &mesh,
    const PlaneAdjacency &adj,
    glm::vec3 basis_u,
    glm::vec3 basis_v,
    apcad_vertex_id start,
    apcad_vertex_id second) {
	std::vector<apcad_vertex_id> path{start, second};
	apcad_vertex_id prev = start, cur = second;

	size_t cap = adj.size() * 2 + 4;
	for (size_t iter = 0; iter < cap; ++iter) {
		auto it = adj.find(cur);
		if (it == adj.end())
			return {}; // shouldn't happen post-prune, but stay defensive

		glm::vec3 cur_pos  = to_glm(mesh.vertices[cur].position);
		glm::vec3 prev_pos = to_glm(mesh.vertices[prev].position);
		float base_angle   = angle_in_plane(prev_pos - cur_pos, basis_u, basis_v);

		apcad_vertex_id best = APCAD_INVALID_ID;
		float best_delta     = 1e30f;
		for (const auto &nb : it->second) {
			if (nb.first == prev)
				continue;
			glm::vec3 n_pos = to_glm(mesh.vertices[nb.first].position);
			float angle     = angle_in_plane(n_pos - cur_pos, basis_u, basis_v);
			float delta     = ccw_delta(base_angle, angle);
			if (delta < best_delta) {
				best_delta = delta;
				best       = nb.first;
			}
		}
		if (best == APCAD_INVALID_ID)
			return {}; // dead end -- pruning should have prevented this

		if (best == start)
			return path; // closed

		for (apcad_vertex_id v : path)
			if (v == best)
				return {}; // would self-intersect -- reject rather than loop forever

		path.push_back(best);
		prev = cur;
		cur  = best;
	}
	return {}; // cap exceeded: malformed/non-manifold coplanar subgraph
}

/* Runs the full add_edge auto-face procedure against an edge that already
 * exists in `mesh` (new_edge's vertices/adjacency are already wired up).
 * Shared between apcad_mesh_add_edge and apcad_mesh_set_edge_construction
 * (false) -- un-construction-izing an edge re-triggers the same detection
 * a brand new edge would. */
void detect_and_create_faces(apcad_mesh_t *mesh, apcad_edge_id new_edge) {
	apcad_vertex_id a = mesh->edges[new_edge].v0;
	apcad_vertex_id b = mesh->edges[new_edge].v1;
	glm::vec3 pa      = to_glm(mesh->vertices[a].position);
	glm::vec3 pb      = to_glm(mesh->vertices[b].position);

	// Step 1: candidate plane normals. Every candidate plane's anchor point
	// is `a` -- it's a shared point on every one of them by construction,
	// which reduces "is this the same plane as one already found" to
	// comparing normals (up to sign) instead of full plane-equality.
	std::vector<glm::vec3> candidate_normals;
	auto try_third_point = [&](apcad_vertex_id third) {
		if (third == a || third == b)
			return;
		glm::vec3 pc  = to_glm(mesh->vertices[third].position);
		glm::vec3 cr  = glm::cross(pb - pa, pc - pa);
		float cr_len  = glm::length(cr);
		if (cr_len < k_collinear_epsilon)
			return; // a, b, third collinear -- no plane pinned down
		glm::vec3 n = cr / cr_len;
		for (const glm::vec3 &existing : candidate_normals)
			if (fabsf(glm::dot(existing, n)) > 0.999999f)
				return; // same plane (or its flip) already queued
		candidate_normals.push_back(n);
	};
	for (apcad_edge_id eid : mesh->vertices[a].edges) {
		if (eid == new_edge || mesh->edges[eid].is_construction)
			continue;
		try_third_point(edge_other_vertex(mesh->edges[eid], a));
	}
	for (apcad_edge_id eid : mesh->vertices[b].edges) {
		if (eid == new_edge || mesh->edges[eid].is_construction)
			continue;
		try_third_point(edge_other_vertex(mesh->edges[eid], b));
	}

	std::vector<std::vector<apcad_vertex_id>> found_loops;

	for (glm::vec3 normal : candidate_normals) {
		// Steps 2-3: coplanar subgraph, pruned, then traced from both
		// directions of the new edge (the two faces it could border, one
		// per side).
		PlaneAdjacency adj = build_coplanar_adjacency(*mesh, pa, normal);
		prune_dangling(adj);
		if (adj.find(a) == adj.end() || adj.find(b) == adj.end())
			continue; // new edge itself got pruned -- nothing closes here

		glm::vec3 bu, bv;
		make_plane_basis(normal, bu, bv);

		auto loop_ab = trace_face_loop(*mesh, adj, bu, bv, a, b);
		if (!loop_ab.empty())
			found_loops.push_back(std::move(loop_ab));
		auto loop_ba = trace_face_loop(*mesh, adj, bu, bv, b, a);
		if (!loop_ba.empty())
			found_loops.push_back(std::move(loop_ba));
	}

	// Step 4: validate + dedupe, then actually create faces.
	std::vector<std::vector<apcad_edge_id>> created_edge_sets;
	for (auto &loop : found_loops) {
		if (loop.size() < 3)
			continue;

		std::vector<apcad_edge_id> loop_edges;
		loop_edges.reserve(loop.size());
		bool edges_ok = true;
		for (size_t i = 0; i < loop.size(); ++i) {
			apcad_edge_id eid = find_edge(*mesh, loop[i], loop[(i + 1) % loop.size()]);
			if (eid == APCAD_INVALID_ID) {
				edges_ok = false;
				break;
			}
			loop_edges.push_back(eid);
		}
		if (!edges_ok)
			continue;

		// An edge already bordering 2 faces is full -- a third can't be
		// added (would break the "each edge borders at most 2 faces"
		// invariant apcad_mesh_edge_face_count promises).
		bool any_full = false;
		for (apcad_edge_id eid : loop_edges)
			if (mesh->edges[eid].faces[0] != APCAD_INVALID_ID && mesh->edges[eid].faces[1] != APCAD_INVALID_ID)
				any_full = true;
		if (any_full)
			continue;

		std::vector<apcad_edge_id> sorted_edges = loop_edges;
		std::sort(sorted_edges.begin(), sorted_edges.end());

		bool duplicate = false;
		for (const apcad_face_rec &f : mesh->faces) {
			if (!f.alive || f.loop_edges.size() != sorted_edges.size())
				continue;
			std::vector<apcad_edge_id> existing = f.loop_edges;
			std::sort(existing.begin(), existing.end());
			if (existing == sorted_edges) {
				duplicate = true;
				break;
			}
		}
		for (const auto &s : created_edge_sets)
			if (s == sorted_edges)
				duplicate = true;
		if (duplicate)
			continue;

		created_edge_sets.push_back(sorted_edges);

		apcad_face_rec f;
		f.alive      = true;
		f.loop_verts = loop;
		f.loop_edges = loop_edges;
		mesh->faces.push_back(std::move(f));
		apcad_face_id fid = (apcad_face_id)(mesh->faces.size() - 1);

		for (apcad_edge_id eid : loop_edges) {
			apcad_edge_rec &e = mesh->edges[eid];
			if (e.faces[0] == APCAD_INVALID_ID)
				e.faces[0] = fid;
			else
				e.faces[1] = fid;
		}
	}
}

} // namespace

// -------------------------
// PUBLIC API
extern "C" {

apcad_mesh apcad_mesh_create(void) {
	return mesh_alloc();
}

void apcad_mesh_destroy(apcad_mesh mesh) {
	if (mesh) {
		mesh->~apcad_mesh_t();
		free(mesh);
	}
}

apcad_mesh apcad_mesh_create_box(float width, float height, float depth) {
	apcad_mesh mesh = apcad_mesh_create();
	if (!mesh)
		return nullptr;

	float hw = width * 0.5f, hh = height * 0.5f, hd = depth * 0.5f;
	// Same 8 corners apcad_box_create's 6 independent quads implicitly
	// share -- back face (-Z) then front face (+Z), each CCW as seen from
	// outside its own face.
	ApriVec3 corners[8] = {
	    {-hw, -hh, -hd}, {hw, -hh, -hd}, {hw, hh, -hd}, {-hw, hh, -hd},
	    {-hw, -hh, hd}, {hw, -hh, hd}, {hw, hh, hd}, {-hw, hh, hd},
	};
	apcad_vertex_id v[8];
	for (int i = 0; i < 8; ++i)
		v[i] = apcad_mesh_weld_vertex(mesh, corners[i], 1e-5f);

	apcad_mesh_add_edge(mesh, v[0], v[1]);
	apcad_mesh_add_edge(mesh, v[1], v[2]);
	apcad_mesh_add_edge(mesh, v[2], v[3]);
	apcad_mesh_add_edge(mesh, v[3], v[0]); // closes the -Z face
	apcad_mesh_add_edge(mesh, v[4], v[5]);
	apcad_mesh_add_edge(mesh, v[5], v[6]);
	apcad_mesh_add_edge(mesh, v[6], v[7]);
	apcad_mesh_add_edge(mesh, v[7], v[4]); // closes the +Z face
	apcad_mesh_add_edge(mesh, v[0], v[4]);
	apcad_mesh_add_edge(mesh, v[1], v[5]);
	apcad_mesh_add_edge(mesh, v[2], v[6]);
	apcad_mesh_add_edge(mesh, v[3], v[7]); // each closes one of the remaining 4 side faces

	// Auto-face detection (apcad_mesh_add_edge) has no notion of "outward"
	// -- correct it here using domain knowledge specific to this shape: a
	// box is convex and centered on its own local origin, so a face whose
	// normal points roughly AWAY from the origin is right-side-out, and
	// one pointing roughly TOWARD the origin is backwards.
	apcad_mesh_t *m = mesh;
	for (apcad_face_id fid = 0; fid < (apcad_face_id)m->faces.size(); ++fid) {
		if (!m->faces[fid].alive)
			continue;
		glm::vec3 centroid(0.0f);
		for (apcad_vertex_id vid : m->faces[fid].loop_verts)
			centroid += to_glm(m->vertices[vid].position);
		centroid /= (float)m->faces[fid].loop_verts.size();

		glm::vec3 normal = mesh_face_normal_glm(*m, m->faces[fid]);
		if (glm::dot(normal, centroid) < 0.0f)
			apcad_mesh_face_flip(mesh, fid);
	}

	return mesh;
}

apcad_vertex_id apcad_mesh_weld_vertex(apcad_mesh mesh, ApriVec3 position, float weld_epsilon) {
	if (!mesh)
		return APCAD_INVALID_ID;

	glm::vec3 p     = to_glm(position);
	float eps_sq    = weld_epsilon * weld_epsilon;
	for (apcad_vertex_id vid = 0; vid < (apcad_vertex_id)mesh->vertices.size(); ++vid) {
		if (!mesh->vertices[vid].alive)
			continue;
		glm::vec3 q = to_glm(mesh->vertices[vid].position);
		glm::vec3 d = p - q;
		if (glm::dot(d, d) <= eps_sq)
			return vid;
	}

	apcad_vertex_rec v;
	v.alive    = true;
	v.position = position;
	mesh->vertices.push_back(std::move(v));
	return (apcad_vertex_id)(mesh->vertices.size() - 1);
}

apcad_edge_id apcad_mesh_add_edge(apcad_mesh mesh, apcad_vertex_id a, apcad_vertex_id b) {
	if (!mesh || a == b || !vertex_alive(*mesh, a) || !vertex_alive(*mesh, b))
		return APCAD_INVALID_ID;
	if (find_edge(*mesh, a, b) != APCAD_INVALID_ID)
		return APCAD_INVALID_ID;

	// Step 0 (not part of the header's numbered steps, added during
	// implementation -- see the "why" below): any existing face whose
	// boundary already contains BOTH new endpoints is a face this edge is
	// about to draw a chord through, and must be torn down first so the
	// smaller replacement(s) detected below aren't created on top of it.
	//
	// Why this step exists: without it, drawing a diagonal across an
	// already-closed quad correctly finds the two new triangles (the
	// tightest-turn trace handles that fine) but never removes the
	// now-invalid larger quad face, leaving three overlapping faces
	// instead of two. Caught by tracing this exact scenario by hand while
	// implementing -- worth remembering as the one place the original
	// header description was incomplete.
	std::vector<apcad_face_id> faces_to_split;
	for (apcad_face_id fid = 0; fid < (apcad_face_id)mesh->faces.size(); ++fid) {
		if (!mesh->faces[fid].alive)
			continue;
		const auto &lv = mesh->faces[fid].loop_verts;
		bool has_a      = std::find(lv.begin(), lv.end(), a) != lv.end();
		bool has_b      = std::find(lv.begin(), lv.end(), b) != lv.end();
		if (has_a && has_b)
			faces_to_split.push_back(fid);
	}
	for (apcad_face_id fid : faces_to_split)
		erase_face_internal(mesh, fid);

	apcad_edge_rec e;
	e.alive = true;
	e.v0    = a;
	e.v1    = b;
	mesh->edges.push_back(e);
	apcad_edge_id new_edge = (apcad_edge_id)(mesh->edges.size() - 1);
	mesh->vertices[a].edges.push_back(new_edge);
	mesh->vertices[b].edges.push_back(new_edge);

	detect_and_create_faces(mesh, new_edge);
	return new_edge;
}

void apcad_mesh_erase_edge(apcad_mesh mesh, apcad_edge_id edge) {
	if (!mesh || !edge_alive(*mesh, edge))
		return;

	apcad_edge_rec &e = mesh->edges[edge];
	if (e.faces[0] != APCAD_INVALID_ID)
		erase_face_internal(mesh, e.faces[0]);
	if (e.faces[1] != APCAD_INVALID_ID)
		erase_face_internal(mesh, e.faces[1]);

	apcad_vertex_id a = e.v0, b = e.v1;
	e.alive           = false;

	auto detach = [&](apcad_vertex_id v) {
		auto &edges = mesh->vertices[v].edges;
		edges.erase(std::remove(edges.begin(), edges.end(), edge), edges.end());
		if (edges.empty())
			mesh->vertices[v].alive = false;
	};
	detach(a);
	detach(b);
}

void apcad_mesh_erase_face(apcad_mesh mesh, apcad_face_id face) {
	if (!mesh)
		return;
	erase_face_internal(mesh, face);
}

void apcad_mesh_face_flip(apcad_mesh mesh, apcad_face_id face) {
	if (!mesh || !face_alive(*mesh, face))
		return;
	apcad_face_rec &f = mesh->faces[face];
	uint32_t n         = (uint32_t)f.loop_verts.size();
	if (n < 3)
		return;

	// new_verts[i] = old_verts[(n - i) % n] keeps old_verts[0] as the
	// first vertex but walks the rest backwards -- reversing the loop's
	// direction (and so its normal, per Newell's method) without
	// otherwise changing which vertex the loop "starts" at. new_edges is
	// simply old_edges reversed: old edge i connected old_verts[i] to
	// old_verts[i+1], so the new loop's edge i (connecting new_verts[i] to
	// new_verts[i+1]) is exactly old edge (n-1-i).
	std::vector<apcad_vertex_id> new_verts(n);
	for (uint32_t i = 0; i < n; ++i)
		new_verts[i] = f.loop_verts[(n - i) % n];
	f.loop_verts = std::move(new_verts);
	std::reverse(f.loop_edges.begin(), f.loop_edges.end());
}

void apcad_mesh_set_edge_construction(apcad_mesh mesh, apcad_edge_id edge, bool is_construction) {
	if (!mesh || !edge_alive(*mesh, edge))
		return;
	apcad_edge_rec &e = mesh->edges[edge];
	if (e.is_construction == is_construction)
		return;

	if (is_construction) {
		if (e.faces[0] != APCAD_INVALID_ID)
			erase_face_internal(mesh, e.faces[0]);
		if (e.faces[1] != APCAD_INVALID_ID)
			erase_face_internal(mesh, e.faces[1]);
		e.is_construction = true;
	} else {
		e.is_construction = false;
		detect_and_create_faces(mesh, edge);
	}
}

bool apcad_mesh_edge_is_construction(apcad_mesh mesh, apcad_edge_id edge) {
	return (mesh && edge_alive(*mesh, edge)) ? mesh->edges[edge].is_construction : false;
}

bool apcad_mesh_vertex_exists(apcad_mesh mesh, apcad_vertex_id v) {
	return mesh && vertex_alive(*mesh, v);
}

ApriVec3 apcad_mesh_vertex_position(apcad_mesh mesh, apcad_vertex_id v) {
	if (!mesh || !vertex_alive(*mesh, v))
		return ApriVec3{};
	return mesh->vertices[v].position;
}

void apcad_mesh_move_vertex(apcad_mesh mesh, apcad_vertex_id v, ApriVec3 new_position) {
	if (!mesh || !vertex_alive(*mesh, v))
		return;
	mesh->vertices[v].position = new_position;
}

bool apcad_mesh_edge_exists(apcad_mesh mesh, apcad_edge_id e) {
	return mesh && edge_alive(*mesh, e);
}

void apcad_mesh_edge_vertices(apcad_mesh mesh, apcad_edge_id e, apcad_vertex_id *out_a, apcad_vertex_id *out_b) {
	apcad_vertex_id a = APCAD_INVALID_ID, b = APCAD_INVALID_ID;
	if (mesh && edge_alive(*mesh, e)) {
		a = mesh->edges[e].v0;
		b = mesh->edges[e].v1;
	}
	if (out_a)
		*out_a = a;
	if (out_b)
		*out_b = b;
}

uint32_t apcad_mesh_edge_face_count(apcad_mesh mesh, apcad_edge_id e) {
	if (!mesh || !edge_alive(*mesh, e))
		return 0;
	const apcad_edge_rec &edge = mesh->edges[e];
	uint32_t count             = 0;
	if (edge.faces[0] != APCAD_INVALID_ID)
		++count;
	if (edge.faces[1] != APCAD_INVALID_ID)
		++count;
	return count;
}

apcad_face_id apcad_mesh_edge_face(apcad_mesh mesh, apcad_edge_id e, uint32_t index) {
	if (!mesh || !edge_alive(*mesh, e) || index > 1)
		return APCAD_INVALID_ID;
	return mesh->edges[e].faces[index];
}

bool apcad_mesh_face_exists(apcad_mesh mesh, apcad_face_id f) {
	return mesh && face_alive(*mesh, f);
}

uint32_t apcad_mesh_face_edge_count(apcad_mesh mesh, apcad_face_id f) {
	if (!mesh || !face_alive(*mesh, f))
		return 0;
	return (uint32_t)mesh->faces[f].loop_edges.size();
}

apcad_edge_id apcad_mesh_face_edge(apcad_mesh mesh, apcad_face_id f, uint32_t index) {
	if (!mesh || !face_alive(*mesh, f) || index >= mesh->faces[f].loop_edges.size())
		return APCAD_INVALID_ID;
	return mesh->faces[f].loop_edges[index];
}

apcad_vertex_id apcad_mesh_face_vertex(apcad_mesh mesh, apcad_face_id f, uint32_t index) {
	if (!mesh || !face_alive(*mesh, f) || index >= mesh->faces[f].loop_verts.size())
		return APCAD_INVALID_ID;
	return mesh->faces[f].loop_verts[index];
}

ApriVec3 apcad_mesh_face_normal(apcad_mesh mesh, apcad_face_id f) {
	if (!mesh || !face_alive(*mesh, f))
		return ApriVec3{};
	return from_glm(mesh_face_normal_glm(*mesh, mesh->faces[f]));
}

uint32_t apcad_mesh_vertex_id_range(apcad_mesh mesh) {
	return mesh ? (uint32_t)mesh->vertices.size() : 0;
}
uint32_t apcad_mesh_edge_id_range(apcad_mesh mesh) {
	return mesh ? (uint32_t)mesh->edges.size() : 0;
}
uint32_t apcad_mesh_face_id_range(apcad_mesh mesh) {
	return mesh ? (uint32_t)mesh->faces.size() : 0;
}

void apcad_mesh_tessellate(apcad_mesh mesh, aprend_mesh mesh_out) {
	if (!mesh || !mesh_out)
		return;

	std::vector<APREND_DEFAULT_VERTEX> vertices;
	std::vector<uint32_t> indices;

	for (const apcad_face_rec &face : mesh->faces) {
		if (!face.alive)
			continue;
		uint32_t count = (uint32_t)face.loop_verts.size();
		if (count < 3)
			continue;

		glm::vec3 normal = mesh_face_normal_glm(*mesh, face);

		uint32_t base = (uint32_t)vertices.size();
		for (uint32_t i = 0; i < count; ++i) {
			const ApriVec3 &p = mesh->vertices[face.loop_verts[i]].position;
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

		// Fan triangulation -- inherits apcad_solid_tessellate's convex-face
		// assumption; see apcad_mesh_add_edge's "known gaps" comment for why
		// that isn't validated here yet.
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

	aprend_mesh_set_vertex_bindings(mesh_out, &binding, 1);
	aprend_mesh_set_vertex_binding_data(mesh_out, 0, (uint32_t)vertices.size(), vertices.data());
	aprend_mesh_set_indices(mesh_out, APREND_INDEX_STRIDE_UINT32, (uint32_t)indices.size(), indices.data());
	aprend_mesh_upload(mesh_out);
}

void apcad_mesh_tessellate_wireframe(apcad_mesh mesh, aprend_mesh mesh_out, bool include_construction) {
	if (!mesh || !mesh_out)
		return;

	// Same APREND_DEFAULT_VERTEX layout as apcad_mesh_tessellate (normal/uv
	// unused, color left white) rather than a position-only format, so a
	// host can reuse one generic vertex-attribute layout for both a
	// line-topology wireframe material and a triangle-topology face
	// material -- see aprend_material_desc::primitive_topology, which is
	// what actually decides lines vs. triangles at draw time; this
	// function only ever emits data, never selects a topology itself.
	std::vector<APREND_DEFAULT_VERTEX> vertices;
	std::vector<uint32_t> indices;
	vertices.reserve(mesh->edges.size() * 2);
	indices.reserve(mesh->edges.size() * 2);

	for (const apcad_edge_rec &edge : mesh->edges) {
		if (!edge.alive)
			continue;
		if (edge.is_construction && !include_construction)
			continue;

		ApriVec3 p0 = mesh->vertices[edge.v0].position;
		ApriVec3 p1 = mesh->vertices[edge.v1].position;

		uint32_t base = (uint32_t)vertices.size();
		for (const ApriVec3 &p : {p0, p1}) {
			APREND_DEFAULT_VERTEX v{};
			v.position[0] = p.x;
			v.position[1] = p.y;
			v.position[2] = p.z;
			v.normal[0] = v.normal[1] = v.normal[2] = 0.0f;
			v.uv[0] = v.uv[1] = 0.0f;
			v.color[0] = v.color[1] = v.color[2] = v.color[3] = 1.0f;
			vertices.push_back(v);
		}
		indices.push_back(base);
		indices.push_back(base + 1);
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

	aprend_mesh_set_vertex_bindings(mesh_out, &binding, 1);
	aprend_mesh_set_vertex_binding_data(mesh_out, 0, (uint32_t)vertices.size(), vertices.data());
	aprend_mesh_set_indices(mesh_out, APREND_INDEX_STRIDE_UINT32, (uint32_t)indices.size(), indices.data());
	aprend_mesh_upload(mesh_out);
}

} // extern "C"
