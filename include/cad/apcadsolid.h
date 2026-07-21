
#ifndef APCAD_SOLID_H
#define APCAD_SOLID_H

#include "aprimath.h"
#include "render/aprendscene.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Solid  —  placeholder geometry kernel
 *
 * A light face-vertex representation: each solid is a list of planar,
 * convex, CCW-wound (as seen from outside the solid) polygonal faces. No
 * edge/vertex topology, no booleans, no curved surfaces — this exists to get
 * real objects on screen and let the rest of ApCAD take shape while a real
 * geometric kernel is still on the roadmap. Expect every function here to be
 * replaced wholesale once that kernel is imported; nothing outside this file
 * should assume a `apcad_solid` is anything more than "some faces".
 *
 * World convention: Y-up, matching Aprend/Erethal. All primitives below are
 * centered on the local origin except apcad_extrude_create, which builds
 * from y=0 upward — extruding a footprint sketched in the XZ plane along +Y
 * is the common case (walls, columns, floor slabs).
 */

typedef struct apcad_solid_t *apcad_solid;

apcad_solid apcad_box_create(float width, float height, float depth);
apcad_solid apcad_cylinder_create(float radius, float height, uint32_t segments);
apcad_solid apcad_sphere_create(float radius, uint32_t segments, uint32_t rings);

/* `polygon` is a simple convex polygon in the XZ plane (ApriVec2.x -> world
 * x, ApriVec2.y -> world z), wound CCW as seen from above (+Y looking down).
 * Extrudes from y=0 to y=height. */
apcad_solid apcad_extrude_create(const ApriVec2 *polygon, uint32_t point_count, float height);

void apcad_solid_destroy(apcad_solid solid);

/* Triangulates every face (flat-shaded — one face normal, vertices
 * duplicated per face) and pushes the result into `mesh` via
 * aprend_mesh_set_vertex_bindings / set_vertex_binding_data / set_indices /
 * upload. Vertex layout matches APREND_DEFAULT_VERTEX (position, normal,
 * uv, color); uv is always {0,0} and color always {1,1,1,1} since this
 * kernel has no notion of texturing — give the geometry a real material if
 * you need either. Caller owns `mesh`'s lifetime; it must already exist
 * (aprend_mesh_create) before calling this. */
void apcad_solid_tessellate(apcad_solid solid, aprend_mesh mesh);

#ifdef __cplusplus
}
#endif

#endif // APCAD_SOLID_H
