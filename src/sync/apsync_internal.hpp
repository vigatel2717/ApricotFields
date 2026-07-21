#ifndef APSYNC_INTERNAL_HPP
#define APSYNC_INTERNAL_HPP

/* Internal header — never included outside ApricotFields/src/sync/.
 * Defines the concrete struct behind apsync_session and the wire opcodes
 * shared by every message-handling function in apsyncnet.cpp. */

#include "sync/apsyncnet.h"
#include <spudnet.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

enum apsync_role { APSYNC_ROLE_HOST, APSYNC_ROLE_CLIENT };

/* Every message on the wire is [uint32 type][uint32 payload_size][payload].
 * Every field inside a payload is written individually at its natural size
 * (never a raw struct memcpy) so the layout never depends on compiler
 * padding. Multi-byte values are host byte order — every platform this
 * currently targets (x64, ARM64) is little-endian, so this is a
 * deliberate simplification, not an oversight. */
enum APSYNC_MSG_TYPE : uint32_t {
	APSYNC_MSG_SNAPSHOT         = 1, // host -> a client that just joined
	APSYNC_MSG_NODE_ADDED       = 2, // host -> clients: one new node
	APSYNC_MSG_TRANSFORM_UPDATE = 3, // either direction: one node's transform
};

/* Last-synced state for one registered node — lets apsync_poll tell a
 * fresh local edit apart from an update it just applied from the network,
 * and lets a host re-describe this node in a snapshot for later joiners. */
struct apsync_node_entry {
	uint64_t             node_id;
	aprend_node          node;
	ApriDVec3            last_translation;
	ApriQuat             last_rotation;
	ApriVec3             last_scale;
	std::vector<uint8_t> solid_blob;
};

/* One TCP connection. Host: one entry per connected client. Client:
 * exactly one entry, the connection to the host. */
struct apsync_peer {
	spudnet_socket       socket;
	std::vector<uint8_t> recv_buffer; // partial-message reassembly across polls
};

struct apsync_session_t {
	apsync_role  role;
	aprend_scene scene;

	spudnet_socket listen_socket{nullptr}; // host only

	// Host: every connected client. Client: exactly one entry, the host.
	std::vector<apsync_peer> peers;
	bool connected{true}; // client only: false once the host connection drops

	apsync_attach_content attach_content{nullptr};
	void *attach_content_user_data{nullptr};

	/* Host-assigned ids live in [1, 2^63) — the shared id space everyone
	 * agrees on, since only the host's snapshot/NODE_ADDED messages ever
	 * introduce a node to more than one machine. A client-local
	 * apsync_register_node call (tracked here but never broadcast — see
	 * apsyncnet.h) draws from [2^63, 2^64) instead, so it can never collide
	 * with an id the host might assign. */
	uint64_t next_node_id{1};

	std::unordered_map<uint64_t, apsync_node_entry> nodes;
};

#endif // APSYNC_INTERNAL_HPP
