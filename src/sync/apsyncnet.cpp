#include "sync/apsyncnet.h"
#include "apsync_internal.hpp"

#include <cstring>
#include <string>
#include <utility>

// ============================================================
// Byte buffer encode / decode
//
// Every field is appended/read individually at its natural size, never a
// raw struct memcpy, so the wire layout never depends on compiler padding.
// See APSYNC_MSG_TYPE in apsync_internal.hpp for the byte-order note.
// ============================================================

namespace {

void put_u32(
    std::vector<uint8_t> &buf,
    uint32_t v) {
	const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
	buf.insert(buf.end(), p, p + sizeof(v));
}
void put_u64(
    std::vector<uint8_t> &buf,
    uint64_t v) {
	const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
	buf.insert(buf.end(), p, p + sizeof(v));
}
void put_f32(
    std::vector<uint8_t> &buf,
    float v) {
	const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
	buf.insert(buf.end(), p, p + sizeof(v));
}
void put_f64(
    std::vector<uint8_t> &buf,
    double v) {
	const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
	buf.insert(buf.end(), p, p + sizeof(v));
}
void put_dvec3(
    std::vector<uint8_t> &buf,
    const ApriDVec3 &v) {
	put_f64(buf, v.x);
	put_f64(buf, v.y);
	put_f64(buf, v.z);
}
void put_quat(
    std::vector<uint8_t> &buf,
    const ApriQuat &v) {
	put_f32(buf, v.x);
	put_f32(buf, v.y);
	put_f32(buf, v.z);
	put_f32(buf, v.w);
}
void put_vec3(
    std::vector<uint8_t> &buf,
    const ApriVec3 &v) {
	put_f32(buf, v.x);
	put_f32(buf, v.y);
	put_f32(buf, v.z);
}
void put_bytes(
    std::vector<uint8_t> &buf,
    const void *data,
    uint64_t size) {
	const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
	buf.insert(buf.end(), p, p + size);
}

// A cursor over a received payload. Every get_* advances `pos` and returns
// false (leaving *out untouched) if the payload doesn't have enough bytes
// left, so a truncated or malformed message fails safe instead of reading
// out of bounds.
struct reader {
	const uint8_t *data;
	uint64_t size;
	uint64_t pos{0};

	bool get_u32(uint32_t *out) {
		if (pos + sizeof(uint32_t) > size)
			return false;
		std::memcpy(out, data + pos, sizeof(uint32_t));
		pos += sizeof(uint32_t);
		return true;
	}
	bool get_u64(uint64_t *out) {
		if (pos + sizeof(uint64_t) > size)
			return false;
		std::memcpy(out, data + pos, sizeof(uint64_t));
		pos += sizeof(uint64_t);
		return true;
	}
	bool get_f32(float *out) {
		if (pos + sizeof(float) > size)
			return false;
		std::memcpy(out, data + pos, sizeof(float));
		pos += sizeof(float);
		return true;
	}
	bool get_f64(double *out) {
		if (pos + sizeof(double) > size)
			return false;
		std::memcpy(out, data + pos, sizeof(double));
		pos += sizeof(double);
		return true;
	}
	bool get_dvec3(ApriDVec3 *out) { return get_f64(&out->x) && get_f64(&out->y) && get_f64(&out->z); }
	bool get_quat(ApriQuat *out) { return get_f32(&out->x) && get_f32(&out->y) && get_f32(&out->z) && get_f32(&out->w); }
	bool get_vec3(ApriVec3 *out) { return get_f32(&out->x) && get_f32(&out->y) && get_f32(&out->z); }
	// Returns a pointer into `data` (not a copy); only valid as long as
	// `data` is.
	bool get_bytes(
	    uint64_t byte_count,
	    const uint8_t **out) {
		if (pos + byte_count > size)
			return false;
		*out = data + pos;
		pos += byte_count;
		return true;
	}
};

void encode_node_entry(
    std::vector<uint8_t> &buf,
    const apsync_node_entry &e) {
	put_u64(buf, e.node_id);
	put_dvec3(buf, e.last_translation);
	put_quat(buf, e.last_rotation);
	put_vec3(buf, e.last_scale);
	put_u64(buf, (uint64_t)e.solid_blob.size());
	put_bytes(buf, e.solid_blob.data(), e.solid_blob.size());
}

// Fills every field except `node`, which the caller creates. false means
// the payload was truncated/malformed.
bool decode_node_entry(
    reader &r,
    uint64_t *node_id,
    ApriDVec3 *translation,
    ApriQuat *rotation,
    ApriVec3 *scale,
    std::vector<uint8_t> *blob) {
	uint64_t blob_size      = 0;
	const uint8_t *blob_ptr = nullptr;
	if (!r.get_u64(node_id))
		return false;
	if (!r.get_dvec3(translation))
		return false;
	if (!r.get_quat(rotation))
		return false;
	if (!r.get_vec3(scale))
		return false;
	if (!r.get_u64(&blob_size))
		return false;
	if (!r.get_bytes(blob_size, &blob_ptr))
		return false;
	blob->assign(blob_ptr, blob_ptr + blob_size);
	return true;
}

std::vector<uint8_t> encode_snapshot(const apsync_session_t *session) {
	std::vector<uint8_t> buf;
	put_u32(buf, (uint32_t)session->nodes.size());
	for (const auto &kv : session->nodes)
		encode_node_entry(buf, kv.second);
	return buf;
}

std::vector<uint8_t> encode_transform_update(
    uint64_t node_id,
    const ApriDVec3 &t,
    const ApriQuat &r,
    const ApriVec3 &s) {
	std::vector<uint8_t> buf;
	put_u64(buf, node_id);
	put_dvec3(buf, t);
	put_quat(buf, r);
	put_vec3(buf, s);
	return buf;
}

// ============================================================
// Framing over spudnet
// ============================================================

// Blocking helpers -- used only for the one-time snapshot handshake
// (accept -> send snapshot, connect -> receive snapshot), never on the
// per-frame non-blocking path.

bool send_exact_blocking(
    spudnet_socket sock,
    const void *data,
    uint64_t size) {
	uint64_t sent_total = 0;
	const uint8_t *src  = (const uint8_t *)data;
	while (sent_total < size) {
		uint64_t sent = 0;
		if (spudnet_send(sock, src + sent_total, size - sent_total, &sent) != SPUD_SUCCESS)
			return false;
		sent_total += sent;
	}
	return true;
}

bool recv_exact_blocking(
    spudnet_socket sock,
    void *data,
    uint64_t size) {
	uint64_t received_total = 0;
	uint8_t *dst            = (uint8_t *)data;
	while (received_total < size) {
		uint64_t received = 0;
		if (spudnet_recv(sock, dst + received_total, size - received_total, &received) != SPUD_SUCCESS)
			return false;
		if (received == 0)
			return false; // peer closed mid-message
		received_total += received;
	}
	return true;
}

bool send_message_blocking(
    spudnet_socket sock,
    uint32_t type,
    const std::vector<uint8_t> &payload) {
	uint32_t payload_size = (uint32_t)payload.size();
	return send_exact_blocking(sock, &type, sizeof(type)) && send_exact_blocking(sock, &payload_size, sizeof(payload_size)) &&
	       (payload.empty() || send_exact_blocking(sock, payload.data(), payload.size()));
}

bool recv_message_blocking(
    spudnet_socket sock,
    uint32_t *out_type,
    std::vector<uint8_t> *out_payload) {
	uint32_t header[2];
	if (!recv_exact_blocking(sock, header, sizeof(header)))
		return false;
	*out_type = header[0];
	out_payload->resize(header[1]);
	if (header[1] > 0 && !recv_exact_blocking(sock, out_payload->data(), header[1]))
		return false;
	return true;
}

// Non-blocking send used on the per-frame path. A short/failed write drops
// the rest of the message -- acceptable for a first pass since transform
// updates are small and frequent (the next poll's diff supersedes a lost
// one), but a slow reader's socket send buffer filling up mid-snapshot-
// relay is a real risk a production version would want a per-peer
// outgoing queue for instead of dropping.
void send_message_nonblocking(
    spudnet_socket sock,
    uint32_t type,
    const std::vector<uint8_t> &payload) {
	std::vector<uint8_t> full;
	put_u32(full, type);
	put_u32(full, (uint32_t)payload.size());
	full.insert(full.end(), payload.begin(), payload.end());

	uint64_t sent_total = 0;
	while (sent_total < full.size()) {
		uint64_t sent = 0;
		if (spudnet_send(sock, full.data() + sent_total, full.size() - sent_total, &sent) != SPUD_SUCCESS)
			return;
		sent_total += sent;
	}
}

// Pulls whatever bytes are currently available (non-blocking) into
// peer.recv_buffer, then peels off as many complete messages as are fully
// buffered. Returns false if the connection died (closed cleanly or
// errored) -- the caller drops the peer in that case.
bool pump_peer_messages(
    apsync_peer &peer,
    std::vector<std::pair<
        uint32_t,
        std::vector<uint8_t>>> &out_messages) {
	uint8_t chunk[4096];
	for (;;) {
		uint64_t received = 0;
		SPUDRESULT r      = spudnet_recv(peer.socket, chunk, sizeof(chunk), &received);
		if (r == SPUDRESULT_SNET_WOULD_BLOCK)
			break;
		if (r != SPUD_SUCCESS)
			return false;
		if (received == 0)
			return false; // peer closed cleanly
		peer.recv_buffer.insert(peer.recv_buffer.end(), chunk, chunk + received);
	}

	uint64_t offset = 0;
	while (peer.recv_buffer.size() - offset >= 8) {
		uint32_t type = 0, payload_size = 0;
		std::memcpy(&type, peer.recv_buffer.data() + offset, 4);
		std::memcpy(&payload_size, peer.recv_buffer.data() + offset + 4, 4);
		if (peer.recv_buffer.size() - offset < 8ull + payload_size)
			break; // message not fully buffered yet -- wait for the next poll

		out_messages.emplace_back(type, std::vector<uint8_t>(peer.recv_buffer.begin() + offset + 8, peer.recv_buffer.begin() + offset + 8 + payload_size));
		offset += 8 + payload_size;
	}
	if (offset > 0)
		peer.recv_buffer.erase(peer.recv_buffer.begin(), peer.recv_buffer.begin() + offset);

	return true;
}

aprend_node create_synced_node(
    apsync_session_t *session,
    uint64_t node_id) {
	return aprend_node_create(session->scene, ("apsync_node_" + std::to_string(node_id)).c_str());
}

} // namespace

extern "C" {

// ============================================================
// Public API
// ============================================================

apsync_session apsync_host_create(
    aprend_scene scene,
    uint16_t port) {
	if (!scene)
		return nullptr;

	spudnet_socket listen_sock = nullptr;
	if (spudnet_listen_create(port, &listen_sock) != SPUD_SUCCESS)
		return nullptr;
	spudnet_set_blocking(listen_sock, false);

	apsync_session_t *session = new apsync_session_t();
	session->role             = APSYNC_ROLE_HOST;
	session->scene            = scene;
	session->listen_socket    = listen_sock;
	return session;
}

apsync_session apsync_join(
    aprend_scene scene,
    const char *host,
    uint16_t port,
    apsync_attach_content attach_content,
    void *user_data) {
	if (!scene || !host || !attach_content)
		return nullptr;

	spudnet_socket sock = nullptr;
	if (spudnet_connect(host, port, &sock) != SPUD_SUCCESS)
		return nullptr;

	// Still blocking here (spudnet_connect leaves it that way) -- fine,
	// this handshake is a one-time startup step, not the per-frame path.
	uint32_t type = 0;
	std::vector<uint8_t> payload;
	if (!recv_message_blocking(sock, &type, &payload) || type != APSYNC_MSG_SNAPSHOT) {
		spudnet_close(sock);
		return nullptr;
	}

	apsync_session_t *session         = new apsync_session_t();
	session->role                     = APSYNC_ROLE_CLIENT;
	session->scene                    = scene;
	session->attach_content           = attach_content;
	session->attach_content_user_data = user_data;
	session->next_node_id             = (uint64_t)1 << 63; // see apsync_internal.hpp

	reader r{payload.data(), payload.size()};
	uint32_t node_count = 0;
	if (!r.get_u32(&node_count)) {
		spudnet_close(sock);
		delete session;
		return nullptr;
	}

	for (uint32_t i = 0; i < node_count; ++i) {
		uint64_t node_id = 0;
		ApriDVec3 translation{};
		ApriQuat rotation{};
		ApriVec3 scale{};
		std::vector<uint8_t> blob;
		if (!decode_node_entry(r, &node_id, &translation, &rotation, &scale, &blob)) {
			spudnet_close(sock);
			delete session;
			return nullptr;
		}

		aprend_node node = create_synced_node(session, node_id);
		aprend_node_set_translation(node, translation);
		aprend_node_set_rotation(node, rotation);
		aprend_node_set_scale(node, scale);
		attach_content(user_data, node, blob.data(), blob.size());

		apsync_node_entry entry{node_id, node, translation, rotation, scale, std::move(blob)};
		session->nodes.emplace(node_id, std::move(entry));
	}

	spudnet_set_blocking(sock, false);
	session->peers.push_back({sock, {}});
	return session;
}

uint64_t apsync_register_node(
    apsync_session session,
    aprend_node node,
    const void *solid_blob,
    uint64_t solid_blob_size) {
	if (!session || !node)
		return 0;

	apsync_node_entry entry;
	entry.node_id          = session->next_node_id++;
	entry.node             = node;
	entry.last_translation = aprend_node_get_translation(node);
	entry.last_rotation    = aprend_node_get_rotation(node);
	entry.last_scale       = aprend_node_get_scale(node);
	entry.solid_blob.assign((const uint8_t *)solid_blob, (const uint8_t *)solid_blob + solid_blob_size);

	// Only the host can introduce a node the rest of the session learns
	// about -- see the note on apsync_register_node in apsyncnet.h.
	if (session->role == APSYNC_ROLE_HOST && !session->peers.empty()) {
		std::vector<uint8_t> msg;
		encode_node_entry(msg, entry);
		for (auto &peer : session->peers)
			send_message_nonblocking(peer.socket, APSYNC_MSG_NODE_ADDED, msg);
	}

	uint64_t node_id = entry.node_id;
	session->nodes.emplace(node_id, std::move(entry));
	return node_id;
}

void apsync_unregister_node(
    apsync_session session,
    uint64_t node_id) {
	if (!session)
		return;
	session->nodes.erase(node_id);
}

void apsync_poll(apsync_session session) {
	if (!session)
		return;

	if (session->role == APSYNC_ROLE_HOST) {
		for (;;) {
			spudnet_socket client_sock = nullptr;
			SPUDRESULT r               = spudnet_accept(session->listen_socket, &client_sock);
			if (r == SPUDRESULT_SNET_WOULD_BLOCK)
				break;
			if (r != SPUD_SUCCESS)
				break; // listen socket unusable this frame

			// Blocking for this one-time send -- see the note on
			// send_message_nonblocking above for why a slow joiner here is
			// an accepted risk for now.
			spudnet_set_blocking(client_sock, true);
			bool ok = send_message_blocking(client_sock, APSYNC_MSG_SNAPSHOT, encode_snapshot(session));
			spudnet_set_blocking(client_sock, false);

			if (ok)
				session->peers.push_back({client_sock, {}});
			else
				spudnet_close(client_sock);
		}
	}

	for (size_t i = 0; i < session->peers.size();) {
		apsync_peer &peer = session->peers[i];
		std::vector<std::pair<uint32_t, std::vector<uint8_t>>> messages;
		bool alive = pump_peer_messages(peer, messages);

		for (auto &msg : messages) {
			reader r{msg.second.data(), msg.second.size()};

			if (msg.first == APSYNC_MSG_TRANSFORM_UPDATE) {
				uint64_t node_id = 0;
				ApriDVec3 translation{};
				ApriQuat rotation{};
				ApriVec3 scale{};
				if (!r.get_u64(&node_id) || !r.get_dvec3(&translation) || !r.get_quat(&rotation) || !r.get_vec3(&scale))
					continue; // malformed -- ignore, keep the connection

				auto it = session->nodes.find(node_id);
				if (it == session->nodes.end())
					continue; // update for a node we don't know about yet

				apsync_node_entry &entry = it->second;
				aprend_node_set_translation(entry.node, translation);
				aprend_node_set_rotation(entry.node, rotation);
				aprend_node_set_scale(entry.node, scale);
				entry.last_translation = translation;
				entry.last_rotation    = rotation;
				entry.last_scale       = scale;

				if (session->role == APSYNC_ROLE_HOST) {
					std::vector<uint8_t> relay = encode_transform_update(node_id, translation, rotation, scale);
					for (size_t j = 0; j < session->peers.size(); ++j)
						if (j != i)
							send_message_nonblocking(session->peers[j].socket, APSYNC_MSG_TRANSFORM_UPDATE, relay);
				}
			} else if (msg.first == APSYNC_MSG_NODE_ADDED && session->role == APSYNC_ROLE_CLIENT) {
				uint64_t node_id = 0;
				ApriDVec3 translation{};
				ApriQuat rotation{};
				ApriVec3 scale{};
				std::vector<uint8_t> blob;
				if (!decode_node_entry(r, &node_id, &translation, &rotation, &scale, &blob))
					continue;
				if (session->nodes.count(node_id))
					continue; // already known -- ignore a duplicate

				aprend_node node = create_synced_node(session, node_id);
				aprend_node_set_translation(node, translation);
				aprend_node_set_rotation(node, rotation);
				aprend_node_set_scale(node, scale);
				session->attach_content(session->attach_content_user_data, node, blob.data(), blob.size());

				apsync_node_entry entry{node_id, node, translation, rotation, scale, std::move(blob)};
				session->nodes.emplace(node_id, std::move(entry));
			}
		}

		if (!alive) {
			spudnet_close(peer.socket);
			session->peers.erase(session->peers.begin() + i);
			if (session->role == APSYNC_ROLE_CLIENT)
				session->connected = false;
			continue; // don't advance i -- the next element shifted into this slot
		}
		++i;
	}

	for (auto &kv : session->nodes) {
		apsync_node_entry &entry = kv.second;
		ApriDVec3 translation    = aprend_node_get_translation(entry.node);
		ApriQuat rotation        = aprend_node_get_rotation(entry.node);
		ApriVec3 scale           = aprend_node_get_scale(entry.node);

		bool changed = std::memcmp(&translation, &entry.last_translation, sizeof(translation)) != 0 ||
		               std::memcmp(&rotation, &entry.last_rotation, sizeof(rotation)) != 0 || std::memcmp(&scale, &entry.last_scale, sizeof(scale)) != 0;
		if (!changed)
			continue;

		entry.last_translation = translation;
		entry.last_rotation    = rotation;
		entry.last_scale       = scale;

		std::vector<uint8_t> msg = encode_transform_update(kv.first, translation, rotation, scale);
		for (auto &peer : session->peers)
			send_message_nonblocking(peer.socket, APSYNC_MSG_TRANSFORM_UPDATE, msg);
	}
}

bool apsync_is_connected(apsync_session session) {
	if (!session)
		return false;
	if (session->role == APSYNC_ROLE_HOST)
		return session->listen_socket != nullptr;
	return session->connected;
}

void apsync_session_destroy(apsync_session session) {
	if (!session)
		return;
	if (session->listen_socket)
		spudnet_close(session->listen_socket);
	for (auto &peer : session->peers)
		spudnet_close(peer.socket);
	delete session;
}

} // Extern "C"
