
#ifndef APSYNC_NET_H
#define APSYNC_NET_H

#include "render/aprendscene.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ApSync  —  multi-device live scene sync
 *
 * One device hosts (apsync_host_create): it opens a listening socket and
 * becomes the scene's source of truth. Other devices join
 * (apsync_join): each pulls a full snapshot on connect, then every
 * device's local edits to a registered node's transform flow to every
 * other device through the host, which relays what it doesn't already
 * know about.
 *
 * A node's *content* (its ApCAD solid, geometry, mesh) is never apsync's
 * concern — that's opaque bytes ("solid_blob") the caller attaches
 * meaning to. apsync only owns node lifecycle for reconstructed nodes and
 * the transform on every registered node. See apsync_attach_content.
 */

typedef struct apsync_session_t *apsync_session;

/* Builds whatever content `solid_blob` describes (ApCAD solid -> geometry
 * -> mesh, typically) and attaches it to `node`. `node` was just created
 * by apsync under `scene` and already has its synced transform applied;
 * the callback's only job is giving it something to look like.
 *
 * Called once per node in the initial snapshot (apsync_join) and once
 * whenever a node registered elsewhere is relayed to this session after
 * that (apsync_poll). Never called for nodes this session registered
 * itself — the caller already built those. */
typedef void (*apsync_attach_content)(
    void *user_data,
    aprend_node node,
    const void *solid_blob,
    uint64_t solid_blob_size);

/*
 * Host  —  opens first, holds the scene, accepts joiners.
 */

/* Binds a listening socket on `port`. Nodes registered on `scene` before
 * or after this call (via apsync_register_node) are what gets sent to
 * anyone who joins. NULL on failure (see spudnet_listen_create). */
apsync_session apsync_host_create(
    aprend_scene scene,
    uint16_t port);

/*
 * Client  —  joins a running host.
 */

/* Connects to host:port and blocks until the initial snapshot arrives.
 * Creates one node per entry in it (parented directly under `scene`) and
 * invokes `attach_content` for each before returning. NULL on failure
 * (unreachable host, connection dropped mid-snapshot, etc). */
apsync_session apsync_join(
    aprend_scene scene,
    const char *host,
    uint16_t port,
    apsync_attach_content attach_content,
    void *user_data);

/*
 * Both roles, once a session exists.
 */

/* Opts an existing local node into sync: apsync starts tracking its
 * transform for changes and (if this session is hosting) makes it part
 * of the snapshot sent to future joiners, broadcasting it immediately to
 * anyone already connected. `solid_blob` is copied; the caller doesn't
 * need to keep it alive. Returns a session-wide node id used in
 * apsync_unregister_node.
 *
 * Calling this on a joined (non-hosting) session only affects local
 * tracking — it does not introduce a new node to the host or other
 * peers. Only the host can add nodes the rest of the session learns
 * about; a client can only sync transform changes on nodes it already
 * knows about (its own or ones relayed from the host). */
uint64_t apsync_register_node(
    apsync_session session,
    aprend_node node,
    const void *solid_blob,
    uint64_t solid_blob_size);

void apsync_unregister_node(
    apsync_session session,
    uint64_t node_id);

/* Call once per frame, host or client alike. Non-blocking — safe from a
 * render loop. Host: accepts new connections and snapshots them, applies
 * and relays transform updates received from any client to every other
 * client. Client: applies transform updates and newly-relayed nodes
 * received from the host. Both: diffs every registered node's current
 * world transform against what was last sent/received and relays any
 * change onward (broadcast if hosting, send-to-host if joined). */
void apsync_poll(apsync_session session);

/* Host: whether the listen socket is still open. Client: whether the
 * connection to the host is still alive. */
bool apsync_is_connected(apsync_session session);

void apsync_session_destroy(apsync_session session);

#ifdef __cplusplus
}
#endif

#endif // APSYNC_NET_H
