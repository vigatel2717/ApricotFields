
#ifndef APCAD_ENTITY_H
#define APCAD_ENTITY_H

#include "aprimath.h"
#include "cad/apcadmesh.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Entity  —  a placed reference to another apcad_mesh (SketchUp-style
 * component instancing), stored as a fourth element kind alongside an
 * apcad_mesh's vertices/edges/faces.
 *
 * DRAFT for review — not yet implemented, not wired into CMakeLists.
 *
 * This is the "second entity type" project_apricot's notes on apcad_entity
 * were waiting for, but it is NOT the wrapper originally sketched there
 * (solid + aprend_geometry + aprend_mesh + a geometry->entity lookup for
 * picking). Working through how SketchUp itself is built changed the
 * shape: SketchUp splits placement into ComponentDefinition (owns geometry,
 * lives independent of any placement) and ComponentInstance (just a
 * transform + a pointer to a definition, sitting as a peer element inside
 * whatever Entities collection it was drawn into). A Group is not a third
 * data type — it is the degenerate case of exactly one instance referencing
 * a definition nothing else points at. This file follows that split
 * directly: an apcad_mesh IS a definition (no separate wrapper type needed,
 * since apcad_mesh is already the shareable, self-contained geometry
 * container), and apcad_entity is the instance — a transform plus a
 * reference to another apcad_mesh, held as a peer element of the mesh it
 * was placed into.
 *
 * Why a peer element of apcad_mesh rather than a new top-level type:
 * SketchUp's Entities collection holds raw geometry and instances side by
 * side — click either one, select either one, erase either one, through
 * the same collection. Vertex/edge/face already live in apcad_mesh as
 * separate tombstoned id-indexed arrays (see apcadmesh.h); apcad_entity
 * follows that exact same shape as a fourth array, rather than inventing a
 * unified "generic Entity" storage type. The peer-ness is about a shared
 * *outward* interface (exists/erase/iterate), not shared storage — the
 * same distinction real SketchUp draws between Sketchup::Entity as a
 * common base class and each subtype's own specialized internal layout.
 *
 * Why no material/color override field: ApCAD stays domain- and
 * rendering-agnostic (see project_apricot notes on ApricotBIM staying
 * separate from ApCAD for the same reason) — Erethal already drives
 * hover/select color externally via aprend_material_set_color rather than
 * storing it on apcad_mesh, and an instance-level override would be the
 * same kind of host/render concern. If per-instance appearance overrides
 * are needed later, they belong wherever aprend_material already lives,
 * keyed by apcad_entity_id, not as a field on this struct.
 */

typedef uint32_t apcad_entity_id;

/* Places `definition` inside `mesh` at `transform`. `definition` must be a
 * different apcad_mesh than `mesh` and must not (transitively, through its
 * own entities) reference `mesh` — either case would make a cycle, which
 * would recurse forever the moment anything walks entities (raycast,
 * tessellation-with-instancing, etc.). Returns APCAD_INVALID_ID if
 * `definition == mesh` or if a cycle would result, otherwise the id of the
 * new instance.
 *
 * `mesh` does NOT take ownership of `definition` -- see apcad_mesh_destroy
 * below. Multiple entities, in the same mesh or different meshes, may
 * reference the same definition; that sharing is the entire point (edit
 * the definition once, every instance updates), same as SketchUp. */
apcad_entity_id apcad_mesh_add_entity(
    apcad_mesh mesh,
    apcad_mesh definition,
    ApriMat4 transform);

/* Removes the instance -- `definition` itself is untouched and keeps
 * existing even if this was its last referencing entity anywhere (same as
 * deleting the last placed copy of a SketchUp component: the definition
 * still exists in the model, just unplaced). Whose job it is to eventually
 * free an unreferenced definition is not decided here -- see "Deliberately
 * out of scope" below. */
void apcad_mesh_erase_entity(
    apcad_mesh mesh,
    apcad_entity_id entity);

bool apcad_mesh_entity_exists(
    apcad_mesh mesh,
    apcad_entity_id entity);

apcad_mesh apcad_mesh_entity_definition(
    apcad_mesh mesh,
    apcad_entity_id entity);

ApriMat4 apcad_mesh_entity_transform(
    apcad_mesh mesh,
    apcad_entity_id entity);

/* Overwrites an existing entity's transform in place -- the instance
 * equivalent of apcad_mesh_move_vertex. Unlike moving a vertex, this
 * touches no geometry at all: the definition's vertex/edge/face data is
 * completely untouched, so a Move tool built on this needs no
 * re-tessellation/re-upload of anything, sidestepping the per-frame
 * GPU-idle retessellation stall apcaddraw.h's Push/Pull already has to pay
 * (see project_apricot's Line/Push-Pull entry) -- see "Consequences for
 * rendering" below for how a host is expected to apply this cheaply. */
void apcad_mesh_set_entity_transform(
    apcad_mesh mesh,
    apcad_entity_id entity,
    ApriMat4 transform);

/* Stable iteration, same contract as apcad_mesh_vertex_id_range/
 * edge_id_range/face_id_range -- ids may have gaps, iterate 0..range and
 * skip ids that fail apcad_mesh_entity_exists. */
uint32_t apcad_mesh_entity_id_range(apcad_mesh mesh);

/*
 * Deliberately out of scope for this file -- open decisions, not silent
 * ones:
 *
 * - Definition ownership/lifetime. Nothing here says who frees a
 *   definition mesh, or what "the set of all definitions in a model" is.
 *   That is the apcad_model/apcad_document container project_apricot
 *   flagged as worth deferring until a second entity type existed -- this
 *   file IS that second entity type, so apcad_model is the natural next
 *   thing to design, not something to half-solve here by guessing at
 *   ownership rules this file has no business making. Until it exists,
 *   the caller (Erethal) owns every apcad_mesh it creates, definitions
 *   included, and must not destroy a definition while any entity anywhere
 *   still references it -- a documented caller contract, not a runtime
 *   check, consistent with how apcad_mesh_move_vertex already trusts its
 *   caller rather than re-validating on every call.
 *
 * - Recursive raycast / hit-testing into placed entities. apcad_mesh_raycast
 *   (apcadraycast.h) still only tests one mesh's own faces, unchanged --
 *   it does not walk entities. Testing a whole nested scene is exactly the
 *   scene-level traversal feedback_picking_architecture already named as
 *   the next step once more than one entity exists ("a BVH at the
 *   entity/scene level"), not something to bolt onto apcad_mesh_raycast's
 *   existing single-mesh contract. The primitive that traversal will need
 *   from THIS file is exactly apcad_mesh_entity_transform, since
 *   apcad_ray_to_local/apcad_local_point_to_world (apcadraycast.h) already
 *   take an ApriMat4 and convert a world ray into (or out of) whatever
 *   local space that matrix describes -- a recursive raycast is "call
 *   apcad_ray_to_local with this entity's transform, recurse into its
 *   definition." That walk is not written here on purpose.
 *
 * - Recursive tessellation / instanced rendering. apcad_mesh_tessellate
 *   stays scoped to one mesh's own faces, unchanged -- it has no idea an
 *   entity array exists. Flattening every instance's geometry into one
 *   giant buffer on the CPU would silently throw away the one thing
 *   instancing is for (tessellate a definition once, no matter how many
 *   times it's placed). The recommended host-side pattern, given Aprend
 *   already has a node/world-transform concept
 *   (aprend_node_get_world_transform, aprendscene.h): tessellate each
 *   distinct definition mesh exactly once into its own aprend_mesh, then
 *   for each apcad_entity referencing it, set an aprend_node's transform
 *   from apcad_mesh_entity_transform and issue one draw of the shared mesh
 *   under that node -- real reuse at the render layer, matching how
 *   apcad_mesh_set_entity_transform above is deliberately cheap (no
 *   retessellation) specifically so this path stays cheap too.
 *
 * - Editing context (SketchUp's active_path). Which mesh apcaddraw.h's
 *   tools currently operate on is still a single fixed apcad_mesh chosen
 *   by the host (Erethal's app.cad_mesh) -- there is no notion here of
 *   "double-click to edit this entity's definition, lock everything
 *   else." That is host/UI policy (which mesh is "currently open" is a
 *   stack the host maintains), not a data-layer concern this file should
 *   decide, and it only becomes decidable once apcad_model exists to
 *   define what "everything else" even is.
 */

#ifdef __cplusplus
}
#endif

#endif // APCAD_ENTITY_H
