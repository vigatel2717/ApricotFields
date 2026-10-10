#ifndef APREND_INTERNAL_HPP
#define APREND_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/.
 * Defines the concrete structs behind Aprend's opaque handles, and provides
 * zero-cost GLM conversion helpers. */

#include "aprimath.h"
#include "render/aprendbuffers.h"
#include "render/aprendcommands.h"
#include "render/aprendcontext.h"
#include "render/aprendframes.h"
#include "render/aprendpipeline.h"
#include "render/aprendswapchain.h"

#include <spudgpu.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/* ============================================================
   GLM conversion helpers
   Reinterpret-cast is valid because Apri### structs are laid out
   identically to their GLM counterparts (same member order, same types).
   ============================================================ */

inline glm::vec2 &to_glm(ApriVec2 &v) { return reinterpret_cast<glm::vec2 &>(v); }
inline glm::vec3 &to_glm(ApriVec3 &v) { return reinterpret_cast<glm::vec3 &>(v); }
inline glm::vec4 &to_glm(ApriVec4 &v) { return reinterpret_cast<glm::vec4 &>(v); }
inline glm::dvec3 &to_glm(ApriDVec3 &v) { return reinterpret_cast<glm::dvec3 &>(v); }
inline glm::dvec4 &to_glm(ApriDVec4 &v) { return reinterpret_cast<glm::dvec4 &>(v); }
inline glm::quat &to_glm(ApriQuat &v) { return reinterpret_cast<glm::quat &>(v); }
inline glm::mat4 &to_glm(ApriMat4 &v) { return reinterpret_cast<glm::mat4 &>(v); }
inline glm::dmat4 &to_glm(ApriDMat4 &v) { return reinterpret_cast<glm::dmat4 &>(v); }

inline const glm::vec2 &to_glm(const ApriVec2 &v) { return reinterpret_cast<const glm::vec2 &>(v); }
inline const glm::vec3 &to_glm(const ApriVec3 &v) { return reinterpret_cast<const glm::vec3 &>(v); }
inline const glm::vec4 &to_glm(const ApriVec4 &v) { return reinterpret_cast<const glm::vec4 &>(v); }
inline const glm::dvec3 &to_glm(const ApriDVec3 &v) { return reinterpret_cast<const glm::dvec3 &>(v); }
inline const glm::quat &to_glm(const ApriQuat &v) { return reinterpret_cast<const glm::quat &>(v); }
inline const glm::mat4 &to_glm(const ApriMat4 &v) { return reinterpret_cast<const glm::mat4 &>(v); }

inline ApriVec3 from_glm(const glm::vec3 &v) { return {v.x, v.y, v.z}; }
inline ApriDVec3 from_glm(const glm::dvec3 &v) { return {v.x, v.y, v.z}; }
inline ApriQuat from_glm(const glm::quat &v) { return {v.x, v.y, v.z, v.w}; }
inline ApriMat4 from_glm(const glm::mat4 &m) {
	ApriMat4 r;
	std::memcpy(r.m, &m[0][0], sizeof(r.m));
	return r;
}

/* ============================================================
 * Macro helpers for managing
 * malloc & new(var)constructor() & deletion/free
 * ============================================================ */
#define APREND_MALLOC__T(var, T) T *var = (T *)malloc(sizeof(T))
#define APREND_CONSTRUCT__T(var, T) var = new (var) T()
#define APREND_DESTRUCT__T(var, T) var->~T(); free(var)

/* Binding sets per descriptor pool of a binding layout. Sets are small and a
 * layout with more than a handful is rare, so this is sized to make a second
 * pool unusual without reserving much for a layout that has one set. */
#define APREND_BINDING_SETS_PER_POOL 32

/* How many SPUDGPU_DESCRIPTOR_TYPE values there are; they are 0 to this - 1. */
#define APREND_DESCRIPTOR_TYPE_COUNT (SPUDGPU_DESCRIPTOR_TYPE_STORAGE_IMAGE + 1)

typedef struct aprend_instance_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_instance_t() = default;
	~aprend_instance_t();

	aprend_instance_desc desc{};
	spudgpu_command_allocator cmd_allocator{nullptr};
	spudgpu_command_list cmd_list{nullptr};
} aprend_instance_t;

typedef struct aprend_command_list_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_command_list_t() = default;
	~aprend_command_list_t();
	aprend_instance instance{nullptr};
	spudgpu_command_list cmd_list{nullptr};
	std::vector<APREND_COMMAND> commands{};
	/* Owned copies of each BEGIN_RENDERING's color target array, so recorded
	 * commands never point at caller memory. Each inner vector's buffer stays
	 * put when the outer vector grows (moves don't reallocate it). */
	std::vector<std::vector<aprend_color_target>> color_target_storage{};
	/* The same for the arrays of SET_VERTEX_BUFFERS, SET_VIEWPORTS and
	 * SET_SCISSOR_RECTS. */
	std::vector<std::vector<aprend_vertex_buffer>> vertex_buffer_storage{};
	std::vector<std::vector<SPUDGPU_VIEWPORT>> viewport_storage{};
	std::vector<std::vector<SPUDGPU_SCISSOR_RECT>> scissor_rect_storage{};
	/* Set by aprend_send_command on a malformed command; fails compile until reset. */
	bool recording_error{false};

	/* Per-texture layout transitions of the last compile. Compiling never
	 * touches a texture's current_layout (the layout the GPU will see
	 * once everything submitted so far has run) - only a successful
	 * aprend_command_list_submit commits final_layout to it. initial_layout is
	 * what this list's first barrier assumes, checked against current_layout
	 * at submit. */
	struct layout_use {
		spudgpu_image image;
		/* The texture's current_layout (2D or 3D). */
		SPUDGPU_IMAGE_LAYOUT *tracked;
		SPUDGPU_IMAGE_LAYOUT initial_layout;
		SPUDGPU_IMAGE_LAYOUT final_layout;
	};
	std::vector<layout_use> layout_uses{};
	/* The last compile succeeded, so the spudgpu list is closed and submittable. */
	bool compiled{false};
	/* Set by a compiled APREND_COMMAND_PRESENT_TEXTURE: the swap chain and the
	 * back buffer index the list copies into. Submit uses the swap-chain-
	 * synchronized path for such a list. */
	aprend_swap_chain present_swap_chain{nullptr};
	uint32_t present_image_index{0};
} aprend_command_list_t;

#define APREND_SWAP_CHAIN_NO_IMAGE 0xFFFFFFFFu

typedef struct aprend_swap_chain_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_swap_chain_t() = default;
	~aprend_swap_chain_t();
	aprend_instance instance{nullptr};
	aprend_swap_chain_desc desc{};
	spudgpu_swap_chain swap_chain{nullptr};
	/* APREND_SWAP_CHAIN_NO_IMAGE until aprend_swap_chain_acquire. */
	uint32_t acquired_image{APREND_SWAP_CHAIN_NO_IMAGE};
	/* A list presenting into acquired_image has been submitted. */
	bool submitted{false};
} aprend_swap_chain_t;

typedef struct aprend_uniform_buffer_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_uniform_buffer_t() = default;
	~aprend_uniform_buffer_t();
	spudgpu_buffer buffer{nullptr};
	spudgpu_buffer_view buffer_view{nullptr};
	spudgpu_buffer_view_desc buffer_view_desc{};
	aprend_uniform_layout layout{};
	uint32_t total_size{0};
	void *uniform_data_ptr{nullptr};
} aprend_uniform_buffer_t;

typedef struct aprend_vertex_buffer_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_vertex_buffer_t() = default;
	~aprend_vertex_buffer_t();
	spudgpu_buffer buffer{nullptr};
	spudgpu_buffer_view buffer_view{nullptr};
	spudgpu_buffer_view_desc buffer_view_desc{};
	aprend_buffer_layout vertex_layout{};
	uint32_t vertex_count{0};
	uint32_t vertex_stride{0};
} aprend_vertex_buffer_t;

typedef struct aprend_index_buffer_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_index_buffer_t() = default;
	~aprend_index_buffer_t();
	spudgpu_buffer buffer{nullptr};
	spudgpu_buffer_view buffer_view{nullptr};
	spudgpu_buffer_view_desc buffer_view_desc{};
	uint32_t index_count{0};
	APREND_INDEX_STRIDE index_stride{APREND_INDEX_STRIDE_NONE};
} aprend_index_buffer_t;

typedef struct aprend_storage_buffer_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_storage_buffer_t() = default;
	~aprend_storage_buffer_t();
	spudgpu_buffer buffer{nullptr};
	spudgpu_buffer_view buffer_view{nullptr};
	spudgpu_buffer_view_desc buffer_view_desc{};
	uint64_t size{0};
} aprend_storage_buffer_t;

typedef struct aprend_shader_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_shader_t() = default;
	~aprend_shader_t();
	aprend_instance instance{nullptr};
	spudgpu_shader_module shader_module{nullptr};
} aprend_shader_t;

typedef struct aprend_graphics_pipeline_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_graphics_pipeline_t() = default;
	~aprend_graphics_pipeline_t();
	aprend_graphics_pipeline_desc desc{};
	aprend_instance instance{nullptr};
	spudgpu_shader_pipeline pipeline{nullptr};
} aprend_graphics_pipeline_t;

typedef struct aprend_binding_layout_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_binding_layout_t() = default;
	~aprend_binding_layout_t();
	aprend_instance instance{nullptr};
	aprend_binding_desc bindings[APREND_MAX_BINDINGS_PER_LAYOUT]{};
	uint32_t binding_count{0};
	/* Every binding's _count added up: the entries one set takes. */
	uint32_t descriptor_count{0};
	spudgpu_descriptor_set_layout layout{nullptr};
	/* The pools this layout's sets come from, each sized for
	 * APREND_BINDING_SETS_PER_POOL sets. A pool is added when the last one is
	 * full; none is released before the layout. */
	spudgpu_descriptor_pool *pools{nullptr};
	uint32_t pool_count{0};
	/* Sets allocated so far from the last pool. */
	uint32_t sets_in_last_pool{0};
	/* Sets whose aprend_binding_set was destroyed, handed out again before
	 * anything new is allocated: SpudGPU releases sets by the pool, not one
	 * at a time. Always has room for every set of every pool. */
	spudgpu_descriptor_set *free_sets{nullptr};
	uint32_t free_set_count{0};
} aprend_binding_layout_t;

typedef struct aprend_binding_set_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_binding_set_t() = default;
	~aprend_binding_set_t();
	aprend_binding_layout layout{nullptr};
	/* From one of the layout's pools; goes back on its free list. */
	spudgpu_descriptor_set set{nullptr};
	/* Every texture view the set holds and the layout its slot reads the
	 * texture in, for a command list to move the texture there before a pass
	 * that binds the set. */
	struct image_use {
		aprend_texture_view view;
		SPUDGPU_IMAGE_LAYOUT layout;
	};
	image_use *image_uses{nullptr};
	uint32_t image_use_count{0};
} aprend_binding_set_t;

#endif // APREND_INTERNAL_HPP
