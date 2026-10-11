#ifndef APREND_INTERNAL_HPP
#define APREND_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/render/.
 * Defines the concrete structs behind Aprend's opaque handles, and provides
 * zero-cost GLM conversion helpers. */

#include "aprimath.h"
#include "render/aprendbuffers.h"
#include "render/aprendcommands.h"
#include "render/aprendcontext.h"
#include "render/aprendpipeline.h"
#include "render/aprendswapchain.h"

#include <spudgpu.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <atomic>
#include <cstring>
#include <mutex>
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
	/* SPUDGPU_DEVICE_PROPERTIES::unified_memory of desc.device, read once at
	 * creation. Decides where vertex, index and storage buffers are kept
	 * (aprend_buffer_store). */
	bool unified_memory{false};
	/* SPUDGPU_DEVICE_PROPERTIES::device_mappable_memory of desc.device, read
	 * with it: whether a buffer a shader may write can be written directly
	 * too (SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE). */
	bool device_mappable_memory{false};
	/* Which copy of every buffer set is current: 0 to
	 * desc.frames_in_flight - 1. */
	uint32_t frame_index{0};
	/* Paces frames. Every aprend_command_list_submit signals it to the next
	 * submit_serial, and frame_last_serial[i] is the serial of the last
	 * submission made while frame index i was current: what
	 * aprend_instance_next_frame waits for before index i is current again.
	 * 0 for an index nothing has been submitted in, which the fence, starting
	 * at 0, has already reached. */
	spudgpu_fence frame_fence{nullptr};
	/* Atomic because a destroy call reads it from whatever thread it is
	 * made on (aprend_instance_retire); only a submission writes it. */
	std::atomic<uint64_t> submit_serial{0};
	uint64_t frame_last_serial[APREND_MAX_FRAMES_IN_FLIGHT]{};
	/* The one queue everything of this instance is submitted on: the queue
	 * of its first submission, NULL until then. The serials are only an
	 * order of completion on a single queue, so aprend_instance_submit
	 * refuses any other. */
	spudgpu_command_queue submit_queue{nullptr};
	/* The release queue: what has been destroyed by its owner and may still
	 * be in use by work the GPU hasn't finished. An entry is released once
	 * the fence has reached its serial, which is submit_serial as it was
	 * when the object was retired: everything submitted up to then may use
	 * the object, and nothing submitted after may. Serials never go down
	 * along the queue, so it is released strictly from the front, in the
	 * order things were retired. Entries before release_head are done. */
	struct release_entry {
		void (*release)(void *object);
		void *object;
		uint64_t serial;
	};
	std::vector<release_entry> release_queue{};
	size_t release_head{0};
	/* Guards release_queue and release_head, and nothing else. It is what
	 * lets a destroy call come from any thread: retiring only pushes an
	 * entry under it. It is never held while a release function runs. */
	std::mutex release_mutex{};
} aprend_instance_t;

/* Queues [object], a handle of [instance] or something one owns, to be
 * released with [release] once every submission made so far has finished.
 * Every destroy call of a GPU-backed handle goes through this, so the caller
 * of one doesn't have to know the GPU is finished with it.
 *
 * Safe from any thread, at the same time as anything else on the instance:
 * it takes the serial and pushes the entry under release_mutex and does
 * nothing more. It never releases anything itself, since a release function
 * touches what the one-thread rule guards (SpudGPU objects, a binding
 * layout's pools). Releasing is aprend_instance_collect's, on the thread
 * that is using the instance.
 *
 * Never releases early. With no memory to queue the entry, the object is
 * left unreleased and that is printed: there is no thread here it would be
 * safe to release it on, and a leak under memory exhaustion is the one
 * outcome that can't make things worse.
 *
 * What it can't see is a command list that was compiled against the object
 * and not yet submitted. Compiling records nothing here; the serial is taken
 * at retirement, so such a list, submitted afterwards, runs after the object
 * may already be gone. Not using a destroyed object in a later submission
 * stays the caller's. */
void aprend_instance_retire(
    aprend_instance instance,
    void *object,
    void (*release)(void *object));
/* Releases every queued entry whose serial the fence has reached, each
 * outside release_mutex. Only for the thread using the instance under the
 * one-thread rule: called after each submission and each wait, never from a
 * destroy call. Cheap when there is nothing to do. */
void aprend_instance_collect(aprend_instance instance);

/* The release function of a handle struct T: its destructor, then free.
 * For aprend_instance_retire. */
template <typename T> void aprend_release_handle(void *object) {
	T *handle = (T *)object;
	handle->~T();
	free(handle);
}
/* Release functions for SpudGPU objects retired on their own, apart from the
 * handle that owned them. */
inline void aprend_release_spudgpu_buffer(void *object) { spudgpu_destroy_buffer((spudgpu_buffer)object); }
inline void aprend_release_spudgpu_image(void *object) { spudgpu_destroy_image((spudgpu_image)object); }
inline void aprend_release_spudgpu_image_view(void *object) { spudgpu_destroy_image_view((spudgpu_image_view)object); }
inline void aprend_release_spudgpu_command_list(void *object) { spudgpu_destroy_command_list((spudgpu_command_list)object); }
inline void aprend_release_spudgpu_command_allocator(void *object) { spudgpu_destroy_command_allocator((spudgpu_command_allocator)object); }

/* The one way anything of [instance] reaches a queue. Refuses, with
 * SPUDRESULT_GPU_INVALID_COMMAND_QUEUE and nothing submitted, a [queue] that
 * is not the one the instance's first submission went on. Submits [lists] on
 * [queue], synchronized with [swap_chain] if it isn't NULL, and signals the
 * instance's frame fence to the next serial, which it records against the
 * current frame index and hands back through [out_serial] (NULL to skip). A
 * failed submission takes no serial. */
SPUDRESULT aprend_instance_submit(
    aprend_instance instance,
    spudgpu_command_queue queue,
    spudgpu_command_list *lists,
    uint32_t list_count,
    spudgpu_swap_chain swap_chain,
    uint64_t *out_serial);
/* Blocks until the submission that took [serial], and every one before it,
 * has finished. */
SPUDRESULT aprend_instance_wait_serial(
    aprend_instance instance,
    uint64_t serial);

/* Where the bytes of a vertex, index or storage buffer are kept, chosen from
 * the device's memory model when the buffer is created.
 *
 * On a device whose memory is the system's, [buffer] is mapped and a write
 * goes straight into it: there is one memory, so nothing is gained by a
 * second copy. A vertex or index buffer is SPUDGPU_MEMORY_KIND_UPLOAD. A
 * storage buffer, which a shader may write, is
 * SPUDGPU_MEMORY_KIND_DEVICE_MAPPABLE where the device has that kind, and is
 * staged like the case below where it doesn't.
 *
 * On a device with memory of its own, [buffer] is SPUDGPU_MEMORY_KIND_DEVICE:
 * in that memory, where the GPU reads it without crossing the bus, and never
 * mapped. A write goes into [staging], an UPLOAD buffer of the same size that
 * holds everything ever written, and widens the dirty range. The next
 * aprend_command_list_submit of a list that uses the buffer copies the dirty
 * range across ahead of the list's own commands.
 *
 * Every write is flushed (spudgpu_flush_buffer), whichever buffer it lands
 * in.
 *
 * Uniform buffers don't use this: they are small and rewritten every frame,
 * and are UPLOAD memory, mapped for life, on every device. */
struct aprend_buffer_store {
	aprend_instance instance{nullptr};
	/* What the GPU reads, and what views and descriptors are made of. */
	spudgpu_buffer buffer{nullptr};
	/* NULL when [buffer] is written directly. */
	spudgpu_buffer staging{nullptr};
	uint64_t size{0};
	/* The state [buffer] is used in, which a copy into it leaves it in. */
	SPUDGPU_RESOURCE_STATE use_state{SPUDGPU_RESOURCE_STATE_COMMON};
	/* Bytes written to [staging] and not yet copied: [dirty_begin,
	 * dirty_end), empty when the two are equal. */
	uint64_t dirty_begin{0};
	uint64_t dirty_end{0};
	/* The buffer is used in more than one state: a storage buffer that is
	 * also an indirect argument buffer. A command list tracks the state of
	 * such a store and moves it (aprend_command_list_t::buffer_states); a
	 * store that is only ever used in [use_state] needs no tracking. */
	bool multi_state{false};
};

/* Creates [store]'s buffers, of [size] bytes and for [usage]. On failure the
 * store holds nothing. */
SPUDRESULT aprend_buffer_store_create(
    aprend_buffer_store *store,
    aprend_instance instance,
    uint64_t size,
    SPUDGPU_BUFFER_USAGE usage,
    SPUDGPU_RESOURCE_STATE use_state);
/* Destroys whatever [store] holds. Views of its buffer go first. */
void aprend_buffer_store_destroy(aprend_buffer_store *store);
/* Copies [size] bytes from [data] to [offset]. The range is the caller's to
 * have checked against the store's size. */
SPUDRESULT aprend_buffer_store_write(
    aprend_buffer_store *store,
    uint64_t offset,
    uint64_t size,
    const void *data);

typedef struct aprend_command_list_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_command_list_t() = default;
	~aprend_command_list_t();
	aprend_instance instance{nullptr};
	spudgpu_command_list cmd_list{nullptr};
	/* Recorded at submit with the copies of whatever staged buffers the list
	 * uses have been written since they were last copied, and submitted ahead
	 * of cmd_list. Never recording while cmd_list is: the two share the
	 * instance's allocator. */
	spudgpu_command_list upload_list{nullptr};
	/* Every staged buffer store the last compile found in use, once each. */
	std::vector<aprend_buffer_store *> staged_stores{};
	/* The state of each multi-state buffer store as the compile walks the
	 * list, from the first point the list uses it. Every such store starts
	 * the list in its use_state: aprend_command_list_submit puts it there
	 * ahead of the list's own commands, whether or not it also copies into
	 * it, so the barriers compiled here always name the state the buffer is
	 * really in. Kept after the compile for submit to know which stores
	 * those are. */
	struct buffer_state {
		aprend_buffer_store *store;
		SPUDGPU_RESOURCE_STATE state;
	};
	std::vector<buffer_state> buffer_states{};
	/* The instance's frame index at the last compile, and whether the list
	 * binds a set that has a descriptor set for each frame. Such a list
	 * reads that frame's copies and is only submittable while the frame index
	 * is still the one it was compiled in. */
	uint32_t compiled_frame{0};
	bool uses_frame_sets{false};
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
	/* And the bytes of PUSH_CONSTANTS. */
	std::vector<std::vector<uint8_t>> push_constant_storage{};
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
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
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
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
	aprend_buffer_store store{};
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
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
	aprend_buffer_store store{};
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
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
	aprend_buffer_store store{};
	spudgpu_buffer_view buffer_view{nullptr};
	spudgpu_buffer_view_desc buffer_view_desc{};
	uint64_t size{0};
	APREND_STORAGE_BUFFER_USAGE usage{APREND_STORAGE_BUFFER_USAGE_NONE};
} aprend_storage_buffer_t;

/* One uniform buffer for each frame in flight, all of one layout.
 * buffers[instance->frame_index] is the current one. */
typedef struct aprend_uniform_buffer_set_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_uniform_buffer_set_t() = default;
	~aprend_uniform_buffer_set_t();
	aprend_instance instance{nullptr};
	aprend_uniform_buffer buffers[APREND_MAX_FRAMES_IN_FLIGHT]{};
	uint32_t buffer_count{0};
} aprend_uniform_buffer_set_t;

/* The same for storage buffers, all of one size. */
typedef struct aprend_storage_buffer_set_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_storage_buffer_set_t() = default;
	~aprend_storage_buffer_set_t();
	aprend_instance instance{nullptr};
	aprend_storage_buffer buffers[APREND_MAX_FRAMES_IN_FLIGHT]{};
	uint32_t buffer_count{0};
} aprend_storage_buffer_set_t;

typedef struct aprend_shader_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_shader_t() = default;
	~aprend_shader_t();
	aprend_instance instance{nullptr};
	spudgpu_shader_module shader_module{nullptr};
	/* The one stage the shader was created for, so a pipeline can refuse a
	 * shader put in the wrong place. */
	SPUDGPU_SHADER_STAGE stage{SPUDGPU_SHADER_STAGE_NONE};
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
	/* One allocation holding the vertex entry point's name and then the
	 * fragment's, each with its terminator. desc's two entry point pointers
	 * point into it, not at the caller's strings. */
	char *entry_points{nullptr};
	/* Stride of each counted vertex binding, for a draw to check the vertex
	 * buffer in that slot against. */
	uint32_t vertex_strides[APREND_MAX_VERTEX_BINDINGS]{};
	/* Where the furthest push constant range ends; 0 with no ranges. */
	uint32_t push_constant_end{0};
	/* Created with a mesh shader in place of a vertex shader: it is run
	 * with DISPATCH_MESH and reads no vertex buffers. */
	bool is_mesh{false};
} aprend_graphics_pipeline_t;

typedef struct aprend_compute_pipeline_t {
#if _DEBUG
	char *debug_name{nullptr};
#endif
	aprend_compute_pipeline_t() = default;
	~aprend_compute_pipeline_t();
	aprend_compute_pipeline_desc desc{};
	aprend_instance instance{nullptr};
	spudgpu_compute_pipeline pipeline{nullptr};
	/* The pipeline's copy of the entry point's name; desc's pointer points
	 * at it, not at the caller's string. */
	char *entry_point{nullptr};
	/* Where the furthest push constant range ends; 0 with no ranges. */
	uint32_t push_constant_end{0};
} aprend_compute_pipeline_t;

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
	/* For aprend_instance_retire when the handle is destroyed. */
	aprend_instance instance{nullptr};
	aprend_binding_layout layout{nullptr};
	/* From the layout's pools; each goes back on its free list. One set if
	 * every entry is a single buffer, texture or sampler. If any entry is a
	 * buffer set there is one for each frame in flight, sets[f] pointing at
	 * copy f of every buffer set and at the same single resources. */
	spudgpu_descriptor_set sets[APREND_MAX_FRAMES_IN_FLIGHT]{};
	uint32_t frame_set_count{0};
	/* Every texture view the set holds and the layout its slot reads the
	 * texture in, for a command list to move the texture there before a pass
	 * that binds the set. */
	struct image_use {
		aprend_texture_view view;
		SPUDGPU_IMAGE_LAYOUT layout;
	};
	image_use *image_uses{nullptr};
	uint32_t image_use_count{0};
	/* The store of every storage buffer the set holds, staged or not. A
	 * command list copies the staged ones before it runs, and orders every
	 * one of them against earlier writes before a pass or a dispatch that
	 * reads the set (a shader may have written it). One run of
	 * storage_store_capacity slots for each of the frame_set_count sets, run
	 * f holding storage_store_counts[f] stores: what sets[f] reads. The same
	 * buffer in two slots is listed twice. */
	aprend_buffer_store **storage_stores{nullptr};
	uint32_t storage_store_capacity{0};
	uint32_t storage_store_counts[APREND_MAX_FRAMES_IN_FLIGHT]{};
} aprend_binding_set_t;

/* Which of [set]'s descriptor sets a command list compiled in frame [frame]
 * binds: the frame's own if the set has one for each frame, its only one
 * otherwise. */
inline uint32_t aprend_binding_set_frame_slot(const aprend_binding_set_t *set, uint32_t frame) {
	return set->frame_set_count > 1 ? frame : 0;
}

#endif // APREND_INTERNAL_HPP
