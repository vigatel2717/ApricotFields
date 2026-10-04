// Headless tests for ApricotMath's ray casting (include/math/apmathbvh.h).
//
// Written in C on purpose: apmathbvh.h is a C front end, and this proves it
// compiles and links as one.

#include "math/apmathbvh.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static const char *current_test = "";

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool near(double a, double b)
{
    return fabs(a - b) <= 1e-9 * (1.0 + fabs(a) + fabs(b));
}

static bool near3(ApriDVec3 v, double x, double y, double z)
{
    return near(v.x, x) && near(v.y, y) && near(v.z, z);
}

static ApriDVec3 vec3(double x, double y, double z)
{
    ApriDVec3 v = {x, y, z};
    return v;
}

static APMATH_RAY make_ray(double ox, double oy, double oz, double dx, double dy, double dz)
{
    APMATH_RAY ray;
    ray.origin = vec3(ox, oy, oz);
    ray.direction = vec3(dx, dy, dz);
    ray.t_min = 0.0;
    ray.t_max = INFINITY;
    return ray;
}

// Repeatable random numbers in [0, 1).
static uint64_t random_state = 0x9E3779B97F4A7C15ull;
static double random_unit(void)
{
    random_state = random_state * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(random_state >> 11) / 9007199254740992.0;
}
static double random_range(double lo, double hi)
{
    return lo + (hi - lo) * random_unit();
}

// The unit cube from (0, 0, 0) to (1, 1, 1): 8 corners, 12 triangles
// facing out, two per face in the order -X, +X, -Y, +Y, -Z, +Z.
static const ApriDVec3 cube_positions[8] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                            {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
static const uint32_t cube_indices[36] = {0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5, 0, 1, 5, 0, 5, 4,
                                          2, 6, 7, 2, 7, 3, 0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6};

static apmath_mesh_bvh make_cube(void)
{
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.positions = cube_positions;
    desc.position_count = 8;
    desc.indices = cube_indices;
    desc.index_count = 36;
    apmath_mesh_bvh bvh = NULL;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
    return bvh;
}

static void test_ray_clip(void)
{
    current_test = "ray_clip";
    APMATH_AABB box = {{0, 0, 0}, {1, 1, 1}};
    APMATH_RAY ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(apmath_ray_clip_aabb(&ray, &box));
    CHECK(near(ray.t_min, 2.0) && near(ray.t_max, 3.0));
    // Starting inside keeps its start.
    ray = make_ray(0.25, 0.5, 0.5, 1, 0, 0);
    CHECK(apmath_ray_clip_aabb(&ray, &box));
    CHECK(near(ray.t_min, 0.0) && near(ray.t_max, 0.75));
    // A miss leaves the ray alone.
    ray = make_ray(-2, 5, 0.5, 1, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &box));
    CHECK(ray.t_min == 0.0 && ray.t_max == INFINITY);
    // Pointing away.
    ray = make_ray(-2, 0.5, 0.5, -1, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &box));
    // A slab between two planes: infinite along the other axes.
    APMATH_AABB slab = {{-INFINITY, -INFINITY, 1}, {INFINITY, INFINITY, 3}};
    ray = make_ray(100, -50, 10, 0, 0, -2);
    CHECK(apmath_ray_clip_aabb(&ray, &slab));
    CHECK(near(ray.t_min, 3.5) && near(ray.t_max, 4.5));
    // Along the slab, inside it and outside it.
    ray = make_ray(0, 0, 2, 1, 0, 0);
    CHECK(apmath_ray_clip_aabb(&ray, &slab));
    CHECK(ray.t_min == 0.0 && ray.t_max == INFINITY);
    ray = make_ray(0, 0, 5, 1, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &slab));
    // Nothing to clip.
    APMATH_AABB empty = {{1, 1, 1}, {0, 0, 0}};
    ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &empty));
    CHECK(!apmath_ray_clip_aabb(NULL, &box));
    CHECK(!apmath_ray_clip_aabb(&ray, NULL));
    ray = make_ray(0.5, 0.5, 0.5, 0, 0, 0);
    CHECK(!apmath_ray_clip_aabb(&ray, &box));
}

static void test_mesh_arguments(void)
{
    current_test = "mesh_arguments";
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    apmath_mesh_bvh bvh = (apmath_mesh_bvh)1;
    CHECK(apmath_mesh_bvh_create(NULL, &bvh) == APRESULT_INVALID_ARGUMENT);
    CHECK(bvh == NULL);
    CHECK(apmath_mesh_bvh_create(&desc, NULL) == APRESULT_INVALID_ARGUMENT);

    // No triangles: valid, never hit.
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
    CHECK(apmath_mesh_bvh_get_triangle_count(bvh) == 0);
    APMATH_AABB bounds = apmath_mesh_bvh_get_bounds(bvh);
    CHECK(bounds.lo.x > bounds.hi.x);
    APMATH_RAY ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(bvh, &ray, NULL));
    apmath_mesh_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(NULL);

    desc.positions = cube_positions;
    desc.position_count = 8; // not a multiple of three, with no indices
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    desc.indices = cube_indices;
    desc.index_count = 35;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    uint32_t past[3] = {0, 1, 8};
    desc.indices = past;
    desc.index_count = 3;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    ApriDVec3 not_finite[3] = {{0, 0, 0}, {1, 0, 0}, {0, NAN, 0}};
    desc.positions = not_finite;
    desc.position_count = 3;
    desc.indices = NULL;
    desc.index_count = 0;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    desc.struct_size = 4;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    CHECK(bvh == NULL);
}

static void test_mesh_cube(void)
{
    current_test = "mesh_cube";
    apmath_mesh_bvh cube = make_cube();
    CHECK(apmath_mesh_bvh_get_triangle_count(cube) == 12);
    APMATH_AABB bounds = apmath_mesh_bvh_get_bounds(cube);
    CHECK(near3(bounds.lo, 0, 0, 0) && near3(bounds.hi, 1, 1, 1));

    // From outside: the -X face, from its front.
    APMATH_RAY ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    APMATH_MESH_HIT hit;
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 2.0) && !hit.back_face && hit.triangle < 2);
    CHECK(near3(hit.point, 0, 0.25, 0.5) && near3(hit.normal, -1, 0, 0));
    CHECK(apmath_mesh_bvh_cast(cube, &ray, NULL));

    // From inside: the +X face, from its back; the normal still points out.
    ray = make_ray(0.5, 0.25, 0.5, 1, 0, 0);
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 0.5) && hit.back_face && (hit.triangle == 2 || hit.triangle == 3));
    CHECK(near3(hit.normal, 1, 0, 0));

    // A direction that isn't unit length: t counts in its lengths.
    ray = make_ray(-2, 0.25, 0.5, 4, 0, 0);
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 0.5) && near3(hit.point, 0, 0.25, 0.5));

    // The range: starting past the near face, ending before any, and a
    // negative start.
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    ray.t_min = 2.5;
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 3.0) && hit.back_face);
    ray.t_min = 0.0;
    ray.t_max = 1.5;
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(3, 0.25, 0.5, 1, 0, 0);
    ray.t_min = -10.0;
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, -3.0));

    // Misses, and rays that can't hit anything.
    ray = make_ray(-2, 5, 0.5, 1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(-2, 0.25, 0.5, -1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(-2, 0.25, 0.5, 0, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(NAN, 0.25, 0.5, 1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    ray.t_min = 5.0;
    ray.t_max = 1.0;
    CHECK(!apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(!apmath_mesh_bvh_cast(cube, NULL, &hit));
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    CHECK(!apmath_mesh_bvh_cast(NULL, &ray, &hit));

    // Along a face's diagonal, where its two triangles meet: no crack.
    ray = make_ray(0.5, 0.5, 5, 0, 0, -1);
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 4.0) && near3(hit.normal, 0, 0, 1));
    // And through a corner of the cube.
    ray = make_ray(2, 2, 2, -1, -1, -1);
    CHECK(apmath_mesh_bvh_cast(cube, &ray, &hit));
    CHECK(near(hit.t, 1.0));
    apmath_mesh_bvh_destroy(cube);
}

static void test_mesh_ties_and_degenerates(void)
{
    current_test = "mesh_ties_and_degenerates";
    // 0: no area. 1 and 2: the same triangle twice. 3: further away.
    ApriDVec3 positions[12] = {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1},
                               {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {0, 0, 2}, {1, 0, 2}, {0, 1, 2}};
    APMATH_MESH_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.positions = positions;
    desc.position_count = 12;
    apmath_mesh_bvh bvh = NULL;
    CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
    CHECK(apmath_mesh_bvh_get_triangle_count(bvh) == 4);
    APMATH_MESH_HIT hit;
    APMATH_RAY ray = make_ray(0.25, 0.25, 0, 0, 0, 1);
    CHECK(apmath_mesh_bvh_cast(bvh, &ray, &hit));
    CHECK(hit.triangle == 1 && near(hit.t, 1.0) && hit.back_face);
    // From the other end, the far one is nearest; then the tie again.
    ray = make_ray(0.25, 0.25, 5, 0, 0, -1);
    CHECK(apmath_mesh_bvh_cast(bvh, &ray, &hit));
    CHECK(hit.triangle == 3 && near(hit.t, 3.0) && !hit.back_face);
    ray.t_min = 3.5;
    CHECK(apmath_mesh_bvh_cast(bvh, &ray, &hit));
    CHECK(hit.triangle == 1 && near(hit.t, 4.0));
    // The one with no area is never hit, even along it.
    ray = make_ray(0.5, 0, -1, 0, 0, 1);
    ray.t_max = 1.5;
    CHECK(!apmath_mesh_bvh_cast(bvh, &ray, &hit));
    apmath_mesh_bvh_destroy(bvh);
}

// Moller-Trumbore, written apart from the library's. True with [out_t] if
// the ray hits the triangle at or after t = 0.
static bool brute_triangle(const ApriDVec3 *p, const APMATH_RAY *ray, double *out_t)
{
    double e1[3] = {p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
    double e2[3] = {p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
    double d[3] = {ray->direction.x, ray->direction.y, ray->direction.z};
    double s[3] = {ray->origin.x - p[0].x, ray->origin.y - p[0].y, ray->origin.z - p[0].z};
    double h[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    double det = e1[0] * h[0] + e1[1] * h[1] + e1[2] * h[2];
    if (det == 0.0)
        return false;
    double u = (s[0] * h[0] + s[1] * h[1] + s[2] * h[2]) / det;
    double q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
    if (u < 0.0 || v < 0.0 || u + v > 1.0)
        return false;
    double t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
    if (t < 0.0)
        return false;
    *out_t = t;
    return true;
}

static APMATH_RAY random_ray(double reach)
{
    APMATH_RAY ray = make_ray(random_range(-reach, reach), random_range(-reach, reach), random_range(-reach, reach),
                              random_range(-1, 1), random_range(-1, 1), random_range(-1, 1));
    if (ray.direction.x == 0.0 && ray.direction.y == 0.0 && ray.direction.z == 0.0)
        ray.direction.x = 1.0;
    return ray;
}

// A big mesh agrees with testing every triangle, whatever shape its
// triangles are in: scattered, or all in one spot.
static void test_mesh_against_brute_force(void)
{
    current_test = "mesh_against_brute_force";
    enum
    {
        TRIANGLES = 3000,
        RAYS = 3000
    };
    ApriDVec3 *positions = (ApriDVec3 *)malloc(sizeof(ApriDVec3) * TRIANGLES * 3);
    for (int shape = 0; shape < 2; shape++)
    {
        for (int i = 0; i < TRIANGLES; i++)
        {
            // Scattered small triangles, then large ones sharing a centre.
            double centre = shape == 0 ? 10.0 : 0.0, size = shape == 0 ? 0.6 : 8.0;
            ApriDVec3 c = vec3(random_range(-centre, centre), random_range(-centre, centre),
                               random_range(-centre, centre));
            for (int corner = 0; corner < 3; corner++)
                positions[i * 3 + corner] = vec3(c.x + random_range(-size, size), c.y + random_range(-size, size),
                                                 c.z + random_range(-size, size));
        }
        APMATH_MESH_BVH_DESC desc;
        memset(&desc, 0, sizeof(desc));
        desc.struct_size = sizeof(desc);
        desc.positions = positions;
        desc.position_count = TRIANGLES * 3;
        apmath_mesh_bvh bvh = NULL;
        CHECK(apmath_mesh_bvh_create(&desc, &bvh) == APRESULT_OK);
        int hits = 0, mismatches = 0;
        for (int i = 0; i < RAYS; i++)
        {
            APMATH_RAY ray = random_ray(12.0);
            bool expected = false;
            double expected_t = 0.0;
            uint32_t expected_triangle = 0;
            for (uint32_t triangle = 0; triangle < TRIANGLES; triangle++)
            {
                double t;
                if (brute_triangle(positions + triangle * 3, &ray, &t) && (!expected || t < expected_t))
                {
                    expected = true;
                    expected_t = t;
                    expected_triangle = triangle;
                }
            }
            APMATH_MESH_HIT hit;
            bool found = apmath_mesh_bvh_cast(bvh, &ray, &hit);
            if (found != expected || (found && (hit.triangle != expected_triangle || !near(hit.t, expected_t))))
                mismatches++;
            hits += found;
        }
        CHECK(mismatches == 0);
        CHECK(hits > RAYS / 20); // the rays do hit things
        apmath_mesh_bvh_destroy(bvh);
    }
    free(positions);
}

// Column-major: scale, then translate.
static ApriDMat4 placement(double sx, double sy, double sz, double tx, double ty, double tz)
{
    ApriDMat4 m;
    memset(&m, 0, sizeof(m));
    m.m[0] = sx;
    m.m[5] = sy;
    m.m[10] = sz;
    m.m[12] = tx;
    m.m[13] = ty;
    m.m[14] = tz;
    m.m[15] = 1.0;
    return m;
}

static apmath_instance_bvh make_instances(const APMATH_BVH_INSTANCE *instances, uint32_t count)
{
    APMATH_INSTANCE_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.instances = instances;
    desc.instance_count = count;
    apmath_instance_bvh bvh = NULL;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_OK);
    return bvh;
}

static void test_instance_arguments(void)
{
    current_test = "instance_arguments";
    apmath_mesh_bvh cube = make_cube();
    APMATH_INSTANCE_BVH_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    apmath_instance_bvh bvh = (apmath_instance_bvh)1;
    CHECK(apmath_instance_bvh_create(NULL, &bvh) == APRESULT_INVALID_ARGUMENT);
    CHECK(bvh == NULL);
    CHECK(apmath_instance_bvh_create(&desc, NULL) == APRESULT_INVALID_ARGUMENT);

    // No instances: valid, never hit.
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_OK);
    CHECK(apmath_instance_bvh_get_instance_count(bvh) == 0);
    APMATH_AABB bounds = apmath_instance_bvh_get_bounds(bvh);
    CHECK(bounds.lo.x > bounds.hi.x);
    APMATH_RAY ray = make_ray(-2, 0.5, 0.5, 1, 0, 0);
    CHECK(!apmath_instance_bvh_cast(bvh, &ray, NULL, NULL));
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, NULL, 0) == 0);
    apmath_instance_bvh_destroy(bvh);
    apmath_instance_bvh_destroy(NULL);

    // Transforms that can't place a mesh.
    APMATH_BVH_INSTANCE instance;
    memset(&instance, 0, sizeof(instance));
    instance.mesh = cube;
    desc.instances = &instance;
    desc.instance_count = 1;
    ApriDMat4 flat = placement(1, 0, 1, 0, 0, 0);
    instance.transform = &flat;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    ApriDMat4 projective = placement(1, 1, 1, 0, 0, 0);
    projective.m[3] = 0.5;
    instance.transform = &projective;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    ApriDMat4 not_finite = placement(1, 1, 1, INFINITY, 0, 0);
    instance.transform = &not_finite;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    CHECK(bvh == NULL);
    desc.instances = NULL;
    CHECK(apmath_instance_bvh_create(&desc, &bvh) == APRESULT_INVALID_ARGUMENT);
    apmath_mesh_bvh_destroy(cube);
}

static uint32_t filter_calls[8];
static bool filter_skip_one(void *user, uint32_t instance, uint64_t id)
{
    filter_calls[instance]++;
    return id != *(const uint64_t *)user;
}

static void test_instances(void)
{
    current_test = "instances";
    apmath_mesh_bvh cube = make_cube();
    // Four cubes in a row along X at 0, 3, 6 and 9, and one with no mesh.
    ApriDMat4 at3 = placement(1, 1, 1, 3, 0, 0), at6 = placement(1, 1, 1, 6, 0, 0), at9 = placement(1, 1, 1, 9, 0, 0);
    APMATH_BVH_INSTANCE instances[5];
    memset(instances, 0, sizeof(instances));
    const ApriDMat4 *transforms[5] = {NULL, &at3, &at6, &at9, &at3};
    for (int i = 0; i < 5; i++)
    {
        instances[i].mesh = i == 4 ? NULL : cube;
        instances[i].transform = transforms[i];
        instances[i].id = 100 + (uint64_t)i;
        instances[i].mask = 1u << i;
    }
    instances[2].mask = 0; // only casts with no desc hit it
    apmath_instance_bvh bvh = make_instances(instances, 5);
    CHECK(apmath_instance_bvh_get_instance_count(bvh) == 5);
    APMATH_AABB bounds = apmath_instance_bvh_get_bounds(bvh);
    CHECK(near3(bounds.lo, 0, 0, 0) && near3(bounds.hi, 10, 1, 1));

    // From each end, the nearest.
    APMATH_INSTANCE_HIT hit;
    APMATH_RAY ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(hit.instance == 0 && hit.id == 100 && near(hit.t, 2.0) && !hit.back_face);
    CHECK(near3(hit.point, 0, 0.25, 0.5) && near3(hit.normal, -1, 0, 0));
    ray = make_ray(20, 0.25, 0.5, -1, 0, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(hit.instance == 3 && hit.id == 103 && near(hit.t, 10.0) && near3(hit.normal, 1, 0, 0));
    CHECK(near3(hit.point, 10, 0.25, 0.5));
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, NULL));
    ray = make_ray(-2, 5, 0.5, 1, 0, 0);
    CHECK(!apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));

    // All of them, in order, one hit each.
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    APMATH_INSTANCE_HIT hits[8];
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, NULL, 0) == 4);
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 8) == 4);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(hits[i].instance == i && near(hits[i].t, 2.0 + 3.0 * i) && near3(hits[i].normal, -1, 0, 0));
    // A buffer too small keeps the nearest.
    memset(hits, 0, sizeof(hits));
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 2) == 4);
    CHECK(hits[0].instance == 0 && hits[1].instance == 1 && hits[2].t == 0.0);
    ray = make_ray(20, 0.25, 0.5, -1, 0, 0);
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 2) == 4);
    CHECK(hits[0].instance == 3 && hits[1].instance == 2);
    // The range applies to each.
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    ray.t_min = 2.5;
    ray.t_max = 8.5;
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 8) == 3);
    CHECK(hits[0].instance == 0 && hits[0].back_face && near(hits[0].t, 3.0));
    CHECK(hits[1].instance == 1 && near(hits[1].t, 5.0) && hits[2].instance == 2 && near(hits[2].t, 8.0));

    // Masks: only instances sharing a bit, and never a maskless one.
    ray = make_ray(-2, 0.25, 0.5, 1, 0, 0);
    APMATH_CAST_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.mask = 1u << 3;
    CHECK(apmath_instance_bvh_cast(bvh, &ray, &desc, &hit));
    CHECK(hit.instance == 3);
    desc.mask = UINT32_MAX;
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, &desc, hits, 8) == 3);
    CHECK(hits[0].instance == 0 && hits[1].instance == 1 && hits[2].instance == 3);
    desc.mask = 0;
    CHECK(!apmath_instance_bvh_cast(bvh, &ray, &desc, &hit));
    desc.struct_size = 2;
    desc.mask = UINT32_MAX;
    CHECK(!apmath_instance_bvh_cast(bvh, &ray, &desc, &hit));

    // The filter: passed through as if it weren't there, asked once each.
    uint64_t skipped = 100;
    desc.struct_size = sizeof(desc);
    desc.filter = filter_skip_one;
    desc.user = &skipped;
    memset(filter_calls, 0, sizeof(filter_calls));
    CHECK(apmath_instance_bvh_cast(bvh, &ray, &desc, &hit));
    CHECK(hit.instance == 1 && near(hit.t, 5.0));
    CHECK(filter_calls[0] == 1 && filter_calls[1] == 1 && filter_calls[2] == 0 && filter_calls[4] == 0);
    memset(filter_calls, 0, sizeof(filter_calls));
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, &desc, hits, 8) == 2);
    CHECK(hits[0].instance == 1 && hits[1].instance == 3);
    CHECK(filter_calls[0] == 1 && filter_calls[1] == 1 && filter_calls[2] == 0 && filter_calls[3] == 1);

    CHECK(!apmath_instance_bvh_cast(NULL, &ray, NULL, &hit));
    CHECK(!apmath_instance_bvh_cast(bvh, NULL, NULL, &hit));
    CHECK(apmath_instance_bvh_cast_all(NULL, &ray, NULL, hits, 8) == 0);
    apmath_instance_bvh_destroy(bvh);

    // Two instances in the same place: the earlier one.
    APMATH_BVH_INSTANCE same[2];
    memset(same, 0, sizeof(same));
    same[0].mesh = same[1].mesh = cube;
    same[0].transform = same[1].transform = &at3;
    bvh = make_instances(same, 2);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(hit.instance == 0 && near(hit.t, 5.0));
    CHECK(apmath_instance_bvh_cast_all(bvh, &ray, NULL, hits, 8) == 2);
    CHECK(hits[0].instance == 0 && hits[1].instance == 1);
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(cube);
}

static void test_instance_transforms(void)
{
    current_test = "instance_transforms";
    apmath_mesh_bvh cube = make_cube();
    APMATH_BVH_INSTANCE instance;
    memset(&instance, 0, sizeof(instance));
    instance.mesh = cube;
    APMATH_INSTANCE_HIT hit;

    // Stretched 4 along X and moved: t stays the ray's own.
    ApriDMat4 stretched = placement(4, 1, 2, 10, 20, 30);
    instance.transform = &stretched;
    apmath_instance_bvh bvh = make_instances(&instance, 1);
    APMATH_AABB bounds = apmath_instance_bvh_get_bounds(bvh);
    CHECK(near3(bounds.lo, 10, 20, 30) && near3(bounds.hi, 14, 21, 32));
    APMATH_RAY ray = make_ray(12, 20.5, 40, 0, 0, -2);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(near(hit.t, 4.0) && near3(hit.point, 12, 20.5, 32) && near3(hit.normal, 0, 0, 1) && !hit.back_face);
    apmath_instance_bvh_destroy(bvh);

    // Mirrored in X: the cube's outside is still its outside.
    ApriDMat4 mirrored = placement(-1, 1, 1, 0, 0, 0);
    instance.transform = &mirrored;
    bvh = make_instances(&instance, 1);
    ray = make_ray(-5, 0.25, 0.5, 1, 0, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(near(hit.t, 4.0) && near3(hit.normal, -1, 0, 0) && !hit.back_face);
    ray = make_ray(-0.5, 0.25, 0.5, 1, 0, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(near(hit.t, 0.5) && near3(hit.normal, 1, 0, 0) && hit.back_face);
    apmath_instance_bvh_destroy(bvh);

    // Turned 45 degrees about Z and sheared: a face's normal follows it.
    double c = sqrt(0.5);
    ApriDMat4 turned = placement(1, 1, 1, 0, 0, 0);
    turned.m[0] = c;
    turned.m[1] = c;
    turned.m[4] = -c;
    turned.m[5] = c;
    instance.transform = &turned;
    bvh = make_instances(&instance, 1);
    // The cube's -Y face now faces (c, -c, 0), from the origin out along (c, c, 0).
    ray = make_ray(1, -1, 0.5, -1, 1, 0);
    CHECK(apmath_instance_bvh_cast(bvh, &ray, NULL, &hit));
    CHECK(near3(hit.normal, c, -c, 0) && !hit.back_face);
    CHECK(near(hit.point.x, hit.point.y) && near(hit.point.z, 0.5));
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(cube);
}

// Many placed cubes agree with casting each one on its own.
static void test_instances_against_brute_force(void)
{
    current_test = "instances_against_brute_force";
    enum
    {
        INSTANCES = 1500,
        RAYS = 2000
    };
    apmath_mesh_bvh cube = make_cube();
    ApriDMat4 *transforms = (ApriDMat4 *)malloc(sizeof(ApriDMat4) * INSTANCES);
    APMATH_BVH_INSTANCE *instances = (APMATH_BVH_INSTANCE *)calloc(INSTANCES, sizeof(APMATH_BVH_INSTANCE));
    apmath_instance_bvh *singles = (apmath_instance_bvh *)malloc(sizeof(apmath_instance_bvh) * INSTANCES);
    for (int i = 0; i < INSTANCES; i++)
    {
        double angle = random_range(0, 6.283185307179586), c = cos(angle), s = sin(angle);
        transforms[i] = placement(1, 1, random_range(0.5, 3), random_range(-40, 40), random_range(-40, 40),
                                  random_range(-40, 40));
        double scale = random_range(0.5, 3);
        transforms[i].m[0] = c * scale;
        transforms[i].m[1] = s * scale;
        transforms[i].m[4] = -s * scale;
        transforms[i].m[5] = c * scale;
        instances[i].mesh = cube;
        instances[i].transform = &transforms[i];
        instances[i].id = (uint64_t)i * 7;
        singles[i] = make_instances(&instances[i], 1);
    }
    apmath_instance_bvh bvh = make_instances(instances, INSTANCES);
    APMATH_INSTANCE_HIT *all = (APMATH_INSTANCE_HIT *)malloc(sizeof(APMATH_INSTANCE_HIT) * INSTANCES);
    int hits = 0, mismatches = 0, all_mismatches = 0;
    for (int i = 0; i < RAYS; i++)
    {
        APMATH_RAY ray = random_ray(45.0);
        bool expected = false;
        uint32_t expected_count = 0;
        APMATH_INSTANCE_HIT nearest;
        memset(&nearest, 0, sizeof(nearest));
        uint32_t nearest_instance = 0;
        for (uint32_t instance = 0; instance < INSTANCES; instance++)
        {
            APMATH_INSTANCE_HIT single;
            if (!apmath_instance_bvh_cast(singles[instance], &ray, NULL, &single))
                continue;
            expected_count++;
            if (!expected || single.t < nearest.t)
            {
                expected = true;
                nearest = single;
                nearest_instance = instance;
            }
        }
        APMATH_INSTANCE_HIT hit;
        bool found = apmath_instance_bvh_cast(bvh, &ray, NULL, &hit);
        if (found != expected || (found && (hit.instance != nearest_instance || hit.id != (uint64_t)nearest_instance * 7 ||
                                            hit.triangle != nearest.triangle || !near(hit.t, nearest.t))))
            mismatches++;
        hits += found;
        uint32_t count = apmath_instance_bvh_cast_all(bvh, &ray, NULL, all, INSTANCES);
        if (count != expected_count || (count && all[0].instance != nearest_instance))
            all_mismatches++;
        for (uint32_t k = 1; k < count; k++)
            if (all[k - 1].t > all[k].t)
                all_mismatches++;
    }
    CHECK(mismatches == 0);
    CHECK(all_mismatches == 0);
    CHECK(hits > RAYS / 20);
    for (int i = 0; i < INSTANCES; i++)
        apmath_instance_bvh_destroy(singles[i]);
    apmath_instance_bvh_destroy(bvh);
    apmath_mesh_bvh_destroy(cube);
    free(all);
    free(singles);
    free(instances);
    free(transforms);
}

int main(void)
{
    test_ray_clip();
    test_mesh_arguments();
    test_mesh_cube();
    test_mesh_ties_and_degenerates();
    test_mesh_against_brute_force();
    test_instance_arguments();
    test_instances();
    test_instance_transforms();
    test_instances_against_brute_force();
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("apmathbvh_tests: all passed\n");
    return 0;
}
