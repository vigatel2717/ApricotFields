
#ifndef APREND_BASE_H
#define APREND_BASE_H

#include <spudgpu.h>

#if __cplusplus
extern "C" {
#endif

/* Upper bound on aprend_instance_desc::frames_in_flight. */
#define APREND_MAX_FRAMES_IN_FLIGHT 4

typedef struct aprend_instance_desc {
	/* The device everything of this instance is created on. Never NULL. It is
	 * the caller's: it must outlive the instance and is not destroyed with it. */
	spudgpu_device device;
	/* How many frames the caller lets the GPU hold at once: how many it
	 * submits before it has to know the oldest has finished. 1 to
	 * APREND_MAX_FRAMES_IN_FLIGHT, and 0 is refused. A buffer set
	 * (aprendbuffers.h) keeps this many copies of its buffer, one for each
	 * frame, so the frame being written never shares a copy with one the GPU
	 * is still reading. 1 is a caller that waits for each frame to finish
	 * before starting the next. */
	uint32_t frames_in_flight;
} aprend_instance_desc;

typedef struct aprend_instance_t *aprend_instance;
aprend_instance aprend_instance_create(const aprend_instance_desc *desc);

/* The frame index runs 0 to frames_in_flight - 1 and starts at 0. It says
 * which copy of every buffer set is the current one: the one the update
 * calls write, and the one a command list compiled now reads.
 *
 * aprend_instance_next_frame moves to the next index, wrapping to 0, and
 * returns it. The caller calls it once at the start of each frame after the
 * first, before writing any buffer set or compiling any command list for
 * that frame.
 *
 * It blocks until everything aprend_command_list_submit submitted the last
 * time this index was current has finished: the frame from frames_in_flight
 * frames ago. So at most frames_in_flight frames are ever on the GPU, and
 * the caller needs no wait of its own between frames. With frames_in_flight
 * 1 it waits for the frame before. If the wait fails the index has still
 * moved, and the failure is printed.
 *
 * What it waits for is what went through Aprend, all on one queue:
 * aprend_command_list_submit and the calls that submit at once (texture
 * update and readback, framebuffer clears). Work the caller submits to
 * SpudGPU itself is not counted.
 *
 * Returns 0 if [instance] is NULL. Not safe to call while another thread
 * writes a buffer set or compiles or submits a command list. */
uint32_t aprend_instance_next_frame(aprend_instance instance);
/* The current frame index; 0 if [instance] is NULL. */
uint32_t aprend_instance_get_frame_index(aprend_instance instance);

/* Blocks until everything submitted through [instance] so far has finished:
 * every aprend_command_list_submit, and every texture update, readback and
 * framebuffer clear (which have finished by the time they return anyway).
 * For the points where nothing may be in flight - before destroying or
 * resizing what submitted work uses, and at shutdown. It waits for Aprend's
 * own submissions and nothing else on the queue: work the caller submits to
 * SpudGPU itself is the caller's to wait for.
 *
 * False if [instance] is NULL or the wait fails. Not safe to call while
 * another thread submits. */
bool aprend_instance_wait_idle(aprend_instance instance);
bool aprend_instance_get_desc(
    aprend_instance instance,
    aprend_instance_desc *out_desc);
void aprend_instance_destroy(aprend_instance instance);

#if __cplusplus
}
#endif

#endif // APREND_BASE_H
