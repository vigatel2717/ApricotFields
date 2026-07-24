
#ifndef APCAD_MESH_H
#define APCAD_MESH_H

#include "aprimath.h"
#include "render/aprendscene.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mesh  —  editable edge/vertex/face topology (SketchUp-style authoring)
 *
 * DRAFT for review — not yet implemented, not wired into CMakeLists. Sketch
 * of the successor to apcad_solid's flat "list of independent faces": here
 * vertices and edges are shared, first-class entities, and a face is never
 * authored directly — it exists only because some set of edges currently
 * forms a closed, planar loop. Draw the last edge of a triangle and a face
 * appears; erase one of its edges and the face is gone. That single rule is
 * the mechanism behind SketchUp's "just draw lines" feel, and it's the one
 * thing apcad_solid's representation structurally cannot express (its faces
 * don't share vertices, so there's no "loop" to detect).
 *
 * Recommended migration: this type subsumes apcad_solid rather than living
 * alongside it long-term. apcad_box_create/apcad_cylinder_create/etc. become
 * convenience constructors that build an initial apcad_mesh topology (a box
 * is 8 vertices + 12 edges + 6 auto-detected faces); apcad_solid_raycast's
 * logic generalizes to face-list-vs-ray here; Move/Rotate swap their
 * `apcad_solid` parameter for `apcad_mesh`. Keeping both representations
 * around indefinitely would mean every future tool has to know which one a
 * given entity is -- better to migrate the primitives than fork the model.
 *
 * IDs, not pointers/indices-into-a-vector: vertex/edge/face handles must
 * stay valid across edits (a tool may hold "the face under the cursor" or
 * "the vertex I just placed" across several frames while the mesh keeps
 * changing underneath it). An id can be looked up in O(1) via an internal
 * generation-checked table; a raw index would dangle the moment an earlier
 * element is erased and the array compacts.
 *
 * World convention and face winding match apcad_solid: Y-up, CCW as seen
 * from outside.
 */

typedef struct apcad_mesh_t *apcad_mesh;

typedef uint32_t apcad_vertex_id;
typedef uint32_t apcad_edge_id;
typedef uint32_t apcad_face_id;

#define APCAD_INVALID_ID ((uint32_t)0xFFFFFFFFu)

apcad_mesh apcad_mesh_create(void);
void apcad_mesh_destroy(apcad_mesh mesh);

/* Convenience constructor: the same box apcad_box_create builds (same
 * corner positions, same Y-up/CCW-outward convention), but as a fully
 * editable apcad_mesh -- 8 shared vertices, 12 edges, and 6 auto-detected
 * faces. Auto-face detection alone has no notion of "outward" (see
 * apcad_mesh_add_edge's "known gaps" comment) -- a face can come out wound
 * backwards relative to its neighbors purely from which trace direction
 * happened to win the internal dedup race. This constructor is the one
 * place in this file that DOES have the domain knowledge to correct that
 * (a box is convex and centered on its own local origin, so "does this
 * face's normal point away from the center" is a reliable outward test),
 * and calls apcad_mesh_face_flip on any face that fails it. That trick is
 * specific to this shape, not a general fix -- see apcad_mesh_face_flip. */
apcad_mesh apcad_mesh_create_box(
    float width,
    float height,
    float depth);

/*
 * Editing — the primitives a drawing tool calls once per user action.
 * Each of these can add/remove faces as a side effect of auto-face
 * detection; there is no separate "make face" call.
 */

/* Returns the id of an existing vertex within `weld_epsilon` of `position`,
 * otherwise creates and returns a new one. Drawing tools should always go
 * through this rather than ever creating a vertex unconditionally, so that
 * clicking near an existing corner reuses it instead of leaving a coincident
 * duplicate -- this is the data-layer half of "endpoint" inference;
 * apcadinference.h's job is telling the host *when* the cursor is close
 * enough that it should snap here and show the inference cue, not doing the
 * welding itself. */
apcad_vertex_id apcad_mesh_weld_vertex(
    apcad_mesh mesh,
    ApriVec3 position,
    float weld_epsilon);

/* Adds an edge between two existing vertices. Returns APCAD_INVALID_ID if
 * `a`/`b` don't exist, `a == b`, or that edge (either winding) already
 * exists.
 *
 * Side effect -- auto-face detection, via a deterministic "tightest turn"
 * planar trace (the same principle a DCEL uses to extract faces from a
 * planar subdivision), NOT a backtracking search:
 *
 *   1. Candidate planes: since a's and b's *other* incident edges are the
 *      only things that can supply the third, non-collinear point needed
 *      to pin down a plane through the new edge, every candidate plane
 *      comes from pairing (a, b) with one such existing edge at a or at b.
 *      A brand-new vertex (no other edges yet) contributes no candidates,
 *      correctly -- it can't be closing anything yet.
 *   2. For each candidate plane, collect the subgraph of edges coplanar
 *      with it (both endpoints within plane_epsilon), then iteratively
 *      strip any vertex left with coplanar-degree < 2 -- a loop can never
 *      pass through a dangling stub, so this prevents the trace below
 *      from ever wandering into one.
 *   3. Trace from (a -> b) and from (b -> a): at each vertex, keep
 *      choosing whichever remaining coplanar edge is the smallest
 *      counterclockwise rotation from the direction just arrived from
 *      (spokes-of-a-wheel, take the very next one). This always closes
 *      the SMALLEST enclosing loop, if any -- a diagonal already
 *      subdividing the region is followed, never skipped past, so an
 *      existing smaller face can't accidentally get papered over by a
 *      bigger one. The two starting directions give the (up to) two
 *      faces adjacent to the new edge, one per side.
 *   4. Reject loops under 3 vertices, self-intersecting traces, or loops
 *      matching a face that already exists; dedupe remaining candidates
 *      by edge-id set before creating faces (multiple candidate planes
 *      can rediscover the same physical loop).
 *
 * Known gaps, left as open decisions rather than silently resolved:
 *   - A traced loop is not guaranteed convex, but apcad_mesh_tessellate
 *     (like apcad_solid_tessellate before it) currently assumes convex
 *     faces for fan triangulation. Until that's resolved (reject
 *     non-convex loops from auto-facing, or switch to ear-clipping),
 *     treat non-convex closures as undefined.
 *   - A new loop's winding says which way the trace went, not which way
 *     is "outward" for the eventual solid -- that's only meaningful once
 *     a watertight shell exists, and needs its own consistent-orientation
 *     pass even then. No apcad_mesh_face_flip exists yet to correct this
 *     by hand.
 *   - No edge-crossing auto-split: unlike SketchUp, drawing an edge across
 *     existing geometry does not insert a vertex at the intersection, so
 *     that geometry won't auto-face the way SketchUp would until this
 *     function also does intersection detection ahead of the steps above.
 */
apcad_edge_id apcad_mesh_add_edge(
    apcad_mesh mesh,
    apcad_vertex_id a,
    apcad_vertex_id b);

/* Erases an edge. Any face(s) bounded by it are removed too (a face can't
 * outlive one of its edges -- erase one edge of a box and you get an open
 * shell, same as SketchUp). Vertices left with zero remaining edges are
 * pruned automatically. */
void apcad_mesh_erase_edge(
    apcad_mesh mesh,
    apcad_edge_id edge);

/* Erases a face without touching its bounding edges or triggering their
 * removal -- "open this face up" (e.g. to see inside a solid) while leaving
 * the wireframe intact. The face is free to reappear via auto-face
 * detection the next time apcad_mesh_add_edge touches one of its edges
 * (e.g. undo-of-erase-then-redraw), same as SketchUp letting a deleted face
 * "heal" if its loop is still closed. */
void apcad_mesh_erase_face(
    apcad_mesh mesh,
    apcad_face_id face);

/* Reverses a face's vertex/edge loop in place, flipping its normal.
 * Direct answer to apcad_mesh_add_edge's own "known gaps" comment:
 * auto-face detection has no way to know which side of a newly-closed
 * loop is "outward" for the eventual solid, so a face can come out wound
 * backwards relative to its neighbors. This is the correction tool for
 * that, for callers who DO have outward-orientation knowledge from
 * elsewhere (e.g. apcad_mesh_create_box, or a future whole-mesh
 * "recalculate normals" pass) -- it does not determine orientation
 * itself. */
void apcad_mesh_face_flip(
    apcad_mesh mesh,
    apcad_face_id face);

/* Marks an edge as construction (guide) geometry: never tessellated into
 * solid faces, never a candidate for auto-face loops, drawn by the host as
 * a dashed guide line rather than a real edge. Toggling this can create or
 * destroy faces that depended on the edge for closure, same as
 * add_edge/erase_edge. */
void apcad_mesh_set_edge_construction(
    apcad_mesh mesh,
    apcad_edge_id edge,
    bool is_construction);
bool apcad_mesh_edge_is_construction(
    apcad_mesh mesh,
    apcad_edge_id edge);

/*
 * Queries — read-only, for rendering and for tools (push/pull, inference)
 * built on top of this module.
 */

bool apcad_mesh_vertex_exists(
    apcad_mesh mesh,
    apcad_vertex_id v);
ApriVec3 apcad_mesh_vertex_position(
    apcad_mesh mesh,
    apcad_vertex_id v);

/* Overwrites an existing vertex's position in place -- for a live drag
 * (Push/Pull, see apcaddraw.h) that needs to reposition the same vertex
 * every frame rather than weld a fresh one each time, which would leave a
 * trail of orphaned vertices behind. Does NOT re-run auto-face detection
 * or re-validate planarity/convexity of faces this vertex already belongs
 * to -- moving a vertex can leave an existing face non-planar with no
 * warning. Keeping any affected loop planar is the caller's job (e.g.
 * apcad_pushpull_tool_update only ever moves a vertex along a fixed
 * normal shared by its whole cap loop, which by construction keeps that
 * loop planar). */
void apcad_mesh_move_vertex(
    apcad_mesh mesh,
    apcad_vertex_id v,
    ApriVec3 new_position);

bool apcad_mesh_edge_exists(
    apcad_mesh mesh,
    apcad_edge_id e);
void apcad_mesh_edge_vertices(
    apcad_mesh mesh,
    apcad_edge_id e,
    apcad_vertex_id *out_a,
    apcad_vertex_id *out_b);

/* Faces this edge currently bounds -- 0, 1 (open shell boundary), or 2
 * (interior edge shared by two faces, e.g. an edge inside a closed solid). */
uint32_t apcad_mesh_edge_face_count(
    apcad_mesh mesh,
    apcad_edge_id e);
apcad_face_id apcad_mesh_edge_face(
    apcad_mesh mesh,
    apcad_edge_id e,
    uint32_t index);

bool apcad_mesh_face_exists(
    apcad_mesh mesh,
    apcad_face_id f);
uint32_t apcad_mesh_face_edge_count(
    apcad_mesh mesh,
    apcad_face_id f);
apcad_edge_id apcad_mesh_face_edge(
    apcad_mesh mesh,
    apcad_face_id f,
    uint32_t index);
/* The face's ordered vertex loop -- loop_vertex(f,i) to
 * loop_vertex(f,(i+1)%count) is the edge loop_edge(f,i). Exposed
 * separately from face_edge because anything wanting actual triangle
 * positions (tessellation, raycasting, push/pull) needs vertices directly
 * rather than resolving them back out of edges itself every time. */
apcad_vertex_id apcad_mesh_face_vertex(
    apcad_mesh mesh,
    apcad_face_id f,
    uint32_t index);
ApriVec3 apcad_mesh_face_normal(
    apcad_mesh mesh,
    apcad_face_id f);

/* Stable iteration for rendering/tessellation -- ids may have gaps (erased
 * elements leave holes), so iterate 0..count and skip ids that fail the
 * corresponding *_exists check, rather than assuming a dense range. */
uint32_t apcad_mesh_vertex_id_range(apcad_mesh mesh); /* one past the highest vertex id ever issued */
uint32_t apcad_mesh_edge_id_range(apcad_mesh mesh);
uint32_t apcad_mesh_face_id_range(apcad_mesh mesh);

/* Triangulates every non-construction face (flat-shaded, vertices
 * duplicated per face, same contract as apcad_solid_tessellate) and pushes
 * the result into `mesh_out` via aprend_mesh_set_vertex_bindings/
 * set_vertex_binding_data/set_indices/upload. */
void apcad_mesh_tessellate(
    apcad_mesh mesh,
    aprend_mesh mesh_out);

/* Emits every edge (including construction edges, flagged via
 * `out_is_construction` in parallel arrays so the host can style them
 * differently -- e.g. dashed) as a line-list, positions only. SketchUp's
 * signature look is edges always visible on top of shaded faces; this is
 * how the host gets the data to draw that pass. */
void apcad_mesh_tessellate_wireframe(
    apcad_mesh mesh,
    aprend_mesh mesh_out,
    bool include_construction);

#ifdef __cplusplus
}
#endif

#endif // APCAD_MESH_H
