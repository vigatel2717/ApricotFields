#include "math/apmathbvh.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <glm/glm.hpp>
#include <limits>
#include <new>
#include <numeric>
#include <string.h>
#include <vector>

// Both levels are the same tree over boxes (bvh_tree_t): a mesh's over its
// triangles' boxes, an instance BVH's over its instances'. Built with a
// binned surface-area heuristic, walked nearest child first.

static const double INF = std::numeric_limits<double>::infinity();

// How far outside a triangle, in barycentric units, a ray still hits it: two
// triangles sharing an edge leave no crack for a ray to slip through.
// Triangles' boxes are padded to match.
static const double EDGE_TOLERANCE = 1e-12;
// Rays' box tests are widened by this, relative to t, so rounding never
// culls a box the ray touches.
static const double BOX_SLACK = 4.0 * DBL_EPSILON;

static const uint32_t MESH_LEAF_SIZE = 4;
static const uint32_t INSTANCE_LEAF_SIZE = 2;
static const uint32_t SAH_BINS = 16;
// Past this depth a build splits at the median instead, which halves a node
// every level: at most 32 more for 2^32 boxes. So no tree is deeper than
// 92, and a walk's stack never holds more than STACK_SIZE nodes.
static const uint32_t SAH_DEPTH_LIMIT = 60;
static const uint32_t STACK_SIZE = 96;

// ---- Boxes --------------------------------------------------------------------

namespace
{
struct box_t
{
    glm::dvec3 lo{INF}, hi{-INF};

    void add(const glm::dvec3 &point)
    {
        lo = glm::min(lo, point);
        hi = glm::max(hi, point);
    }
    void add(const box_t &box)
    {
        lo = glm::min(lo, box.lo);
        hi = glm::max(hi, box.hi);
    }
    bool empty() const
    {
        return lo.x > hi.x || lo.y > hi.y || lo.z > hi.z;
    }
    double area() const
    {
        glm::dvec3 d = hi - lo;
        return 2.0 * (d.x * d.y + d.y * d.z + d.z * d.x);
    }
    double max_extent() const
    {
        glm::dvec3 d = hi - lo;
        return std::max(d.x, std::max(d.y, d.z));
    }
    void pad(double amount)
    {
        lo -= glm::dvec3(amount);
        hi += glm::dvec3(amount);
    }
};

// A leaf if [count] isn't 0: boxes order[first .. first + count). Otherwise
// its children are nodes [first] and [first] + 1.
struct bvh_node_t
{
    glm::dvec3 lo{0.0}, hi{0.0};
    uint32_t first{0};
    uint32_t count{0};
};

struct bvh_tree_t
{
    std::vector<bvh_node_t> nodes; // [0] is the root; empty for no boxes
    std::vector<uint32_t> order;   // the boxes' indices, leaf by leaf
};

struct ray_t
{
    glm::dvec3 origin, direction, inverse;
};
} // namespace

static bool is_finite(const glm::dvec3 &v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

static glm::dvec3 to_glm(const ApriDVec3 &v)
{
    return glm::dvec3(v.x, v.y, v.z);
}

static ApriDVec3 from_glm(const glm::dvec3 &v)
{
    return ApriDVec3{v.x, v.y, v.z};
}

static APMATH_AABB to_aabb(const box_t &box)
{
    return APMATH_AABB{from_glm(box.lo), from_glm(box.hi)};
}

static ray_t make_ray(const glm::dvec3 &origin, const glm::dvec3 &direction)
{
    // A zero component's inverse is never used (see clip_to_box()).
    return ray_t{origin, direction, 1.0 / direction};
}

// Narrows [t_min, t_max] to the part of [ray] inside the box, widened by
// [slack] relative to t. False if none of it is.
static bool clip_to_box(const ray_t &ray, const glm::dvec3 &lo, const glm::dvec3 &hi, double slack, double &t_min,
                        double &t_max)
{
    for (int axis = 0; axis < 3; axis++)
    {
        if (ray.direction[axis] == 0.0)
        {
            if (ray.origin[axis] < lo[axis] || ray.origin[axis] > hi[axis])
                return false;
            continue;
        }
        double t1 = (lo[axis] - ray.origin[axis]) * ray.inverse[axis];
        double t2 = (hi[axis] - ray.origin[axis]) * ray.inverse[axis];
        if (t1 > t2)
            std::swap(t1, t2);
        if (slack != 0.0)
        {
            t1 -= std::abs(t1) * slack;
            t2 += std::abs(t2) * slack;
        }
        if (t1 > t_min)
            t_min = t1;
        if (t2 < t_max)
            t_max = t2;
        if (!(t_min <= t_max))
            return false;
    }
    return true;
}

// True, with the ray as the casts use it, if [ray] can hit anything.
static bool read_ray(const APMATH_RAY *ray, ray_t &out)
{
    if (!ray)
        return false;
    glm::dvec3 origin = to_glm(ray->origin), direction = to_glm(ray->direction);
    if (!is_finite(origin) || !is_finite(direction) || direction == glm::dvec3(0.0))
        return false;
    if (!(ray->t_min <= ray->t_max)) // also either being NaN
        return false;
    out = make_ray(origin, direction);
    return true;
}

// ---- Building -----------------------------------------------------------------

// Reorders order[first .. first + count) so its first part and the rest
// make two good children, and returns where the rest starts - never at
// either end. [centroid_bounds] is around the range's centroids.
static uint32_t split_range(const std::vector<box_t> &boxes, const std::vector<glm::dvec3> &centroids,
                            std::vector<uint32_t> &order, uint32_t first, uint32_t count, const box_t &centroid_bounds,
                            uint32_t depth)
{
    auto begin = order.begin() + first, end = begin + count;
    if (depth < SAH_DEPTH_LIMIT)
    {
        double best_cost = INF;
        int best_axis = -1;
        uint32_t best_bin = 0;
        double best_scale = 0.0;
        for (int axis = 0; axis < 3; axis++)
        {
            double extent = centroid_bounds.hi[axis] - centroid_bounds.lo[axis];
            if (!(extent > 0.0) || !std::isfinite(extent))
                continue;
            double scale = SAH_BINS / extent;
            if (!std::isfinite(scale))
                continue;
            box_t bin_boxes[SAH_BINS];
            uint32_t bin_counts[SAH_BINS] = {};
            for (auto it = begin; it != end; ++it)
            {
                uint32_t bin = std::min(SAH_BINS - 1,
                                        (uint32_t)((centroids[*it][axis] - centroid_bounds.lo[axis]) * scale));
                bin_boxes[bin].add(boxes[*it]);
                bin_counts[bin]++;
            }
            // The cost of splitting before bin i + 1: each side's area times
            // how many boxes it holds.
            double right_areas[SAH_BINS - 1];
            uint32_t right_counts[SAH_BINS - 1];
            box_t side;
            uint32_t side_count = 0;
            for (uint32_t i = SAH_BINS - 1; i > 0; i--)
            {
                side.add(bin_boxes[i]);
                side_count += bin_counts[i];
                right_areas[i - 1] = side_count ? side.area() : 0.0;
                right_counts[i - 1] = side_count;
            }
            side = box_t();
            side_count = 0;
            for (uint32_t i = 0; i < SAH_BINS - 1; i++)
            {
                side.add(bin_boxes[i]);
                side_count += bin_counts[i];
                if (!side_count || !right_counts[i])
                    continue;
                double cost = side_count * side.area() + right_counts[i] * right_areas[i];
                if (cost < best_cost)
                {
                    best_cost = cost;
                    best_axis = axis;
                    best_bin = i + 1;
                    best_scale = scale;
                }
            }
        }
        if (best_axis >= 0)
        {
            double lo = centroid_bounds.lo[best_axis];
            auto middle = std::partition(begin, end, [&](uint32_t index) {
                return std::min(SAH_BINS - 1, (uint32_t)((centroids[index][best_axis] - lo) * best_scale)) < best_bin;
            });
            return first + (uint32_t)(middle - begin);
        }
    }
    // The centroids coincide, the areas aren't finite, or the tree is deep:
    // half the boxes each side, along the axis they're most spread on.
    glm::dvec3 spread = centroid_bounds.hi - centroid_bounds.lo;
    int axis = spread.y > spread.x ? 1 : 0;
    if (spread.z > spread[axis])
        axis = 2;
    auto middle = begin + count / 2;
    std::nth_element(begin, middle, end,
                     [&](uint32_t a, uint32_t b) { return centroids[a][axis] < centroids[b][axis]; });
    return first + count / 2;
}

// Throws std::bad_alloc. None of [boxes] is empty.
static void build_tree(const std::vector<box_t> &boxes, uint32_t leaf_size, bvh_tree_t &tree)
{
    uint32_t box_count = (uint32_t)boxes.size();
    tree.order.resize(box_count);
    std::iota(tree.order.begin(), tree.order.end(), 0u);
    if (!box_count)
        return;
    std::vector<glm::dvec3> centroids(box_count);
    for (uint32_t i = 0; i < box_count; i++)
        centroids[i] = boxes[i].lo * 0.5 + boxes[i].hi * 0.5;

    struct task_t
    {
        uint32_t node, first, count, depth;
    };
    std::vector<task_t> tasks;
    // Every leaf holds a box, so there are at most box_count leaves.
    tree.nodes.reserve((size_t)box_count * 2);
    tree.nodes.emplace_back();
    tasks.push_back({0, 0, box_count, 0});
    while (!tasks.empty())
    {
        task_t task = tasks.back();
        tasks.pop_back();
        box_t bounds, centroid_bounds;
        for (uint32_t i = task.first; i < task.first + task.count; i++)
        {
            bounds.add(boxes[tree.order[i]]);
            centroid_bounds.add(centroids[tree.order[i]]);
        }
        tree.nodes[task.node].lo = bounds.lo;
        tree.nodes[task.node].hi = bounds.hi;
        if (task.count <= leaf_size)
        {
            tree.nodes[task.node].first = task.first;
            tree.nodes[task.node].count = task.count;
            continue;
        }
        uint32_t middle = split_range(boxes, centroids, tree.order, task.first, task.count, centroid_bounds, task.depth);
        uint32_t left = (uint32_t)tree.nodes.size();
        tree.nodes.emplace_back();
        tree.nodes.emplace_back();
        tree.nodes[task.node].first = left;
        tree.nodes[task.node].count = 0;
        tasks.push_back({left, task.first, middle - task.first, task.depth + 1});
        tasks.push_back({left + 1, middle, task.first + task.count - middle, task.depth + 1});
    }
}

// ---- Walking ------------------------------------------------------------------

// Calls leaf(first, count) for each leaf whose box [ray] enters within
// [t_min, t_max], nearest first. [t_max] is read again after every leaf, so
// a leaf that finds a hit shrinks it and the walk skips what's beyond.
template <typename Leaf>
static void walk_tree(const bvh_tree_t &tree, const ray_t &ray, double t_min, const double &t_max, Leaf &&leaf)
{
    if (tree.nodes.empty())
        return;
    struct entry_t
    {
        uint32_t node;
        double enter;
    };
    entry_t stack[STACK_SIZE];
    uint32_t top = 0;
    const bvh_node_t *nodes = tree.nodes.data();
    double enter = t_min, leave = t_max;
    if (!clip_to_box(ray, nodes[0].lo, nodes[0].hi, BOX_SLACK, enter, leave))
        return;
    stack[top++] = {0, enter};
    while (top)
    {
        entry_t entry = stack[--top];
        if (entry.enter > t_max)
            continue;
        const bvh_node_t &node = nodes[entry.node];
        if (node.count)
        {
            leaf(node.first, node.count);
            continue;
        }
        double enter_a = t_min, leave_a = t_max, enter_b = t_min, leave_b = t_max;
        bool hit_a = clip_to_box(ray, nodes[node.first].lo, nodes[node.first].hi, BOX_SLACK, enter_a, leave_a);
        bool hit_b = clip_to_box(ray, nodes[node.first + 1].lo, nodes[node.first + 1].hi, BOX_SLACK, enter_b, leave_b);
        if (hit_a && hit_b)
        {
            // The nearer one last, so it's popped first.
            if (enter_a <= enter_b)
            {
                stack[top++] = {node.first + 1, enter_b};
                stack[top++] = {node.first, enter_a};
            }
            else
            {
                stack[top++] = {node.first, enter_a};
                stack[top++] = {node.first + 1, enter_b};
            }
        }
        else if (hit_a)
            stack[top++] = {node.first, enter_a};
        else if (hit_b)
            stack[top++] = {node.first + 1, enter_b};
    }
}

// ---- Objects ------------------------------------------------------------------

namespace
{
// A triangle as casts read it: a corner and the two edges from it.
struct triangle_t
{
    glm::dvec3 v0, e1, e2;
};

// A hit inside one mesh, before its point and normal are worked out.
struct mesh_hit_t
{
    uint32_t slot{0};     // in triangles
    uint32_t triangle{0}; // in the desc's order
    double t{0.0};
    bool back_face{false};
};
} // namespace

struct apmath_mesh_bvh_t
{
#ifdef _DEBUG
    const char *debug_name{nullptr};
#endif
    // The desc's triangles, degenerate ones included.
    uint32_t triangle_count{0};
    box_t bounds;
    // The ones with an area, in the tree's leaf order, and each one's
    // position in the desc's order.
    std::vector<triangle_t> triangles;
    std::vector<uint32_t> triangle_indices;
    bvh_tree_t tree;
};

namespace
{
struct instance_t
{
    const apmath_mesh_bvh_t *mesh{nullptr};
    uint64_t id{0};
    uint32_t mask{0};
    bool placed{false}; // has a transform
    // The instance BVH's coordinates to the mesh's.
    glm::dmat3 inverse_linear{1.0};
    glm::dvec3 inverse_translation{0.0};
    // Mesh normals to the instance BVH's (not unit length).
    glm::dmat3 normal_matrix{1.0};
    // Around the placed mesh, padded. Empty if it has no triangles.
    box_t box;
};
} // namespace

struct apmath_instance_bvh_t
{
#ifdef _DEBUG
    const char *debug_name{nullptr};
#endif
    std::vector<instance_t> instances; // the desc's order
    box_t bounds;
    // Over the instances with triangles; its order holds their indices.
    bvh_tree_t tree;
};

// As aparchive.cpp's: the fields a caller's struct_size covers, zeros past
// them. False if it's smaller than the struct's first version.
template <typename T> static bool read_versioned(const T *in, size_t v1_size, T &out)
{
    if (!in || in->struct_size < v1_size)
        return false;
    memset(&out, 0, sizeof(T));
    memcpy(&out, in, in->struct_size < sizeof(T) ? in->struct_size : sizeof(T));
    out.struct_size = (uint32_t)sizeof(T);
    return true;
}

#define APMATH_MESH_BVH_DESC_V1_SIZE sizeof(APMATH_MESH_BVH_DESC)
#define APMATH_INSTANCE_BVH_DESC_V1_SIZE sizeof(APMATH_INSTANCE_BVH_DESC)
#define APMATH_CAST_DESC_V1_SIZE sizeof(APMATH_CAST_DESC)

// ---- Casting ------------------------------------------------------------------

// True if [candidate] is the hit to keep over [best]: nearer, or as near and
// earlier in the caller's order.
static bool is_better(double t, uint32_t index, double best_t, uint32_t best_index)
{
    return t < best_t || (t == best_t && index < best_index);
}

// The mesh's nearest hit within [t_min, t_max], in its own coordinates.
static bool cast_mesh(const apmath_mesh_bvh_t &mesh, const ray_t &ray, double t_min, double t_max, mesh_hit_t &out_hit)
{
    bool found = false;
    walk_tree(mesh.tree, ray, t_min, t_max, [&](uint32_t first, uint32_t count) {
        for (uint32_t slot = first; slot < first + count; slot++)
        {
            // Moller-Trumbore, both sides.
            const triangle_t &triangle = mesh.triangles[slot];
            glm::dvec3 p = glm::cross(ray.direction, triangle.e2);
            double det = glm::dot(triangle.e1, p);
            if (det == 0.0)
                continue; // the ray runs along the triangle's plane
            double inverse_det = 1.0 / det;
            glm::dvec3 s = ray.origin - triangle.v0;
            double u = glm::dot(s, p) * inverse_det;
            glm::dvec3 q = glm::cross(s, triangle.e1);
            double v = glm::dot(ray.direction, q) * inverse_det;
            // Written to fail on NaN too.
            if (!(u >= -EDGE_TOLERANCE && v >= -EDGE_TOLERANCE && u + v <= 1.0 + EDGE_TOLERANCE))
                continue;
            double t = glm::dot(triangle.e2, q) * inverse_det;
            if (!(t >= t_min && t <= t_max))
                continue;
            uint32_t index = mesh.triangle_indices[slot];
            if (found && !is_better(t, index, out_hit.t, out_hit.triangle))
                continue;
            found = true;
            out_hit.slot = slot;
            out_hit.triangle = index;
            out_hit.t = t;
            // det is -direction . normal: positive when the ray meets the front.
            out_hit.back_face = det < 0.0;
            t_max = t;
        }
    });
    return found;
}

// Unit, out of the triangle's front, in the mesh's coordinates.
static glm::dvec3 triangle_normal(const triangle_t &triangle)
{
    return glm::normalize(glm::cross(triangle.e1, triangle.e2));
}

// The instance's nearest hit within [t_min, t_max], with [ray] in the
// instance BVH's coordinates. t is the same in both: the transform is
// applied to the ray's direction as it is, not renormalized.
static bool cast_instance(const instance_t &instance, const ray_t &ray, double t_min, double t_max, mesh_hit_t &out_hit)
{
    if (!instance.placed)
        return cast_mesh(*instance.mesh, ray, t_min, t_max, out_hit);
    glm::dvec3 origin = instance.inverse_linear * ray.origin + instance.inverse_translation;
    glm::dvec3 direction = instance.inverse_linear * ray.direction;
    if (!is_finite(origin) || !is_finite(direction) || direction == glm::dvec3(0.0))
        return false;
    return cast_mesh(*instance.mesh, make_ray(origin, direction), t_min, t_max, out_hit);
}

static void fill_instance_hit(const apmath_instance_bvh_t &bvh, uint32_t index, const mesh_hit_t &hit, const ray_t &ray,
                              APMATH_INSTANCE_HIT &out)
{
    const instance_t &instance = bvh.instances[index];
    glm::dvec3 normal = triangle_normal(instance.mesh->triangles[hit.slot]);
    if (instance.placed)
        normal = glm::normalize(instance.normal_matrix * normal);
    out.instance = index;
    out.id = instance.id;
    out.triangle = hit.triangle;
    out.back_face = hit.back_face;
    out.t = hit.t;
    out.point = from_glm(ray.origin + ray.direction * hit.t);
    out.normal = from_glm(normal);
}

// False if [desc] is given and can't be read - the cast hits nothing.
static bool read_cast_desc(const APMATH_CAST_DESC *desc, APMATH_CAST_DESC &out, bool &out_filtered)
{
    out_filtered = desc != nullptr;
    if (!desc)
        return true;
    return read_versioned(desc, APMATH_CAST_DESC_V1_SIZE, out);
}

// Calls hit(index, mesh_hit) for each instance [ray] hits among those the
// desc lets through, nearest boxes first. [t_max] is read again after each.
template <typename Hit>
static void walk_instances(const apmath_instance_bvh_t &bvh, const ray_t &ray, double t_min, const double &t_max,
                           bool filtered, const APMATH_CAST_DESC &desc, Hit &&hit)
{
    walk_tree(bvh.tree, ray, t_min, t_max, [&](uint32_t first, uint32_t count) {
        for (uint32_t i = first; i < first + count; i++)
        {
            uint32_t index = bvh.tree.order[i];
            const instance_t &instance = bvh.instances[index];
            if (filtered && !(instance.mask & desc.mask))
                continue;
            double enter = t_min, leave = t_max;
            if (!clip_to_box(ray, instance.box.lo, instance.box.hi, BOX_SLACK, enter, leave))
                continue;
            if (filtered && desc.filter && !desc.filter(desc.user, index, instance.id))
                continue;
            mesh_hit_t mesh_hit;
            if (cast_instance(instance, ray, t_min, t_max, mesh_hit))
                hit(index, mesh_hit);
        }
    });
}

extern "C"
{
    // ---- Rays and boxes ---------------------------------------------------------

    bool apmath_ray_clip_aabb(APMATH_RAY *ray, const APMATH_AABB *box)
    {
        ray_t r;
        if (!box || !read_ray(ray, r))
            return false;
        glm::dvec3 lo = to_glm(box->lo), hi = to_glm(box->hi);
        if (!(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z)) // empty, or NaN
            return false;
        double t_min = ray->t_min, t_max = ray->t_max;
        if (!clip_to_box(r, lo, hi, 0.0, t_min, t_max))
            return false;
        ray->t_min = t_min;
        ray->t_max = t_max;
        return true;
    }

    // ---- Mesh BVH -----------------------------------------------------------------

    APRESULT apmath_mesh_bvh_create(const APMATH_MESH_BVH_DESC *desc, apmath_mesh_bvh *out_bvh)
    {
        if (!out_bvh)
            return APRESULT_INVALID_ARGUMENT;
        *out_bvh = nullptr;
        APMATH_MESH_BVH_DESC d;
        if (!read_versioned(desc, APMATH_MESH_BVH_DESC_V1_SIZE, d))
            return APRESULT_INVALID_ARGUMENT;
        if (d.position_count && !d.positions)
            return APRESULT_INVALID_ARGUMENT;
        uint32_t corner_count = d.indices ? d.index_count : d.position_count;
        if (corner_count % 3 || (!d.indices && d.index_count))
            return APRESULT_INVALID_ARGUMENT;
        for (uint32_t i = 0; i < d.position_count; i++)
            if (!is_finite(to_glm(d.positions[i])))
                return APRESULT_INVALID_ARGUMENT;
        for (uint32_t i = 0; i < d.index_count; i++)
            if (d.indices[i] >= d.position_count)
                return APRESULT_INVALID_ARGUMENT;

        apmath_mesh_bvh_t *bvh = new (std::nothrow) apmath_mesh_bvh_t();
        if (!bvh)
            return APRESULT_OUT_OF_MEMORY;
        try
        {
            bvh->triangle_count = corner_count / 3;
            std::vector<triangle_t> triangles;
            std::vector<uint32_t> indices;
            std::vector<box_t> boxes;
            for (uint32_t triangle = 0; triangle < bvh->triangle_count; triangle++)
            {
                glm::dvec3 corners[3];
                for (uint32_t corner = 0; corner < 3; corner++)
                {
                    uint32_t at = triangle * 3 + corner;
                    corners[corner] = to_glm(d.positions[d.indices ? d.indices[at] : at]);
                    bvh->bounds.add(corners[corner]);
                }
                glm::dvec3 e1 = corners[1] - corners[0], e2 = corners[2] - corners[0];
                double doubled_area = glm::length(glm::cross(e1, e2));
                if (!(doubled_area > 0.0) || !std::isfinite(doubled_area))
                    continue; // no area: never hit
                box_t box;
                for (const glm::dvec3 &corner : corners)
                    box.add(corner);
                // Covers what EDGE_TOLERANCE lets a ray hit past the edges.
                box.pad(4.0 * EDGE_TOLERANCE * box.max_extent());
                triangles.push_back({corners[0], e1, e2});
                indices.push_back(triangle);
                boxes.push_back(box);
            }
            build_tree(boxes, MESH_LEAF_SIZE, bvh->tree);
            // Into leaf order, so a leaf's triangles sit together.
            bvh->triangles.resize(triangles.size());
            bvh->triangle_indices.resize(triangles.size());
            for (size_t slot = 0; slot < triangles.size(); slot++)
            {
                bvh->triangles[slot] = triangles[bvh->tree.order[slot]];
                bvh->triangle_indices[slot] = indices[bvh->tree.order[slot]];
            }
            // A leaf's range is in slots now; the order has done its job.
            std::vector<uint32_t>().swap(bvh->tree.order);
        }
        catch (const std::bad_alloc &)
        {
            delete bvh;
            return APRESULT_OUT_OF_MEMORY;
        }
        *out_bvh = bvh;
        return APRESULT_OK;
    }

    void apmath_mesh_bvh_destroy(apmath_mesh_bvh bvh)
    {
        delete bvh;
    }

    uint32_t apmath_mesh_bvh_get_triangle_count(apmath_mesh_bvh bvh)
    {
        return bvh ? bvh->triangle_count : 0;
    }

    APMATH_AABB apmath_mesh_bvh_get_bounds(apmath_mesh_bvh bvh)
    {
        return to_aabb(bvh ? bvh->bounds : box_t());
    }

    bool apmath_mesh_bvh_cast(apmath_mesh_bvh bvh, const APMATH_RAY *ray, APMATH_MESH_HIT *out_hit)
    {
        ray_t r;
        if (!bvh || !read_ray(ray, r))
            return false;
        mesh_hit_t hit;
        if (!cast_mesh(*bvh, r, ray->t_min, ray->t_max, hit))
            return false;
        if (out_hit)
        {
            out_hit->triangle = hit.triangle;
            out_hit->back_face = hit.back_face;
            out_hit->t = hit.t;
            out_hit->point = from_glm(r.origin + r.direction * hit.t);
            out_hit->normal = from_glm(triangle_normal(bvh->triangles[hit.slot]));
        }
        return true;
    }

    // ---- Instance BVH -------------------------------------------------------------

    APRESULT apmath_instance_bvh_create(const APMATH_INSTANCE_BVH_DESC *desc, apmath_instance_bvh *out_bvh)
    {
        if (!out_bvh)
            return APRESULT_INVALID_ARGUMENT;
        *out_bvh = nullptr;
        APMATH_INSTANCE_BVH_DESC d;
        if (!read_versioned(desc, APMATH_INSTANCE_BVH_DESC_V1_SIZE, d))
            return APRESULT_INVALID_ARGUMENT;
        // Node indices are 32-bit, and a tree has up to two nodes a box.
        if ((d.instance_count && !d.instances) || d.instance_count > UINT32_MAX / 2)
            return APRESULT_INVALID_ARGUMENT;

        apmath_instance_bvh_t *bvh = new (std::nothrow) apmath_instance_bvh_t();
        if (!bvh)
            return APRESULT_OUT_OF_MEMORY;
        try
        {
            bvh->instances.resize(d.instance_count);
            std::vector<box_t> boxes;
            std::vector<uint32_t> boxed; // boxes[i] is instance boxed[i]'s
            for (uint32_t index = 0; index < d.instance_count; index++)
            {
                const APMATH_BVH_INSTANCE &in = d.instances[index];
                instance_t &instance = bvh->instances[index];
                instance.mesh = in.mesh;
                instance.id = in.id;
                instance.mask = in.mask;
                box_t mesh_bounds = in.mesh ? in.mesh->bounds : box_t();
                if (in.transform)
                {
                    const double *m = in.transform->m; // column-major
                    for (int i = 0; i < 16; i++)
                        if (!std::isfinite(m[i]))
                        {
                            delete bvh;
                            return APRESULT_INVALID_ARGUMENT;
                        }
                    glm::dmat3 linear(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
                    glm::dvec3 translation(m[12], m[13], m[14]);
                    double det = glm::determinant(linear);
                    glm::dmat3 inverse = glm::inverse(linear);
                    bool affine = m[3] == 0.0 && m[7] == 0.0 && m[11] == 0.0 && m[15] == 1.0;
                    bool invertible = det != 0.0 && std::isfinite(det) && is_finite(inverse[0]) &&
                                      is_finite(inverse[1]) && is_finite(inverse[2]);
                    if (!affine || !invertible)
                    {
                        delete bvh;
                        return APRESULT_INVALID_ARGUMENT;
                    }
                    instance.placed = true;
                    instance.inverse_linear = inverse;
                    instance.inverse_translation = -(inverse * translation);
                    instance.normal_matrix = glm::transpose(inverse);
                    if (!mesh_bounds.empty())
                    {
                        // Around the mesh's box's corners: loose for a rotated
                        // mesh, but never too small.
                        for (int corner = 0; corner < 8; corner++)
                        {
                            glm::dvec3 point(corner & 1 ? mesh_bounds.hi.x : mesh_bounds.lo.x,
                                             corner & 2 ? mesh_bounds.hi.y : mesh_bounds.lo.y,
                                             corner & 4 ? mesh_bounds.hi.z : mesh_bounds.lo.z);
                            instance.box.add(linear * point + translation);
                        }
                    }
                }
                else
                    instance.box = mesh_bounds;
                if (instance.box.empty())
                    continue;
                if (!is_finite(instance.box.lo) || !is_finite(instance.box.hi))
                {
                    delete bvh; // placed out of double's range
                    return APRESULT_INVALID_ARGUMENT;
                }
                bvh->bounds.add(instance.box);
                // As a triangle's box, plus the transform's own rounding.
                glm::dvec3 reach = glm::max(glm::abs(instance.box.lo), glm::abs(instance.box.hi));
                instance.box.pad(4.0 * EDGE_TOLERANCE * instance.box.max_extent() +
                                 16.0 * DBL_EPSILON * std::max(reach.x, std::max(reach.y, reach.z)));
                boxes.push_back(instance.box);
                boxed.push_back(index);
            }
            build_tree(boxes, INSTANCE_LEAF_SIZE, bvh->tree);
            for (uint32_t &entry : bvh->tree.order)
                entry = boxed[entry];
        }
        catch (const std::bad_alloc &)
        {
            delete bvh;
            return APRESULT_OUT_OF_MEMORY;
        }
        *out_bvh = bvh;
        return APRESULT_OK;
    }

    void apmath_instance_bvh_destroy(apmath_instance_bvh bvh)
    {
        delete bvh;
    }

    uint32_t apmath_instance_bvh_get_instance_count(apmath_instance_bvh bvh)
    {
        return bvh ? (uint32_t)bvh->instances.size() : 0;
    }

    APMATH_AABB apmath_instance_bvh_get_bounds(apmath_instance_bvh bvh)
    {
        return to_aabb(bvh ? bvh->bounds : box_t());
    }

    bool apmath_instance_bvh_cast(apmath_instance_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                                  APMATH_INSTANCE_HIT *out_hit)
    {
        ray_t r;
        APMATH_CAST_DESC d;
        bool filtered;
        if (!bvh || !read_ray(ray, r) || !read_cast_desc(desc, d, filtered))
            return false;
        bool found = false;
        uint32_t best_index = 0;
        mesh_hit_t best;
        double t_max = ray->t_max;
        walk_instances(*bvh, r, ray->t_min, t_max, filtered, d, [&](uint32_t index, const mesh_hit_t &hit) {
            if (found && !is_better(hit.t, index, best.t, best_index))
                return;
            found = true;
            best_index = index;
            best = hit;
            t_max = hit.t;
        });
        if (found && out_hit)
            fill_instance_hit(*bvh, best_index, best, r, *out_hit);
        return found;
    }

    uint32_t apmath_instance_bvh_cast_all(apmath_instance_bvh bvh, const APMATH_RAY *ray, const APMATH_CAST_DESC *desc,
                                          APMATH_INSTANCE_HIT *out_hits, uint32_t hit_capacity)
    {
        ray_t r;
        APMATH_CAST_DESC d;
        bool filtered;
        if (!bvh || !read_ray(ray, r) || !read_cast_desc(desc, d, filtered))
            return 0;
        if (!out_hits)
            hit_capacity = 0;
        // The caller's buffer is a heap of the best hits so far, the worst of
        // them on top to be pushed out - so nothing is allocated.
        auto before = [](const APMATH_INSTANCE_HIT &a, const APMATH_INSTANCE_HIT &b) {
            return is_better(a.t, a.instance, b.t, b.instance);
        };
        uint32_t total = 0, kept = 0;
        double t_max = ray->t_max;
        walk_instances(*bvh, r, ray->t_min, t_max, filtered, d, [&](uint32_t index, const mesh_hit_t &hit) {
            total++;
            if (!hit_capacity)
                return;
            if (kept == hit_capacity)
            {
                if (!is_better(hit.t, index, out_hits[0].t, out_hits[0].instance))
                    return;
                std::pop_heap(out_hits, out_hits + kept, before);
                kept--;
            }
            fill_instance_hit(*bvh, index, hit, r, out_hits[kept++]);
            std::push_heap(out_hits, out_hits + kept, before);
        });
        std::sort_heap(out_hits, out_hits + kept, before);
        return total;
    }
}
