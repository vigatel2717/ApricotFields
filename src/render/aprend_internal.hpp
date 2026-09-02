#ifndef APREND_INTERNAL_HPP
#define APREND_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/.
 * Defines the concrete structs behind every opaque handle in aprendscene.h
 * and aprendbase.h, and provides zero-cost GLM conversion helpers. */

#include "aprimath.h"
#include "render/aprendbuffers.h"
#include "render/aprendcommands.h"
#include "render/aprenderer.h"
#include "render/aprendframes.h"
#include "render/aprendpipeline.h"

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
} aprend_command_list_t;

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

#endif // APREND_INTERNAL_HPP
