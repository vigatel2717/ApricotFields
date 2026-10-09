#include "apsync_internal.hpp"
#include <algorithm>
#include <iterator>
#include <new>
#include <stdexcept>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Sessions, channels and snapshots over SpudNet's TCP sockets. What isn't
// here yet, and what SpudNet still owes it, is in APSYNC_TODO.md.
//
// Who runs what. Each session has one thread of ApSync's, and every SpudNet
// object the session has is that thread's alone: it waits on all of them in
// one wait set and is the only one to send, receive and accept. The caller's
// thread never touches the network. The two meet in two queues - commands
// going in (the outbox) and events coming out (the inbox) - and the caller
// ends the thread's wait with spudnet_wait_set_wake when it has put
// something in the first.
//
// Nothing below trusts a timing or an ordering SpudNet's backends haven't
// been seen to give (SPUDNET_TODO.md: only macOS has run, and only over
// loopback). In particular:
// - Being named by the wait set is only a reason to try. Every receive and
//   accept is made with SPUDNET_NO_WAIT and repeated until it says
//   SPUDRESULT_SPUDNET_TIMED_OUT.
// - Any result but success and that one ends the connection it came from.
//   Which failure it was only chooses the words in the event.
// - A peer that has gone quiet is noticed by ApSync's own clock
//   (peer_timeout_ms), not by waiting for the system to report it.

// ---- Errors -------------------------------------------------------------------

[[maybe_unused]] static void clear_error(APSYNC_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// Records [format] in [error] (if any) and returns [result].
static APRESULT fail(APSYNC_ERROR *error, APRESULT result, const char *format, ...)
{
    if (error)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return result;
}

// ---- Struct versions ----------------------------------------------------------

// Copies a caller's versioned input struct into [out] at the current version:
// the fields its struct_size covers, zeros past them. False if it's smaller
// than the struct's first version.
template <typename T> static bool read_versioned(const T *in, size_t v1_size, T &out)
{
    if (!in || in->struct_size < v1_size)
        return false;
    memset(&out, 0, sizeof(T));
    memcpy(&out, in, in->struct_size < sizeof(T) ? in->struct_size : sizeof(T));
    out.struct_size = (uint32_t)sizeof(T);
    return true;
}

// Writes [value] into a caller's versioned output struct, up to its
// struct_size, which is kept as the caller set it.
template <typename T> static bool write_versioned(T *out, size_t v1_size, const T &value)
{
    if (!out || out->struct_size < v1_size)
        return false;
    uint32_t caller_size = out->struct_size;
    memcpy(out, &value, caller_size < sizeof(T) ? caller_size : sizeof(T));
    out->struct_size = caller_size;
    return true;
}

#if SPUDNET_EXT_TCP

// ---- Bytes --------------------------------------------------------------------

static void put_u8(apsync_bytes &out, uint8_t value)
{
    out.push_back(value);
}

static void put_u16(apsync_bytes &out, uint16_t value)
{
    out.push_back((uint8_t)value);
    out.push_back((uint8_t)(value >> 8));
}

static void put_u32(apsync_bytes &out, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back((uint8_t)(value >> shift));
}

static void put_u64(apsync_bytes &out, uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back((uint8_t)(value >> shift));
}

static void put_bytes(apsync_bytes &out, const void *data, size_t size)
{
    if (size == 0)
        return;
    const uint8_t *bytes = (const uint8_t *)data;
    out.insert(out.end(), bytes, bytes + size);
}

static uint32_t get_u32_at(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

// A cursor over one frame's payload. Each read fails, and takes nothing,
// when the payload hasn't that many bytes left.
struct apsync_reader
{
    const uint8_t *data;
    size_t size;
    size_t position{0};

    size_t left() const
    {
        return size - position;
    }
    const uint8_t *rest() const
    {
        return data + position;
    }
    bool u8(uint8_t &out)
    {
        if (left() < 1)
            return false;
        out = data[position++];
        return true;
    }
    bool u16(uint16_t &out)
    {
        if (left() < 2)
            return false;
        out = (uint16_t)(data[position] | (data[position + 1] << 8));
        position += 2;
        return true;
    }
    bool u32(uint32_t &out)
    {
        if (left() < 4)
            return false;
        out = get_u32_at(data + position);
        position += 4;
        return true;
    }
    bool u64(uint64_t &out)
    {
        if (left() < 8)
            return false;
        out = (uint64_t)get_u32_at(data + position) | ((uint64_t)get_u32_at(data + position + 4) << 32);
        position += 8;
        return true;
    }
};

// ---- Frames -------------------------------------------------------------------

// A frame with its type written and its length still to be: fill in the
// payload, then finish_frame.
static apsync_bytes begin_frame(APSYNC_FRAME type, size_t payload_size)
{
    apsync_bytes frame;
    frame.reserve(5 + payload_size);
    frame.resize(4);
    frame.push_back((uint8_t)type);
    return frame;
}

static void write_frame_length(apsync_bytes &frame, uint32_t length)
{
    frame[0] = (uint8_t)length;
    frame[1] = (uint8_t)(length >> 8);
    frame[2] = (uint8_t)(length >> 16);
    frame[3] = (uint8_t)(length >> 24);
}

static apsync_shared_bytes finish_frame(apsync_bytes &&frame)
{
    write_frame_length(frame, (uint32_t)(frame.size() - 4));
    return std::make_shared<const apsync_bytes>(std::move(frame));
}

static apsync_out_item whole_item(apsync_shared_bytes bytes, bool counted)
{
    apsync_out_item item;
    item.end = bytes->size();
    item.bytes = std::move(bytes);
    item.counted = counted;
    return item;
}

static apsync_shared_bytes frame_peer_joined(APSYNC_PEER peer, const apsync_bytes &info)
{
    apsync_bytes frame = begin_frame(APSYNC_FRAME_PEER_JOINED, 4 + info.size());
    put_u32(frame, peer);
    put_bytes(frame, info.data(), info.size());
    return finish_frame(std::move(frame));
}

static apsync_shared_bytes frame_peer_left(APSYNC_PEER peer, APSYNC_REASON reason)
{
    apsync_bytes frame = begin_frame(APSYNC_FRAME_PEER_LEFT, 5);
    put_u32(frame, peer);
    put_u8(frame, (uint8_t)reason);
    return finish_frame(std::move(frame));
}

static apsync_shared_bytes frame_refuse(APSYNC_REASON reason)
{
    apsync_bytes frame = begin_frame(APSYNC_FRAME_REFUSE, 1);
    put_u8(frame, (uint8_t)reason);
    return finish_frame(std::move(frame));
}

static apsync_shared_bytes frame_latest(uint32_t channel, uint64_t key, APSYNC_PEER origin, const void *data,
                                        size_t size)
{
    apsync_bytes frame = begin_frame(APSYNC_FRAME_LATEST, 14 + size);
    put_u16(frame, (uint16_t)channel);
    put_u64(frame, key);
    put_u32(frame, origin);
    put_bytes(frame, data, size);
    return finish_frame(std::move(frame));
}

// A reason as it came off the wire, which may be from a newer ApSync.
static APSYNC_REASON reason_from_wire(uint8_t value)
{
    return value <= (uint8_t)APSYNC_REASON_INTERNAL ? (APSYNC_REASON)value : APSYNC_REASON_CLOSED;
}

// ---- SpudNet's error detail ---------------------------------------------------

static const char *error_source_name(SPUDNET_ERROR_SOURCE source)
{
    switch (source)
    {
    case SPUDNET_ERROR_SOURCE_WIN32:
        return "Win32";
    case SPUDNET_ERROR_SOURCE_ERRNO:
        return "errno";
    case SPUDNET_ERROR_SOURCE_RESOLVER:
        return "resolver";
    case SPUDNET_ERROR_SOURCE_CURL:
        return "libcurl";
    case SPUDNET_ERROR_SOURCE_NSURL:
        return "NSURLError";
    case SPUDNET_ERROR_SOURCE_OSSTATUS:
        return "OSStatus";
    default:
        return "platform";
    }
}

// "[what]: SpudNet result N", with the platform's own code after it when
// [detail] is the record of the call that returned [result]. Not every
// failure has one (spudnet.h, "Error detail").
static std::string describe(const char *what, SPUDRESULT result, const spudnet_error &detail)
{
    char text[APSYNC_ERROR_MESSAGE_SIZE];
    if (detail.source != SPUDNET_ERROR_SOURCE_NONE && detail.result == result)
        snprintf(text, sizeof(text), "%s: SpudNet result %d, %s code %lld%s%s", what, (int)result,
                 error_source_name(detail.source), (long long)detail.code, detail.text[0] ? " - " : "", detail.text);
    else
        snprintf(text, sizeof(text), "%s: SpudNet result %d", what, (int)result);
    return text;
}

static std::string describe_socket(spudnet_tcp_socket socket, SPUDNET_ERROR_SIDE side, const char *what,
                                   SPUDRESULT result)
{
    spudnet_error detail;
    spudnet_tcp_socket_get_error(socket, side, &detail);
    return describe(what, result, detail);
}

// ---- Events out ---------------------------------------------------------------

static void emit(apsync_session_t &s, apsync_queued_event &&event)
{
    {
        std::lock_guard<std::mutex> lock(s.inbox_mutex);
        s.inbox.push_back(std::move(event));
    }
    s.inbox_changed.notify_all();
}

static void emit_simple(apsync_session_t &s, APSYNC_EVENT_TYPE type, APSYNC_PEER peer, APSYNC_REASON reason,
                        std::string message)
{
    apsync_queued_event event;
    event.type = type;
    event.peer = peer;
    event.reason = reason;
    event.message = std::move(message);
    emit(s, std::move(event));
}

static void emit_message(apsync_session_t &s, APSYNC_PEER origin, uint32_t channel, uint64_t sequence, uint64_t key,
                         const uint8_t *data, size_t size)
{
    apsync_queued_event event;
    event.type = APSYNC_EVENT_MESSAGE;
    event.peer = origin;
    event.channel = channel;
    event.sequence = sequence;
    event.key = key;
    event.data.assign(data, data + size);
    emit(s, std::move(event));
}

// ---- One peer's queue ---------------------------------------------------------

static void drop_peer(apsync_session_t &s, apsync_peer &p, APSYNC_REASON reason, std::string message);

static bool is_live(const apsync_peer &p)
{
    return !p.dead && p.state == APSYNC_PEER_LIVE;
}

// Queues [item] for [p]. A latest value takes the place of an older one for
// its key that hasn't begun to go out. Hosting, a peer this puts over the
// backlog limit is dropped.
static void enqueue(apsync_session_t &s, apsync_peer &p, apsync_out_item item)
{
    if (p.dead)
        return;
    size_t size = item.end - item.begin;
    bool replaced = false;
    if (item.latest)
    {
        auto found = p.latest.find(item.key);
        if (found != p.latest.end())
        {
            auto waiting = found->second;
            bool started = waiting == p.out.begin() && p.front_sent > 0;
            if (!started)
            {
                p.out_bytes -= waiting->end - waiting->begin;
                *waiting = std::move(item);
                replaced = true;
            }
        }
        if (!replaced)
        {
            apsync_latest_key key = item.key;
            p.out.push_back(std::move(item));
            // The one already going out, if any, finishes; this is now the
            // one a newer value replaces.
            p.latest[key] = std::prev(p.out.end());
        }
        p.out_bytes += size;
    }
    else
    {
        if (item.counted)
            p.out_bytes += size;
        p.out.push_back(std::move(item));
    }
    if (s.hosting && p.out_bytes > s.max_backlog_size)
        drop_peer(s, p, APSYNC_REASON_BACKLOG, "");
}

// Asks the wait set about room on [p]'s socket, or stops asking. A wait
// set's members can't be changed in place, so it is taken out and put back.
static void set_waiting_for_room(apsync_session_t &s, apsync_peer &p, bool waiting)
{
    if (p.dead || !p.in_wait_set || p.waiting_for_room == waiting)
        return;
    spudnet_wait_set_remove_tcp_socket(s.wait_set, p.socket);
    p.in_wait_set = false;
    uint32_t wait_for = SPUDNET_WAIT_FOR_INCOMING | (waiting ? (uint32_t)SPUDNET_WAIT_FOR_ROOM : 0u);
    SPUDRESULT result =
        spudnet_wait_set_add_tcp_socket(s.wait_set, p.socket, wait_for, static_cast<apsync_wait_member *>(&p));
    if (result != SPUD_SUCCESS)
    {
        spudnet_error none{};
        drop_peer(s, p, APSYNC_REASON_INTERNAL, describe("putting a connection back in the wait set", result, none));
        return;
    }
    p.in_wait_set = true;
    p.waiting_for_room = waiting;
}

// Sends as much of [p]'s queue as its connection takes right now.
static void flush_peer(apsync_session_t &s, apsync_peer &p)
{
    while (!p.dead && !p.out.empty())
    {
        apsync_out_item &item = p.out.front();
        size_t size = item.end - item.begin;
        if (p.front_sent < size)
        {
            uint64_t sent = 0;
            SPUDRESULT result = spudnet_tcp_socket_send(p.socket, item.bytes->data() + item.begin + p.front_sent,
                                                        size - p.front_sent, SPUDNET_NO_WAIT, &sent);
            if (result == SPUDRESULT_SPUDNET_TIMED_OUT)
                break;
            if (result != SPUD_SUCCESS)
            {
                drop_peer(s, p, APSYNC_REASON_NETWORK,
                          describe_socket(p.socket, SPUDNET_ERROR_SIDE_SENDING, "sending", result));
                return;
            }
            p.front_sent += (size_t)sent;
            p.last_sent = apsync_clock::now();
            if (item.counted || item.latest)
                p.out_bytes -= sent;
        }
        if (p.front_sent >= size)
        {
            if (item.latest)
            {
                auto found = p.latest.find(item.key);
                if (found != p.latest.end() && found->second == p.out.begin())
                    p.latest.erase(found);
            }
            p.out.pop_front();
            p.front_sent = 0;
        }
    }
    if (!p.dead)
        set_waiting_for_room(s, p, !p.out.empty());
}

// Takes [p] out of the session. Its socket stays until the loop reaps it:
// events already handed back by the wait set may still name it.
static void drop_peer(apsync_session_t &s, apsync_peer &p, APSYNC_REASON reason, std::string message)
{
    if (p.dead)
        return;
    apsync_peer_state state = p.state;
    p.dead = true;

    if (!s.hosting)
    {
        emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE, reason, std::move(message));
        return;
    }
    if (state == APSYNC_PEER_LIVE)
    {
        apsync_shared_bytes frame = frame_peer_left(p.id, reason);
        for (auto &other : s.peers)
            if (is_live(*other))
                enqueue(s, *other, whole_item(frame, true));
    }
    // A newcomer that never said what it is was never anybody; nobody is
    // told it went.
    if (state == APSYNC_PEER_LIVE || state == APSYNC_PEER_AWAITING_SNAPSHOT)
        emit_simple(s, APSYNC_EVENT_PEER_LEFT, p.id, reason, std::move(message));
}

// ---- Hosting: what the session does with a message ----------------------------

// Gives an ordered message its place and sends it to everyone, its sender
// and the host application included.
static void sequence_and_send(apsync_session_t &s, APSYNC_PEER origin, uint32_t channel, const uint8_t *data,
                              size_t size)
{
    uint64_t sequence = ++s.sequence;
    apsync_bytes built = begin_frame(APSYNC_FRAME_ORDERED, 14 + size);
    put_u16(built, (uint16_t)channel);
    put_u64(built, sequence);
    put_u32(built, origin);
    put_bytes(built, data, size);
    apsync_shared_bytes frame = finish_frame(std::move(built));

    apsync_log_entry entry;
    entry.sequence = sequence;
    entry.frame = frame;
    s.log.push_back(std::move(entry));

    for (auto &peer : s.peers)
        if (is_live(*peer))
            enqueue(s, *peer, whole_item(frame, true));
    emit_message(s, origin, channel, sequence, 0, data, size);
}

// Sends a latest value to every peer in the session but [except].
static void relay_latest(apsync_session_t &s, APSYNC_PEER origin, uint32_t channel, uint64_t key,
                         const uint8_t *data, size_t size, const apsync_peer *except)
{
    apsync_shared_bytes frame = frame_latest(channel, key, origin, data, size);
    for (auto &peer : s.peers)
    {
        if (!is_live(*peer) || peer.get() == except)
            continue;
        apsync_out_item item = whole_item(frame, true);
        item.latest = true;
        item.key.channel = channel;
        item.key.origin = origin;
        item.key.key = key;
        enqueue(s, *peer, std::move(item));
    }
}

static bool channel_is(const apsync_session_t &s, uint32_t channel, APSYNC_CHANNEL_MODE mode)
{
    return channel < s.channels.size() && s.channels[channel] == mode;
}

// Tells a newcomer why it isn't being let in, and drops it. Whether the
// words arrive before the connection closes is the network's to say.
static void refuse_peer(apsync_session_t &s, apsync_peer &p, APSYNC_REASON reason)
{
    enqueue(s, p, whole_item(frame_refuse(reason), false));
    flush_peer(s, p);
    drop_peer(s, p, reason, "");
}

// ---- Frames in ----------------------------------------------------------------

static void host_frame(apsync_session_t &s, apsync_peer &p, uint8_t type, const uint8_t *data, size_t size)
{
    apsync_reader r{data, size};
    switch (type)
    {
    case APSYNC_FRAME_PING:
        return;

    case APSYNC_FRAME_HELLO: {
        uint32_t magic = 0;
        uint16_t wire_version = 0;
        if (p.state != APSYNC_PEER_AWAITING_HELLO || !r.u32(magic) || !r.u16(wire_version) ||
            magic != APSYNC_WIRE_MAGIC)
            break;
        uint64_t application_id = 0, resume_sequence = 0;
        uint32_t application_version = 0;
        // Another wire version may lay the rest out differently, so it is
        // turned away before the rest is read.
        if (wire_version != APSYNC_WIRE_VERSION || !r.u64(application_id) || !r.u32(application_version) ||
            !r.u64(resume_sequence) || application_id != s.application_id ||
            application_version != s.application_version)
        {
            refuse_peer(s, p, APSYNC_REASON_REFUSED_APPLICATION);
            return;
        }
        if (r.left() > APSYNC_MAX_PEER_INFO_SIZE)
            break;
        p.info.assign(r.rest(), r.rest() + r.left());
        p.id = s.next_peer++;
        p.state = APSYNC_PEER_AWAITING_SNAPSHOT;

        apsync_bytes welcome = begin_frame(APSYNC_FRAME_WELCOME, 4);
        put_u32(welcome, p.id);
        enqueue(s, p, whole_item(finish_frame(std::move(welcome)), false));

        apsync_queued_event event;
        event.type = APSYNC_EVENT_SNAPSHOT_REQUEST;
        event.peer = p.id;
        event.data = p.info;
        emit(s, std::move(event));
        return;
    }

    case APSYNC_FRAME_SUBMIT: {
        uint16_t channel = 0;
        if (p.state != APSYNC_PEER_LIVE || !r.u16(channel) || !channel_is(s, channel, APSYNC_CHANNEL_ORDERED) ||
            r.left() > s.max_message_size)
            break;
        sequence_and_send(s, p.id, channel, r.rest(), r.left());
        return;
    }

    case APSYNC_FRAME_LATEST: {
        uint16_t channel = 0;
        uint64_t key = 0;
        uint32_t origin = 0; // a joiner's is ignored: the host knows who sent it
        if (p.state != APSYNC_PEER_LIVE || !r.u16(channel) || !r.u64(key) || !r.u32(origin) ||
            !channel_is(s, channel, APSYNC_CHANNEL_LATEST) || r.left() > s.max_message_size)
            break;
        relay_latest(s, p.id, channel, key, r.rest(), r.left(), &p);
        emit_message(s, p.id, channel, 0, key, r.rest(), r.left());
        return;
    }

    default:
        break;
    }
    drop_peer(s, p, APSYNC_REASON_PROTOCOL, "a frame that is malformed or out of place");
}

static void joined_frame(apsync_session_t &s, apsync_peer &p, uint8_t type, const uint8_t *data, size_t size)
{
    apsync_reader r{data, size};
    switch (type)
    {
    case APSYNC_FRAME_PING:
        return;

    case APSYNC_FRAME_WELCOME: {
        uint32_t id = 0;
        if (p.state != APSYNC_PEER_AWAITING_WELCOME || !r.u32(id) || id <= APSYNC_PEER_HOST)
            break;
        p.state = APSYNC_PEER_RECEIVING_SNAPSHOT;
        emit_simple(s, APSYNC_EVENT_CONNECTED, id, APSYNC_REASON_NONE, "");
        return;
    }

    case APSYNC_FRAME_REFUSE: {
        uint8_t reason = 0;
        if (p.state != APSYNC_PEER_AWAITING_WELCOME || !r.u8(reason))
            break;
        drop_peer(s, p,
                  reason == (uint8_t)APSYNC_REASON_REFUSED_FULL ? APSYNC_REASON_REFUSED_FULL
                                                                : APSYNC_REASON_REFUSED_APPLICATION,
                  "");
        return;
    }

    case APSYNC_FRAME_SNAPSHOT_BEGIN: {
        if (p.state != APSYNC_PEER_RECEIVING_SNAPSHOT || p.snapshot_begun || !r.u64(p.snapshot_sequence) ||
            !r.u64(p.snapshot_total))
            break;
        if (p.snapshot_total > s.max_snapshot_size)
        {
            char text[96];
            snprintf(text, sizeof(text), "the snapshot is %llu bytes", (unsigned long long)p.snapshot_total);
            drop_peer(s, p, APSYNC_REASON_SNAPSHOT_TOO_BIG, text);
            return;
        }
        p.snapshot_begun = true;
        p.snapshot.reserve((size_t)p.snapshot_total);
        return;
    }

    case APSYNC_FRAME_SNAPSHOT_DATA:
        if (p.state != APSYNC_PEER_RECEIVING_SNAPSHOT || !p.snapshot_begun ||
            size > p.snapshot_total - p.snapshot.size())
            break;
        p.snapshot.insert(p.snapshot.end(), data, data + size);
        return;

    case APSYNC_FRAME_SNAPSHOT_END: {
        if (p.state != APSYNC_PEER_RECEIVING_SNAPSHOT || !p.snapshot_begun || p.snapshot.size() != p.snapshot_total)
            break;
        p.state = APSYNC_PEER_LIVE;
        apsync_queued_event event;
        event.type = APSYNC_EVENT_SNAPSHOT;
        event.sequence = p.snapshot_sequence;
        event.data = std::move(p.snapshot);
        p.snapshot = apsync_bytes();
        emit(s, std::move(event));
        return;
    }

    case APSYNC_FRAME_PEER_JOINED: {
        uint32_t id = 0;
        if (p.state != APSYNC_PEER_LIVE || !r.u32(id) || r.left() > APSYNC_MAX_PEER_INFO_SIZE)
            break;
        apsync_queued_event event;
        event.type = APSYNC_EVENT_PEER_JOINED;
        event.peer = id;
        event.data.assign(r.rest(), r.rest() + r.left());
        emit(s, std::move(event));
        return;
    }

    case APSYNC_FRAME_PEER_LEFT: {
        uint32_t id = 0;
        uint8_t reason = 0;
        if (p.state != APSYNC_PEER_LIVE || !r.u32(id) || !r.u8(reason))
            break;
        emit_simple(s, APSYNC_EVENT_PEER_LEFT, id, reason_from_wire(reason), "");
        return;
    }

    case APSYNC_FRAME_ORDERED: {
        uint16_t channel = 0;
        uint64_t sequence = 0;
        uint32_t origin = 0;
        if (p.state != APSYNC_PEER_LIVE || !r.u16(channel) || !r.u64(sequence) || !r.u32(origin) ||
            !channel_is(s, channel, APSYNC_CHANNEL_ORDERED) || r.left() > s.max_message_size)
            break;
        emit_message(s, origin, channel, sequence, 0, r.rest(), r.left());
        return;
    }

    case APSYNC_FRAME_LATEST: {
        uint16_t channel = 0;
        uint64_t key = 0;
        uint32_t origin = 0;
        if (p.state != APSYNC_PEER_LIVE || !r.u16(channel) || !r.u64(key) || !r.u32(origin) ||
            !channel_is(s, channel, APSYNC_CHANNEL_LATEST) || r.left() > s.max_message_size)
            break;
        emit_message(s, origin, channel, 0, key, r.rest(), r.left());
        return;
    }

    default:
        break;
    }
    drop_peer(s, p, APSYNC_REASON_PROTOCOL, "a frame that is malformed or out of place");
}

// Handles every whole frame [p] has received and keeps what is left of the
// next one.
static void parse_frames(apsync_session_t &s, apsync_peer &p)
{
    size_t position = 0;
    while (!p.dead && p.in.size() - position >= 4)
    {
        uint32_t length = get_u32_at(p.in.data() + position);
        if (length == 0 || length > s.frame_limit)
        {
            char text[96];
            snprintf(text, sizeof(text), "a frame of %u bytes, where %u is the most", length, s.frame_limit);
            drop_peer(s, p, APSYNC_REASON_PROTOCOL, text);
            break;
        }
        if (p.in.size() - position - 4 < length)
            break;
        const uint8_t *frame = p.in.data() + position + 4;
        if (s.hosting)
            host_frame(s, p, frame[0], frame + 1, length - 1);
        else
            joined_frame(s, p, frame[0], frame + 1, length - 1);
        position += 4 + (size_t)length;
    }
    if (position > 0 && !p.dead)
        p.in.erase(p.in.begin(), p.in.begin() + (ptrdiff_t)position);
}

// Takes everything [p]'s connection has, until it says there is no more.
static void receive_peer(apsync_session_t &s, apsync_peer &p)
{
    while (!p.dead)
    {
        uint64_t received = 0;
        SPUDRESULT result = spudnet_tcp_socket_recv(p.socket, s.receive_buffer.data(), s.receive_buffer.size(),
                                                    SPUDNET_NO_WAIT, &received);
        if (result == SPUDRESULT_SPUDNET_TIMED_OUT)
            return;
        if (result != SPUD_SUCCESS)
        {
            drop_peer(s, p, APSYNC_REASON_NETWORK,
                      describe_socket(p.socket, SPUDNET_ERROR_SIDE_RECEIVING, "receiving", result));
            return;
        }
        if (received == 0)
        {
            drop_peer(s, p, APSYNC_REASON_CLOSED, "");
            return;
        }
        p.last_received = apsync_clock::now();
        p.in.insert(p.in.end(), s.receive_buffer.begin(), s.receive_buffer.begin() + (ptrdiff_t)received);
        parse_frames(s, p);
    }
}

// ---- Hosting: newcomers -------------------------------------------------------

// How many accepts in a row may fail before a listener is given up. One
// failure can be a connection that went away while it waited; a listener
// that only fails would otherwise be named by every wait.
static const uint32_t APSYNC_FAILED_ACCEPTS_BEFORE_GIVING_UP = 8;

static void accept_peers(apsync_session_t &s, apsync_listener &listener)
{
    while (listener.handle)
    {
        // Room for the newcomer is found before it is accepted, so that
        // running out of memory never leaves a connection nobody owns.
        s.peers.reserve(s.peers.size() + 1);
        std::unique_ptr<apsync_peer> peer(new apsync_peer());

        spudnet_tcp_socket socket = nullptr;
        SPUDRESULT result = spudnet_tcp_listener_accept(listener.handle, SPUDNET_NO_WAIT, &socket);
        if (result == SPUDRESULT_SPUDNET_TIMED_OUT)
            return;
        if (result != SPUD_SUCCESS)
        {
            if (++listener.failed_accepts < APSYNC_FAILED_ACCEPTS_BEFORE_GIVING_UP)
                return;
            spudnet_error detail;
            spudnet_tcp_listener_get_error(listener.handle, &detail);
            std::string message = describe("accepting a connection", result, detail);
            spudnet_wait_set_remove_tcp_listener(s.wait_set, listener.handle);
            spudnet_tcp_listener_destroy(listener.handle);
            listener.handle = nullptr;
            emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE, APSYNC_REASON_NETWORK, std::move(message));
            return;
        }
        listener.failed_accepts = 0;

        uint32_t present = 0;
        for (auto &other : s.peers)
            if (!other->dead)
                present++;
        if (present >= s.max_peers)
        {
            // Not worth a place in the wait set: told once, as far as the
            // connection takes it right now, and closed.
            apsync_shared_bytes refusal = frame_refuse(APSYNC_REASON_REFUSED_FULL);
            uint64_t sent = 0;
            spudnet_tcp_socket_send(socket, refusal->data(), refusal->size(), SPUDNET_NO_WAIT, &sent);
            spudnet_tcp_socket_finish_sending(socket);
            spudnet_tcp_socket_destroy(socket);
            continue;
        }

        // Every frame goes out in one send and is wanted at the other end
        // at once, so there is nothing for the system to gain by holding
        // small writes back.
        spudnet_tcp_socket_set_no_delay(socket, true);

        peer->socket = socket;
        peer->state = APSYNC_PEER_AWAITING_HELLO;
        peer->last_received = peer->last_sent = apsync_clock::now();
        peer->handshake_deadline = peer->last_received + std::chrono::milliseconds(s.handshake_timeout_ms);
        if (spudnet_wait_set_add_tcp_socket(s.wait_set, socket, SPUDNET_WAIT_FOR_INCOMING,
                                            static_cast<apsync_wait_member *>(peer.get())) != SPUD_SUCCESS)
        {
            spudnet_tcp_socket_destroy(socket);
            continue;
        }
        peer->in_wait_set = true;
        s.peers.push_back(std::move(peer));
    }
}

// ---- Commands in --------------------------------------------------------------

static apsync_peer *find_peer(apsync_session_t &s, APSYNC_PEER id)
{
    for (auto &peer : s.peers)
        if (!peer->dead && peer->id == id)
            return peer.get();
    return nullptr;
}

// Hosting: sends [p] the snapshot, everyone present, and the ordered
// messages the snapshot was made too early to include - and from here it is
// in the session.
static void send_snapshot(apsync_session_t &s, apsync_peer &p, apsync_command &command)
{
    uint64_t total = command.data.size();
    apsync_bytes begin = begin_frame(APSYNC_FRAME_SNAPSHOT_BEGIN, 16);
    put_u64(begin, command.sequence);
    put_u64(begin, total);
    enqueue(s, p, whole_item(finish_frame(std::move(begin)), false));

    // The pieces are sent straight out of the one copy: each is a five-byte
    // frame header of its own and then a stretch of the snapshot.
    apsync_shared_bytes snapshot = std::make_shared<const apsync_bytes>(std::move(command.data));
    for (uint64_t offset = 0; offset < total; offset += APSYNC_SNAPSHOT_CHUNK_SIZE)
    {
        uint64_t piece = std::min<uint64_t>(APSYNC_SNAPSHOT_CHUNK_SIZE, total - offset);
        apsync_bytes header(5);
        write_frame_length(header, (uint32_t)piece + 1);
        header[4] = (uint8_t)APSYNC_FRAME_SNAPSHOT_DATA;
        enqueue(s, p, whole_item(std::make_shared<const apsync_bytes>(std::move(header)), false));

        apsync_out_item data;
        data.bytes = snapshot;
        data.begin = (size_t)offset;
        data.end = (size_t)(offset + piece);
        enqueue(s, p, std::move(data));
    }
    enqueue(s, p, whole_item(finish_frame(begin_frame(APSYNC_FRAME_SNAPSHOT_END, 0)), false));

    enqueue(s, p, whole_item(frame_peer_joined(APSYNC_PEER_HOST, s.peer_info), true));
    for (auto &other : s.peers)
        if (is_live(*other))
            enqueue(s, p, whole_item(frame_peer_joined(other->id, other->info), true));

    // Everything numbered after the snapshot's point is still in the log:
    // the log is only trimmed up to what the application had polled before
    // this command was taken (run_loop).
    for (const apsync_log_entry &entry : s.log)
        if (entry.sequence > command.sequence)
            enqueue(s, p, whole_item(entry.frame, true));

    if (p.dead)
        return; // fell over the backlog limit before it began

    apsync_shared_bytes joined = frame_peer_joined(p.id, p.info);
    for (auto &other : s.peers)
        if (is_live(*other))
            enqueue(s, *other, whole_item(joined, true));
    p.state = APSYNC_PEER_LIVE;

    apsync_queued_event event;
    event.type = APSYNC_EVENT_PEER_JOINED;
    event.peer = p.id;
    event.data = p.info;
    emit(s, std::move(event));
}

static void run_send(apsync_session_t &s, apsync_command &command)
{
    bool ordered = s.channels[command.channel] == APSYNC_CHANNEL_ORDERED;
    if (s.hosting)
    {
        if (ordered)
            sequence_and_send(s, APSYNC_PEER_HOST, command.channel, command.data.data(), command.data.size());
        else
            relay_latest(s, APSYNC_PEER_HOST, command.channel, command.key, command.data.data(), command.data.size(),
                         nullptr);
        return;
    }

    if (s.peers.empty() || !is_live(*s.peers[0]))
        return; // the connection ended after the caller's send was taken
    apsync_peer &host = *s.peers[0];
    if (ordered)
    {
        apsync_bytes frame = begin_frame(APSYNC_FRAME_SUBMIT, 2 + command.data.size());
        put_u16(frame, (uint16_t)command.channel);
        put_bytes(frame, command.data.data(), command.data.size());
        enqueue(s, host, whole_item(finish_frame(std::move(frame)), true));
    }
    else
    {
        apsync_out_item item = whole_item(
            frame_latest(command.channel, command.key, APSYNC_PEER_NONE, command.data.data(), command.data.size()),
            true);
        item.latest = true;
        item.key.channel = command.channel;
        item.key.key = command.key;
        enqueue(s, host, std::move(item));
    }
}

static void run_commands(apsync_session_t &s)
{
    std::vector<apsync_command> commands;
    {
        std::lock_guard<std::mutex> lock(s.outbox_mutex);
        commands.swap(s.outbox);
        s.outbox_bytes = 0;
    }
    for (apsync_command &command : commands)
    {
        switch (command.kind)
        {
        case apsync_command::SEND:
            run_send(s, command);
            break;
        case apsync_command::SNAPSHOT:
            // A peer that left while the snapshot was being made has no
            // use for it; APSYNC_EVENT_PEER_LEFT has said so.
            if (apsync_peer *peer = find_peer(s, command.peer))
                if (peer->state == APSYNC_PEER_AWAITING_SNAPSHOT)
                    send_snapshot(s, *peer, command);
            break;
        case apsync_command::DISCONNECT:
            if (apsync_peer *peer = find_peer(s, command.peer))
                drop_peer(s, *peer, APSYNC_REASON_DISCONNECTED_BY_HOST, "");
            break;
        }
    }
}

// ---- The clock ----------------------------------------------------------------

// ApSync's own, and the same one on every platform: the standard library's
// monotonic clock. Whether that counts time the machine spent asleep
// differs by system; a connection that sat through a sleep is as likely
// dropped for silence as not, and was probably gone anyway.

static void run_timers(apsync_session_t &s)
{
    apsync_clock::time_point now = apsync_clock::now();
    for (auto &owned : s.peers)
    {
        apsync_peer &p = *owned;
        if (p.dead)
            continue;
        bool in_handshake = p.state == APSYNC_PEER_AWAITING_HELLO || p.state == APSYNC_PEER_AWAITING_WELCOME;
        if (in_handshake && now >= p.handshake_deadline)
        {
            drop_peer(s, p, APSYNC_REASON_TIMED_OUT, "the opening exchange took too long");
            continue;
        }
        if (s.peer_timeout_ms != 0 && now - p.last_received >= std::chrono::milliseconds(s.peer_timeout_ms))
        {
            drop_peer(s, p, APSYNC_REASON_TIMED_OUT, "nothing was heard from the other end");
            continue;
        }
        // With something already waiting to go out there is nothing a ping
        // would add: either it goes, which shows as much, or the other end
        // isn't reading and its own time-out is the judge of that.
        if (s.heartbeat_interval_ms != 0 && p.out.empty() &&
            now - p.last_sent >= std::chrono::milliseconds(s.heartbeat_interval_ms))
            enqueue(s, p, whole_item(s.ping_frame, false));
    }
}

// How long the loop may wait before a timer is due.
static uint32_t next_timeout(apsync_session_t &s)
{
    bool any = false;
    apsync_clock::time_point earliest;
    auto consider = [&](apsync_clock::time_point when) {
        if (!any || when < earliest)
            earliest = when;
        any = true;
    };
    for (auto &owned : s.peers)
    {
        apsync_peer &p = *owned;
        if (p.dead)
            continue;
        if (p.state == APSYNC_PEER_AWAITING_HELLO || p.state == APSYNC_PEER_AWAITING_WELCOME)
            consider(p.handshake_deadline);
        if (s.peer_timeout_ms != 0)
            consider(p.last_received + std::chrono::milliseconds(s.peer_timeout_ms));
        if (s.heartbeat_interval_ms != 0 && p.out.empty())
            consider(p.last_sent + std::chrono::milliseconds(s.heartbeat_interval_ms));
    }
    if (!any)
        return SPUDNET_WAIT_FOREVER;
    apsync_clock::time_point now = apsync_clock::now();
    if (earliest <= now)
        return SPUDNET_NO_WAIT;
    // Rounded up, so the timer is due when the wait is over.
    int64_t left = std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now).count() + 1;
    return left >= (int64_t)SPUDNET_WAIT_FOREVER ? SPUDNET_WAIT_FOREVER - 1 : (uint32_t)left;
}

// ---- The loop -----------------------------------------------------------------

// Destroys the peers that were dropped. Only here, between waits, is it
// certain that nothing still refers to them.
static void reap_peers(apsync_session_t &s)
{
    for (size_t i = 0; i < s.peers.size();)
    {
        apsync_peer &p = *s.peers[i];
        if (!p.dead)
        {
            ++i;
            continue;
        }
        if (p.in_wait_set)
            spudnet_wait_set_remove_tcp_socket(s.wait_set, p.socket);
        // Lets what was already sent - a refusal, most of all - reach the
        // other end ahead of the close, where the system allows.
        spudnet_tcp_socket_finish_sending(p.socket);
        spudnet_tcp_socket_destroy(p.socket);
        s.peers.erase(s.peers.begin() + (ptrdiff_t)i);
    }
}

static void run_loop(apsync_session_t &s)
{
    spudnet_wait_event events[64];
    while (!s.stop.load())
    {
        // Read before the commands are taken, and used after them. A
        // snapshot command names the last message its application had
        // polled; one taken now named a value no smaller than the last
        // loop's, and one not yet queued will name one no smaller than
        // this. So trimming to this value after running what was taken
        // never removes a message some snapshot still has to be followed
        // by.
        uint64_t polled = s.delivered_sequence.load();
        run_commands(s);
        while (!s.log.empty() && s.log.front().sequence <= polled)
            s.log.pop_front();

        // A queue that is already waiting for room is left to the wait set.
        for (auto &peer : s.peers)
            if (!peer->dead && !peer->out.empty() && !peer->waiting_for_room)
                flush_peer(s, *peer);
        reap_peers(s);

        if (!s.hosting)
        {
            if (s.peers.empty())
                return; // the connection to the host has ended
            s.queued_bytes.store(s.peers[0]->out_bytes);
        }

        uint32_t count = 0;
        SPUDRESULT result = spudnet_wait_set_wait(s.wait_set, next_timeout(s), events, 64, &count);
        if (result == SPUDRESULT_SPUDNET_ABORTED)
            return;
        if (result != SPUD_SUCCESS && result != SPUDRESULT_SPUDNET_TIMED_OUT)
        {
            spudnet_error detail;
            spudnet_wait_set_get_error(s.wait_set, &detail);
            emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE, APSYNC_REASON_INTERNAL,
                        describe("waiting on the session's connections", result, detail));
            return;
        }

        for (uint32_t i = 0; i < count; ++i)
        {
            apsync_wait_member *member = static_cast<apsync_wait_member *>(events[i].user);
            if (member->is_listener)
            {
                accept_peers(s, *static_cast<apsync_listener *>(member));
                continue;
            }
            apsync_peer &p = *static_cast<apsync_peer *>(member);
            if (!p.dead && (events[i].ready & SPUDNET_WAIT_FOR_INCOMING))
                receive_peer(s, p);
            if (!p.dead && (events[i].ready & SPUDNET_WAIT_FOR_ROOM))
                flush_peer(s, p);
        }
        run_timers(s);
    }
}

// ---- Joining ------------------------------------------------------------------

static const uint32_t APSYNC_MAX_RESOLVED_ADDRESSES = 8;

// Looks the host's name up. False when there is nothing to connect to, in
// which case the event saying why has been emitted - unless the session is
// being destroyed, which needs none.
static bool resolve_host(apsync_session_t &s, char addresses[][SPUDNET_MAX_ADDRESS_STRING_LEN], uint32_t &count)
{
    spudnet_error none{};
    spudnet_resolver_desc desc{};
    desc.host = s.join_host.c_str();
    desc.family = s.join_family;
    spudnet_resolver resolver = nullptr;
    SPUDRESULT result = spudnet_resolver_create(s.instance, &desc, &resolver);
    if (result != SPUD_SUCCESS)
    {
        emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE, APSYNC_REASON_INTERNAL,
                    describe("making a resolver", result, none));
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s.abort_mutex);
        s.resolver = resolver;
    }
    // A destroy that looked before the resolver was there to abort.
    if (s.stop.load())
        spudnet_resolver_abort(resolver);

    // The lookup blocks for as long as the system takes and, outside
    // Windows, nothing can call it back. So it gets a thread that does
    // nothing else and that nobody waits for: SpudNet keeps the resolver
    // alive until that thread is out of it, whenever that is - and the
    // platform's networking with it, so the session's instance may go
    // first.
    bool started = true;
    try
    {
        std::thread([resolver] { spudnet_resolver_run(resolver); }).detach();
    }
    catch (...)
    {
        started = false;
    }
    if (!started)
    {
        // Every resolver is run once. Aborted first, it returns at once.
        spudnet_resolver_abort(resolver);
        spudnet_resolver_run(resolver);
    }

    count = 0;
    std::string message;
    if (started)
    {
        result = spudnet_resolver_wait(resolver, s.resolve_timeout_ms, addresses, APSYNC_MAX_RESOLVED_ADDRESSES, &count);
        if (result != SPUD_SUCCESS && result != SPUDRESULT_SPUDNET_TIMED_OUT && result != SPUDRESULT_SPUDNET_ABORTED)
        {
            spudnet_error detail;
            spudnet_resolver_get_error(resolver, &detail);
            message = describe("looking up the host's name", result, detail);
        }
    }
    {
        std::lock_guard<std::mutex> lock(s.abort_mutex);
        s.resolver = nullptr;
    }
    spudnet_resolver_destroy(resolver);

    if (!started)
    {
        emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE, APSYNC_REASON_INTERNAL,
                    "couldn't start a thread to look the host's name up");
        return false;
    }
    if (result == SPUDRESULT_SPUDNET_ABORTED || s.stop.load())
        return false;
    if (result == SPUDRESULT_SPUDNET_TIMED_OUT)
    {
        emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE, APSYNC_REASON_TIMED_OUT,
                    "looking up the host's name took too long");
        return false;
    }
    if (result != SPUD_SUCCESS || count == 0)
    {
        emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE,
                    result == SPUD_SUCCESS || result == SPUDRESULT_SPUDNET_RESOLVE_FAILED ? APSYNC_REASON_RESOLVE_FAILED
                                                                                          : APSYNC_REASON_NETWORK,
                    std::move(message));
        return false;
    }
    return true;
}

// Looks the host up, connects, and says hello. True with the connection in
// the wait set and the session's one peer made; false with the reason
// emitted (or none, when the session is being destroyed).
static bool connect_to_host(apsync_session_t &s)
{
    char addresses[APSYNC_MAX_RESOLVED_ADDRESSES][SPUDNET_MAX_ADDRESS_STRING_LEN];
    uint32_t count = 0;
    if (!resolve_host(s, addresses, count))
        return false;

    spudnet_error none{};
    // Made before there is a connection for it to own; see accept_peers.
    s.peers.reserve(1);
    std::unique_ptr<apsync_peer> peer(new apsync_peer());

    spudnet_tcp_socket connected = nullptr;
    bool every_attempt_timed_out = true;
    std::string message;
    for (uint32_t i = 0; i < count && !connected; ++i)
    {
        if (s.stop.load())
            return false;
        // A socket is good for one attempt.
        spudnet_tcp_socket socket = nullptr;
        SPUDRESULT result = spudnet_tcp_socket_create(s.instance, &socket);
        if (result != SPUD_SUCCESS)
        {
            emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE, APSYNC_REASON_INTERNAL,
                        describe("making a socket", result, none));
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(s.abort_mutex);
            s.connecting = socket;
        }
        if (s.stop.load())
            spudnet_tcp_socket_abort(socket);

        spudnet_tcp_socket_connect_desc desc{};
        desc.address = addresses[i];
        desc.port = s.join_port;
        desc.timeout_ms = s.connect_timeout_ms;
        result = spudnet_tcp_socket_connect(socket, &desc);
        if (result == SPUDRESULT_SPUDNET_TIMED_OUT)
        {
            message = std::string("connecting to ") + addresses[i] + " took too long";
        }
        else if (result != SPUD_SUCCESS && result != SPUDRESULT_SPUDNET_ABORTED)
        {
            every_attempt_timed_out = false;
            std::string what = std::string("connecting to ") + addresses[i];
            message = describe_socket(socket, SPUDNET_ERROR_SIDE_RECEIVING, what.c_str(), result);
        }
        {
            std::lock_guard<std::mutex> lock(s.abort_mutex);
            s.connecting = nullptr;
        }
        if (result == SPUD_SUCCESS)
        {
            connected = socket;
            break;
        }
        spudnet_tcp_socket_destroy(socket);
        if (result == SPUDRESULT_SPUDNET_ABORTED)
            return false;
    }
    if (!connected)
    {
        emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE,
                    every_attempt_timed_out ? APSYNC_REASON_TIMED_OUT : APSYNC_REASON_CONNECT_FAILED,
                    std::move(message));
        return false;
    }

    spudnet_tcp_socket_set_no_delay(connected, true); // see accept_peers

    peer->id = APSYNC_PEER_HOST;
    peer->socket = connected;
    peer->state = APSYNC_PEER_AWAITING_WELCOME;
    peer->last_received = peer->last_sent = apsync_clock::now();
    peer->handshake_deadline = peer->last_received + std::chrono::milliseconds(s.handshake_timeout_ms);
    SPUDRESULT result = spudnet_wait_set_add_tcp_socket(s.wait_set, connected, SPUDNET_WAIT_FOR_INCOMING,
                                                        static_cast<apsync_wait_member *>(peer.get()));
    if (result != SPUD_SUCCESS)
    {
        spudnet_tcp_socket_destroy(connected);
        emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE, APSYNC_REASON_INTERNAL,
                    describe("putting the connection in the wait set", result, none));
        return false;
    }
    peer->in_wait_set = true;
    // The session's from here, before anything else can fail.
    apsync_peer &host = *peer;
    s.peers.push_back(std::move(peer));

    apsync_bytes hello = begin_frame(APSYNC_FRAME_HELLO, 26 + s.peer_info.size());
    put_u32(hello, APSYNC_WIRE_MAGIC);
    put_u16(hello, APSYNC_WIRE_VERSION);
    put_u64(hello, s.application_id);
    put_u32(hello, s.application_version);
    put_u64(hello, 0); // resume sequence: see apsync_internal.hpp
    put_bytes(hello, s.peer_info.data(), s.peer_info.size());
    enqueue(s, host, whole_item(finish_frame(std::move(hello)), false));
    return true;
}

// ---- The thread ---------------------------------------------------------------

// Closes every connection and listener. On ApSync's thread as it ends, and
// again from the destroy that follows, where it finds nothing left - or, if
// the thread was never started, everything.
static void release_network(apsync_session_t &s)
{
    for (auto &peer : s.peers)
    {
        if (peer->in_wait_set)
            spudnet_wait_set_remove_tcp_socket(s.wait_set, peer->socket);
        spudnet_tcp_socket_destroy(peer->socket);
    }
    s.peers.clear();
    for (auto &listener : s.listeners)
    {
        if (!listener->handle)
            continue;
        spudnet_wait_set_remove_tcp_listener(s.wait_set, listener->handle);
        spudnet_tcp_listener_destroy(listener->handle);
        listener->handle = nullptr;
    }
}

static void thread_main(apsync_session_t *session)
{
    apsync_session_t &s = *session;
    try
    {
        if (s.hosting || connect_to_host(s))
            run_loop(s);
    }
    catch (...)
    {
        // Out of memory, in practice. The session can't go on; say so if
        // there is memory enough for that.
        try
        {
            emit_simple(s, APSYNC_EVENT_DISCONNECTED, APSYNC_PEER_NONE, APSYNC_REASON_INTERNAL, "out of memory");
        }
        catch (...)
        {
        }
    }
    release_network(s);
}

// ---- Creation -----------------------------------------------------------------

static void destroy_session(apsync_session_t *s)
{
    s->stop.store(true);
    {
        std::lock_guard<std::mutex> lock(s->abort_mutex);
        if (s->resolver)
            spudnet_resolver_abort(s->resolver);
        if (s->connecting)
            spudnet_tcp_socket_abort(s->connecting);
    }
    // Aborting is how every SpudNet object is taken apart: abort, wait for
    // the thread that was in it, destroy.
    spudnet_wait_set_abort(s->wait_set);
    if (s->thread.joinable())
        s->thread.join();
    release_network(*s);
    spudnet_wait_set_destroy(s->wait_set);
    spudnet_instance_destroy(s->instance);
    delete s;
}

// Checks [desc] and copies it into a new session, with its wait set made.
static APRESULT create_session(const APSYNC_SESSION_DESC *desc, bool hosting, apsync_session_t *&out_session,
                               APSYNC_ERROR *error)
{
    out_session = nullptr;
    APSYNC_SESSION_DESC d;
    if (!read_versioned(desc, APSYNC_SESSION_DESC_V1_SIZE, d))
        return fail(error, APRESULT_INVALID_ARGUMENT, "desc is null, or its struct_size is too small");
    if (!d.channels || d.channel_count == 0 || d.channel_count > APSYNC_MAX_CHANNELS)
        return fail(error, APRESULT_INVALID_ARGUMENT, "a session has between 1 and %u channels", APSYNC_MAX_CHANNELS);
    for (uint32_t i = 0; i < d.channel_count; ++i)
        if (d.channels[i] != APSYNC_CHANNEL_ORDERED && d.channels[i] != APSYNC_CHANNEL_LATEST)
            return fail(error, APRESULT_INVALID_ARGUMENT, "channel %u has no such mode", i);
    if (d.peer_info_size > APSYNC_MAX_PEER_INFO_SIZE || (d.peer_info_size != 0 && !d.peer_info))
        return fail(error, APRESULT_INVALID_ARGUMENT, "peer_info is null, or over %u bytes", APSYNC_MAX_PEER_INFO_SIZE);
    if (d.max_message_size == 0 || d.max_message_size > 0xFFFFFFFFu - APSYNC_FRAME_OVERHEAD)
        return fail(error, APRESULT_INVALID_ARGUMENT, "max_message_size is 0, or too large to frame");
    if (d.max_backlog_size == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "max_backlog_size is 0");
    if (d.handshake_timeout_ms == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "handshake_timeout_ms is 0");

    apsync_session_t *s = new (std::nothrow) apsync_session_t();
    if (!s)
        return fail(error, APRESULT_OUT_OF_MEMORY, "out of memory");
    try
    {
        s->hosting = hosting;
        s->application_id = d.application_id;
        s->application_version = d.application_version;
        s->channels.assign(d.channels, d.channels + d.channel_count);
        if (d.peer_info_size != 0)
            s->peer_info.assign((const uint8_t *)d.peer_info, (const uint8_t *)d.peer_info + d.peer_info_size);
        s->max_message_size = d.max_message_size;
        s->max_backlog_size = d.max_backlog_size;
        s->heartbeat_interval_ms = d.heartbeat_interval_ms;
        s->peer_timeout_ms = d.peer_timeout_ms;
        s->handshake_timeout_ms = d.handshake_timeout_ms;
        // The longest frame is a message, a piece of a snapshot or a
        // peer's description, plus what ApSync puts round it.
        s->frame_limit =
            std::max({d.max_message_size, APSYNC_SNAPSHOT_CHUNK_SIZE, APSYNC_MAX_PEER_INFO_SIZE}) + APSYNC_FRAME_OVERHEAD;
        s->receive_buffer.resize(64 * 1024);
        s->ping_frame = finish_frame(begin_frame(APSYNC_FRAME_PING, 0));
    }
    catch (const std::bad_alloc &)
    {
        delete s;
        return fail(error, APRESULT_OUT_OF_MEMORY, "out of memory");
    }

    // The platform's networking, started for this session alone. Other
    // users of SpudNet in the process have instances of their own.
    spudnet_error detail;
    SPUDRESULT result = spudnet_instance_create(&s->instance, &detail);
    if (result != SPUD_SUCCESS)
    {
        std::string message = describe("starting the platform's networking", result, detail);
        delete s;
        APRESULT code = result == SPUDRESULT_OUT_OF_MEMORY       ? APRESULT_OUT_OF_MEMORY
                        : result == SPUDRESULT_SPUDNET_UNSUPPORTED ? APRESULT_UNSUPPORTED
                                                                   : APRESULT_IO;
        return fail(error, code, "%s", message.c_str());
    }
    result = spudnet_wait_set_create(s->instance, &s->wait_set, &detail);
    if (result != SPUD_SUCCESS)
    {
        std::string message = describe("making a wait set", result, detail);
        spudnet_instance_destroy(s->instance);
        delete s;
        return fail(error, result == SPUDRESULT_OUT_OF_MEMORY ? APRESULT_OUT_OF_MEMORY : APRESULT_IO, "%s",
                    message.c_str());
    }
    out_session = s;
    return APRESULT_OK;
}

// Starts ApSync's thread, which is the last step of making a session:
// whatever fails before it is undone on the caller's thread with nothing
// running.
static APRESULT start_session(apsync_session_t *s, apsync_session *out_session, APSYNC_ERROR *error)
{
    try
    {
        s->thread = std::thread(thread_main, s);
    }
    catch (...)
    {
        destroy_session(s);
        return fail(error, APRESULT_IO, "couldn't start the session's thread");
    }
    *out_session = s;
    return APRESULT_OK;
}

// Queues [command] for ApSync's thread and ends its wait.
static APRESULT post_command(apsync_session_t *s, apsync_command &&command)
{
    try
    {
        std::lock_guard<std::mutex> lock(s->outbox_mutex);
        s->outbox_bytes += command.data.size();
        s->outbox.push_back(std::move(command));
    }
    catch (const std::bad_alloc &)
    {
        return APRESULT_OUT_OF_MEMORY;
    }
    spudnet_wait_set_wake(s->wait_set);
    return APRESULT_OK;
}

#endif // SPUDNET_EXT_TCP

// ---- C API --------------------------------------------------------------------

extern "C"
{
#if SPUDNET_EXT_TCP

    APRESULT apsync_session_host(const APSYNC_SESSION_DESC *desc, const APSYNC_HOST_DESC *host_desc,
                                 apsync_session *out_session, APSYNC_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_session)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_session is null");
        *out_session = nullptr;
        APSYNC_HOST_DESC h;
        if (!read_versioned(host_desc, APSYNC_HOST_DESC_V1_SIZE, h))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "host_desc is null, or its struct_size is too small");
        if (!h.listen_addresses || h.listen_address_count == 0 || h.listen_address_count > APSYNC_MAX_LISTEN_ADDRESSES)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "a host listens on between 1 and %u addresses",
                        APSYNC_MAX_LISTEN_ADDRESSES);
        for (uint32_t i = 0; i < h.listen_address_count; ++i)
            if (!h.listen_addresses[i])
                return fail(out_error, APRESULT_INVALID_ARGUMENT, "listen address %u is null", i);
        if (h.listen_backlog == 0 || h.max_peers == 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "listen_backlog or max_peers is 0");

        apsync_session_t *s = nullptr;
        APRESULT created = create_session(desc, true, s, out_error);
        if (created != APRESULT_OK)
            return created;
        s->max_peers = h.max_peers;
        s->local_peer = APSYNC_PEER_HOST;
        s->can_send = true;

        try
        {
            for (uint32_t i = 0; i < h.listen_address_count; ++i)
            {
                spudnet_tcp_listener_desc listener_desc{};
                listener_desc.address = h.listen_addresses[i];
                listener_desc.port = h.port;
                listener_desc.backlog = h.listen_backlog;
                // The entry first, so that a listener is owned by the session
                // from the moment it exists.
                std::unique_ptr<apsync_listener> listener(new apsync_listener());
                listener->is_listener = true;
                apsync_listener *added = listener.get();
                s->listeners.push_back(std::move(listener));
                s->ports.reserve(s->listeners.size());

                spudnet_tcp_listener handle = nullptr;
                spudnet_error detail;
                SPUDRESULT result = spudnet_tcp_listener_create(s->instance, &listener_desc, &handle, &detail);
                if (result != SPUD_SUCCESS)
                {
                    std::string what = std::string("listening on ") + h.listen_addresses[i];
                    std::string message = describe(what.c_str(), result, detail);
                    destroy_session(s);
                    APRESULT code = result == SPUDRESULT_SPUDNET_ADDRESS_IN_USE   ? APRESULT_APSYNC_ADDRESS_IN_USE
                                    : result == SPUDRESULT_SPUDNET_INVALID_ADDRESS ? APRESULT_INVALID_ARGUMENT
                                                                                   : APRESULT_IO;
                    return fail(out_error, code, "%s", message.c_str());
                }
                added->handle = handle;

                uint16_t port = 0;
                spudnet_tcp_listener_get_port(handle, &port);
                s->ports.push_back(port);

                result = spudnet_wait_set_add_tcp_listener(s->wait_set, handle, static_cast<apsync_wait_member *>(added));
                if (result != SPUD_SUCCESS)
                {
                    spudnet_error none{};
                    std::string message = describe("putting a listener in the wait set", result, none);
                    destroy_session(s);
                    return fail(out_error, APRESULT_IO, "%s", message.c_str());
                }
            }
        }
        catch (const std::bad_alloc &)
        {
            destroy_session(s);
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory");
        }
        return start_session(s, out_session, out_error);
    }

    APRESULT apsync_session_join(const APSYNC_SESSION_DESC *desc, const APSYNC_JOIN_DESC *join_desc,
                                 apsync_session *out_session, APSYNC_ERROR *out_error)
    {
        clear_error(out_error);
        if (!out_session)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "out_session is null");
        *out_session = nullptr;
        APSYNC_JOIN_DESC j;
        if (!read_versioned(join_desc, APSYNC_JOIN_DESC_V1_SIZE, j))
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "join_desc is null, or its struct_size is too small");
        if (!j.host || j.host[0] == '\0')
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "join_desc has no host");
        if (j.family != APSYNC_ADDRESS_FAMILY_ANY && j.family != APSYNC_ADDRESS_FAMILY_IPV4 &&
            j.family != APSYNC_ADDRESS_FAMILY_IPV6)
            return fail(out_error, APRESULT_INVALID_ARGUMENT, "join_desc has no such address family");
        if (j.resolve_timeout_ms == 0 || j.connect_timeout_ms == 0 || j.max_snapshot_size == 0)
            return fail(out_error, APRESULT_INVALID_ARGUMENT,
                        "resolve_timeout_ms, connect_timeout_ms or max_snapshot_size is 0");

        apsync_session_t *s = nullptr;
        APRESULT created = create_session(desc, false, s, out_error);
        if (created != APRESULT_OK)
            return created;
        try
        {
            s->join_host = j.host;
        }
        catch (const std::bad_alloc &)
        {
            destroy_session(s);
            return fail(out_error, APRESULT_OUT_OF_MEMORY, "out of memory");
        }
        s->join_port = j.port;
        s->join_family = j.family == APSYNC_ADDRESS_FAMILY_IPV4   ? SPUDNET_ADDRESS_FAMILY_IPV4
                         : j.family == APSYNC_ADDRESS_FAMILY_IPV6 ? SPUDNET_ADDRESS_FAMILY_IPV6
                                                                  : SPUDNET_ADDRESS_FAMILY_ANY;
        s->resolve_timeout_ms = j.resolve_timeout_ms;
        s->connect_timeout_ms = j.connect_timeout_ms;
        s->max_snapshot_size = j.max_snapshot_size;
        return start_session(s, out_session, out_error);
    }

    void apsync_session_destroy(apsync_session session)
    {
        if (session)
            destroy_session(session);
    }

    APRESULT apsync_session_get_port(apsync_session session, uint32_t index, uint16_t *out_port)
    {
        if (!session || !out_port || !session->hosting || index >= session->ports.size())
            return APRESULT_INVALID_ARGUMENT;
        *out_port = session->ports[index];
        return APRESULT_OK;
    }

    APSYNC_PEER apsync_session_get_local_peer(apsync_session session)
    {
        return session ? session->local_peer : APSYNC_PEER_NONE;
    }

    APRESULT apsync_session_poll(apsync_session session, APSYNC_EVENT *out_event)
    {
        if (!session || !out_event || out_event->struct_size < APSYNC_EVENT_V1_SIZE)
            return APRESULT_INVALID_ARGUMENT;
        {
            std::lock_guard<std::mutex> lock(session->inbox_mutex);
            if (session->inbox.empty())
                return APRESULT_NOT_FOUND;
            // The one handed out last time is let go here, and not before.
            session->current = std::move(session->inbox.front());
            session->inbox.pop_front();
        }
        const apsync_queued_event &current = session->current;

        // What the caller's side of the session has to know about, learnt
        // at the moment the caller does.
        switch (current.type)
        {
        case APSYNC_EVENT_CONNECTED:
            session->local_peer = current.peer;
            break;
        case APSYNC_EVENT_SNAPSHOT:
            session->can_send = true;
            break;
        case APSYNC_EVENT_SNAPSHOT_REQUEST:
            try
            {
                session->awaiting_snapshot.insert(current.peer);
            }
            catch (const std::bad_alloc &)
            {
                // The request can't be answered; the caller still sees it,
                // and its apsync_session_send_snapshot says NOT_FOUND.
            }
            break;
        case APSYNC_EVENT_PEER_LEFT:
            session->awaiting_snapshot.erase(current.peer);
            break;
        case APSYNC_EVENT_MESSAGE:
            if (session->hosting && current.sequence != 0)
            {
                session->polled_sequence = current.sequence;
                session->delivered_sequence.store(current.sequence);
            }
            break;
        case APSYNC_EVENT_DISCONNECTED:
            if (!session->hosting || current.reason == APSYNC_REASON_INTERNAL)
                session->can_send = false;
            break;
        default:
            break;
        }

        APSYNC_EVENT event{};
        event.struct_size = (uint32_t)sizeof(event);
        event.type = current.type;
        event.peer = current.peer;
        event.channel = current.channel;
        event.sequence = current.sequence;
        event.key = current.key;
        event.reason = current.reason;
        event.data = current.data.empty() ? nullptr : current.data.data();
        event.size = current.data.size();
        event.message = current.message.c_str();
        write_versioned(out_event, APSYNC_EVENT_V1_SIZE, event);
        return APRESULT_OK;
    }

    APRESULT apsync_session_wait(apsync_session session, uint32_t timeout_ms)
    {
        if (!session)
            return APRESULT_INVALID_ARGUMENT;
        std::unique_lock<std::mutex> lock(session->inbox_mutex);
        auto ready = [session] { return !session->inbox.empty() || session->woken; };
        if (timeout_ms == APSYNC_WAIT_FOREVER)
            session->inbox_changed.wait(lock, ready);
        else
            session->inbox_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        session->woken = false;
        return session->inbox.empty() ? APRESULT_NOT_FOUND : APRESULT_OK;
    }

    void apsync_session_wake(apsync_session session)
    {
        if (!session)
            return;
        {
            std::lock_guard<std::mutex> lock(session->inbox_mutex);
            session->woken = true;
        }
        session->inbox_changed.notify_all();
    }

    APRESULT apsync_session_send(apsync_session session, uint32_t channel, uint64_t key, const void *data,
                                 uint32_t size)
    {
        if (!session || channel >= session->channels.size() || (size != 0 && !data))
            return APRESULT_INVALID_ARGUMENT;
        if (session->channels[channel] == APSYNC_CHANNEL_ORDERED && key != 0)
            return APRESULT_INVALID_ARGUMENT;
        if (!session->can_send)
            return APRESULT_APSYNC_NOT_CONNECTED;
        if (size > session->max_message_size)
            return APRESULT_LIMIT;
        if (!session->hosting)
        {
            // A soft limit: the two counts are read a moment apart, and a
            // latest value that will replace one already waiting is counted
            // as if it were added.
            std::lock_guard<std::mutex> lock(session->outbox_mutex);
            uint64_t waiting = session->outbox_bytes + session->queued_bytes.load();
            if (waiting >= session->max_backlog_size || size > session->max_backlog_size - waiting)
                return APRESULT_LIMIT;
        }
        apsync_command command;
        command.kind = apsync_command::SEND;
        command.channel = channel;
        command.key = key;
        try
        {
            if (size != 0)
                command.data.assign((const uint8_t *)data, (const uint8_t *)data + size);
        }
        catch (const std::bad_alloc &)
        {
            return APRESULT_OUT_OF_MEMORY;
        }
        return post_command(session, std::move(command));
    }

    APRESULT apsync_session_send_snapshot(apsync_session session, APSYNC_PEER peer, const void *data, uint64_t size)
    {
        if (!session || !session->hosting || (size != 0 && !data))
            return APRESULT_INVALID_ARGUMENT;
        if (session->awaiting_snapshot.find(peer) == session->awaiting_snapshot.end())
            return APRESULT_NOT_FOUND;
        apsync_command command;
        command.kind = apsync_command::SNAPSHOT;
        command.peer = peer;
        command.sequence = session->polled_sequence;
        try
        {
            if (size != 0)
                command.data.assign((const uint8_t *)data, (const uint8_t *)data + size);
        }
        catch (const std::bad_alloc &)
        {
            return APRESULT_OUT_OF_MEMORY;
        }
        catch (const std::length_error &)
        {
            return APRESULT_OUT_OF_MEMORY;
        }
        APRESULT posted = post_command(session, std::move(command));
        if (posted == APRESULT_OK)
            session->awaiting_snapshot.erase(peer);
        return posted;
    }

    APRESULT apsync_session_disconnect_peer(apsync_session session, APSYNC_PEER peer)
    {
        if (!session || !session->hosting || peer == APSYNC_PEER_NONE || peer == APSYNC_PEER_HOST)
            return APRESULT_INVALID_ARGUMENT;
        apsync_command command;
        command.kind = apsync_command::DISCONNECT;
        command.peer = peer;
        APRESULT posted = post_command(session, std::move(command));
        if (posted == APRESULT_OK)
            session->awaiting_snapshot.erase(peer);
        return posted;
    }

#else // no transport on this platform

    // Nothing here can make a session, so the rest are only ever handed
    // NULL.

    APRESULT apsync_session_host(const APSYNC_SESSION_DESC *, const APSYNC_HOST_DESC *, apsync_session *out_session,
                                 APSYNC_ERROR *out_error)
    {
        if (out_session)
            *out_session = nullptr;
        return fail(out_error, APRESULT_UNSUPPORTED, "this platform has no transport ApSync can use");
    }

    APRESULT apsync_session_join(const APSYNC_SESSION_DESC *, const APSYNC_JOIN_DESC *, apsync_session *out_session,
                                 APSYNC_ERROR *out_error)
    {
        if (out_session)
            *out_session = nullptr;
        return fail(out_error, APRESULT_UNSUPPORTED, "this platform has no transport ApSync can use");
    }

    void apsync_session_destroy(apsync_session)
    {
    }

    APRESULT apsync_session_get_port(apsync_session, uint32_t, uint16_t *)
    {
        return APRESULT_INVALID_ARGUMENT;
    }

    APSYNC_PEER apsync_session_get_local_peer(apsync_session)
    {
        return APSYNC_PEER_NONE;
    }

    APRESULT apsync_session_poll(apsync_session, APSYNC_EVENT *)
    {
        return APRESULT_INVALID_ARGUMENT;
    }

    APRESULT apsync_session_wait(apsync_session, uint32_t)
    {
        return APRESULT_INVALID_ARGUMENT;
    }

    void apsync_session_wake(apsync_session)
    {
    }

    APRESULT apsync_session_send(apsync_session, uint32_t, uint64_t, const void *, uint32_t)
    {
        return APRESULT_INVALID_ARGUMENT;
    }

    APRESULT apsync_session_send_snapshot(apsync_session, APSYNC_PEER, const void *, uint64_t)
    {
        return APRESULT_INVALID_ARGUMENT;
    }

    APRESULT apsync_session_disconnect_peer(apsync_session, APSYNC_PEER)
    {
        return APRESULT_INVALID_ARGUMENT;
    }

#endif // SPUDNET_EXT_TCP
}
