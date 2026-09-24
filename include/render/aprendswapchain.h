
#ifndef APREND_SWAP_CHAIN_H
#define APREND_SWAP_CHAIN_H

#include "aprendcontext.h"

/****************************************************
 * Apricot Render Swap Chain
 *
 * Presents finished images to a window. A frame that presents goes:
 *   aprend_swap_chain_acquire
 *   -> record + compile a command list ending in APREND_COMMAND_PRESENT_TEXTURE
 *   -> aprend_command_list_submit (on the swap chain's queue)
 *   -> aprend_swap_chain_present
 * Each step fails (returns false) when called out of that order.
 ****************************************************/

#if __cplusplus
extern "C" {
#endif

typedef struct aprend_swap_chain_desc {
	/* Created by the caller from its window (spudgpu_create_surface), and
	 * still owned by it: it must outlive the swap chain. */
	spudgpu_surface surface;
	/* The queue that presents, and that lists presenting into this swap chain
	 * must be submitted on. */
	spudgpu_command_queue queue;
	uint32_t width;
	uint32_t height;
	uint32_t buffer_count;
	SPUDGPU_FORMAT format;
	SPUDGPU_PRESENT_MODE present_mode;
} aprend_swap_chain_desc;

typedef struct aprend_swap_chain_t *aprend_swap_chain;

aprend_swap_chain aprend_swap_chain_create(
    aprend_instance instance,
    const aprend_swap_chain_desc *desc);
/* Waits for the queue to go idle first, so nothing in flight still uses it. */
void aprend_swap_chain_destroy(aprend_swap_chain swap_chain);
/* Recreates the back buffers at the new size (waits for the queue to go idle
 * first). Drops any acquired, not yet presented back buffer. */
bool aprend_swap_chain_resize(
    aprend_swap_chain swap_chain,
    uint32_t width,
    uint32_t height);
bool aprend_swap_chain_get_desc(
    aprend_swap_chain swap_chain,
    aprend_swap_chain_desc *out_desc);

/* Acquires the next back buffer. Call before compiling the list that
 * presents into it. If a back buffer is already acquired but nothing
 * presenting into it was submitted, that one is kept. */
bool aprend_swap_chain_acquire(aprend_swap_chain swap_chain);
/* Presents the acquired back buffer, after the list that presents into it has
 * been submitted. */
bool aprend_swap_chain_present(aprend_swap_chain swap_chain);

#if __cplusplus
}
#endif

#endif // APREND_SWAP_CHAIN_H
