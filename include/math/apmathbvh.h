/*
 * Built in src/math/apmathbvh.cpp, with headless tests in
 * tests/apmathbvh_tests.c. apricotfields.h doesn't include it yet.
 */

#ifndef APMATHBVH_H
#define APMATHBVH_H

#include "../apricore.h"
#include "../aprimath.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/*
 * ApricotMath's ray casting - which triangle a ray hits first, among many
 * meshes, in double precision. Two levels of bounding volume hierarchy
 * (BVH), so a cast costs about the logarithm of the triangle count and of
 * the mesh count rather than their sum:
 *
 *   mesh BVH      over one mesh's triangles. Built once per mesh.
 *   instance BVH  over the bounds of many instances - each a mesh BVH
 *                 placed by a transform, with an id and a mask that are the
 *                 caller's. Built once per set of instances; a mesh used a
 *                 thousand times is built once and instanced.
 *
 * Both are immutable once created: a change is a new BVH. Rebuilding an
 * instance BVH doesn't rebuild its meshes, so changing one mesh among many
 * costs that mesh's build plus a build over boxes.
 *
 * General purpose: it knows nothing about what a mesh is or why a ray is
 * cast. What an instance means is the caller's, through its id; which
 * instances a cast considers is the caller's, through masks and a filter
 * callback.
 *
 * Out of scope: curved primitives, motion, refitting a BVH in place, and
 * anything GPU - this is CPU geometry, unrelated to Aprend.
 */

/* ---- Conventions ----------------------------------------------------- */

/*
 * Threads. Creating and destroying are safe from any thread. Casts are safe
 * from any number of threads at once, on the same BVH, and never block or
 * allocate. Destroying a BVH while a cast on it is running isn't safe. A filter callback runs on
 * the thread that called the cast.
 *
 * Results. Calls return APRESULT (apricore.h). The general codes mean, here:
 *   APRESULT_INVALID_ARGUMENT  also a position or transform that isn't
 *                              finite, an index past the positions, a
 *                              count that isn't a multiple of three, a
 *                              transform that can't be inverted or whose
 *                              last row isn't 0, 0, 0, 1
 *   APRESULT_OUT_OF_MEMORY     only when creating
 * Casts can't fail: they return whether, or how many times, the ray hit. A
 * cast given a NULL BVH, a NULL ray or a ray that isn't finite hits
 * nothing.
 *
 * Struct versions. Descs start with [struct_size], set to sizeof(the
 * struct) as compiled; fields past it read as zero, as in aparchive.h.
 * APMATH_AABB, APMATH_RAY, APMATH_BVH_INSTANCE and the hit structs are
 * frozen: they never grow (a new need gets a new type).
 *
 * Geometry. Right-handed coordinates; a triangle's front is the side its
 * points run counter-clockwise from. A triangle with no area is never hit.
 * Rays hit both sides of a triangle.
 */

/* ---- Rays and boxes -------------------------------------------------- */

/* An axis-aligned box. Empty if any [lo] is above its [hi]. */
typedef struct APMATH_AABB {
	ApriDVec3 lo;
	ApriDVec3 hi;
} APMATH_AABB;

/* The points [origin] + t * [direction] for t from [t_min] to [t_max],
 * both included. [direction] needn't be unit length: t counts in its
 * lengths, so with a unit direction it's a distance. [t_max] may be
 * INFINITY for a ray with no far end; [t_min] may be negative. A zero
 * [direction], or a [t_min] above [t_max], hits nothing. */
typedef struct APMATH_RAY {
	ApriDVec3 origin;
	ApriDVec3 direction;
	double t_min;
	double t_max;
} APMATH_RAY;

/* Narrows [ray]'s [t_min] and [t_max] to the part of it inside [box], and
 * returns true - or returns false, leaving [ray] as it was, if none of it
 * is. A box side may be infinite: a slab between two planes is a box
 * infinite along the other two axes. */
bool apmath_ray_clip_aabb(APMATH_RAY *ray, const APMATH_AABB *box);

/* ---- Mesh BVH -------------------------------------------------------- */

typedef struct apmath_mesh_bvh_t *apmath_mesh_bvh;

typedef struct APMATH_MESH_BVH_DESC {
	uint32_t struct_size;
	/* The mesh's points, in its own coordinates. Copied: the array needn't
	 * outlive the call. */
	const ApriDVec3 *positions;
	uint32_t position_count;
	/* Three per triangle, each a position's index. NULL for a mesh with no
	 * index list: then every three positions are a triangle, in order, and
	 * [position_count] must be a multiple of three. */
	const uint32_t *indices;
	uint32_t index_count; /* a multiple of three; 0 when [indices] is NULL */
} APMATH_MESH_BVH_DESC;

/* A mesh with no triangles is valid, and never hit. [out_bvh] is NULL on
 * failure. */
APRESULT apmath_mesh_bvh_create(const APMATH_MESH_BVH_DESC *desc, apmath_mesh_bvh *out_bvh);
/* Destroy a mesh only after every instance BVH holding it. NULL is fine. */
void apmath_mesh_bvh_destroy(apmath_mesh_bvh bvh);

uint32_t apmath_mesh_bvh_get_triangle_count(apmath_mesh_bvh bvh);
/* Empty for a mesh with no triangles. */
APMATH_AABB apmath_mesh_bvh_get_bounds(apmath_mesh_bvh bvh);

/* Where a ray hit a mesh, in the mesh's coordinates. */
typedef struct APMATH_MESH_HIT {
	/* The triangle's position in the desc's order - never the BVH's own. */
	uint32_t triangle;
	/* True if the ray hit the triangle's back - e.g. leaving a closed
	 * solid it started inside. */
	bool back_face;
	double t;
	ApriDVec3 point;
	/* Unit, out of the triangle's front - whichever side was hit. */
	ApriDVec3 normal;
} APMATH_MESH_HIT;

/* The hit with the smallest t. Of triangles hit at the same t, the one
 * earliest in the desc's order - so a cast is repeatable. [out_hit] may be
 * NULL, to ask only whether the ray hits; it's left alone on a miss. */
bool apmath_mesh_bvh_cast(apmath_mesh_bvh bvh, const APMATH_RAY *ray, APMATH_MESH_HIT *out_hit);

/* ---- Instance BVH ---------------------------------------------------- */

typedef struct apmath_instance_bvh_t *apmath_instance_bvh;

/* One placement of a mesh. Frozen. */
typedef struct APMATH_BVH_INSTANCE {
	/* Not owned, and not copied: it must outlive the instance BVH. NULL is
	 * an instance that's never hit. */
	apmath_mesh_bvh mesh;
	/* Mesh coordinates to the instance BVH's: any transform that can be
	 * inverted and keeps straight lines straight and parallel ones
	 * parallel (rotation, translation, scale, shear, mirror) - its last row
	 * is 0, 0, 0, 1. Copied. NULL places the mesh as it is. */
	const ApriDMat4 *transform;
	/* The caller's, handed back in hits and to the filter. Needn't be
	 * unique. */
	uint64_t id;
	/* Which kinds of cast consider the instance - see APMATH_CAST_DESC. An
	 * instance with no bits set is only hit by casts that pass no desc. */
	uint32_t mask;
} APMATH_BVH_INSTANCE;

typedef struct APMATH_INSTANCE_BVH_DESC {
	uint32_t struct_size;
	const APMATH_BVH_INSTANCE *instances; /* copied */
	uint32_t instance_count;
} APMATH_INSTANCE_BVH_DESC;

/* No instances is valid, and never hit. [out_bvh] is NULL on failure. */
APRESULT apmath_instance_bvh_create(const APMATH_INSTANCE_BVH_DESC *desc, apmath_instance_bvh *out_bvh);
/* Leaves the meshes alone. NULL is fine. */
void apmath_instance_bvh_destroy(apmath_instance_bvh bvh);

uint32_t apmath_instance_bvh_get_instance_count(apmath_instance_bvh bvh);
/* Around every instance; empty if there are none with triangles. */
APMATH_AABB apmath_instance_bvh_get_bounds(apmath_instance_bvh bvh);

/* True to let the ray hit the instance, false to pass through it as if it
 * weren't there. [instance] is its position in the desc's order. Called
 * for instances whose bounds the ray enters and whose mask matched, at most
 * once per instance per cast, in no particular order; it must not create
 * or destroy BVHs. */
typedef bool (*APMATH_CAST_FILTER)(void *user, uint32_t instance, uint64_t id);

/* Which instances a cast considers. A NULL desc considers them all. */
typedef struct APMATH_CAST_DESC {
	uint32_t struct_size;
	/* Only instances sharing a bit with [mask] are considered. */
	uint32_t mask;
	/* Asked about each of those the ray might hit. NULL lets them all
	 * through. */
	APMATH_CAST_FILTER filter;
	void *user;
} APMATH_CAST_DESC;

/* Where a ray hit an instance, in the instance BVH's coordinates. */
typedef struct APMATH_INSTANCE_HIT {
	uint32_t instance; /* its position in the desc's order */
	uint64_t id;
	/* As APMATH_MESH_HIT's. A triangle's front is the mesh's, wherever the
	 * transform puts it - so a closed solid's outside stays its outside
	 * when an instance mirrors it. */
	uint32_t triangle;
	bool back_face;
	double t;
	ApriDVec3 point;
	ApriDVec3 normal;
} APMATH_INSTANCE_HIT;

/* The hit with the smallest t among the instances [desc] lets through. Of
 * instances hit at the same t, the one earliest in the desc's order; of
 * triangles, likewise - so a cast is repeatable. [out_hit] may be NULL; it's
 * left alone on a miss. */
bool apmath_instance_bvh_cast(apmath_instance_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                              APMATH_INSTANCE_HIT *out_hit);

/* Every instance the ray hits, one hit each - the instance's own nearest -
 * ordered as apmath_instance_bvh_cast() would choose between them. Writes
 * the first [hit_capacity] of them and returns how many instances the ray
 * hits, so a call with a NULL [out_hits] asks how big a buffer to pass. */
uint32_t apmath_instance_bvh_cast_all(apmath_instance_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                                      APMATH_INSTANCE_HIT *out_hits, uint32_t hit_capacity);

#if __cplusplus
}
#endif

#endif // APMATHBVH_H
