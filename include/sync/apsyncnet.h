#ifndef APSYNC_NET_H
#define APSYNC_NET_H

#include "../apricore.h"
#include <stdbool.h>
#include <stdint.h>

#if __cplusplus
extern "C" {
#endif

/*
 * ApSync - sessions, channels and snapshots between several running copies
 * of one application.
 *
 * General purpose: it knows nothing about what is being kept in step. Every
 * message, every snapshot and every peer's description is bytes the caller
 * wrote and the caller reads. What ApSync owns is what lies between those
 * bytes and a socket, and is the same whatever they mean:
 *
 * - Sessions. One copy hosts, the others join it. Each has a peer id; who
 *   is in the session, and who came and went, arrives as events.
 * - Channels. Each has one of two ways of delivering:
 *     ORDERED  nothing is lost, and every peer receives every message in
 *              the same order, which the host decides. For what has to
 *              agree everywhere - edits to a shared model.
 *     LATEST   per key, only the newest value matters: a value still
 *              waiting to go out is replaced by the next one for its key.
 *              For what goes stale - a cursor, a selection, a drag under
 *              way, a camera.
 * - Snapshots. A peer that joins late gets the host application's state as
 *   one block, then exactly the ordered messages that came after it.
 * - The network itself: a thread of ApSync's own does all of it, so no call
 *   here except apsync_session_wait waits on anything.
 *
 * The shape is a star. Every joined peer talks to the host only, and the
 * host passes on what the others need. The host is the one copy that has to
 * stay up: when it goes, the session is over for everyone.
 *
 * How ordered channels are meant to be used. A peer does not change its
 * state when it sends an ordered message; it changes it when the message
 * comes back in apsync_session_poll, which it does for the peer that sent
 * it as for everyone else (the host included). Every peer then applies the
 * same messages in the same order to the same starting snapshot, and that
 * is all that keeps them in agreement - two edits that conflict are settled
 * by which the host numbered first, and how the loser is treated is the
 * caller's rule. A caller that wants its own edit on screen before the
 * round trip shows it provisionally and settles up when the message
 * arrives.
 *
 * What ApSync does not do, on purpose. It doesn't reconnect: a joined
 * session whose connection ends is finished, and the caller makes another,
 * which starts from a fresh snapshot. It doesn't store anything. It doesn't
 * decide who may join beyond the application id and version matching; an
 * unwelcome peer is turned away by the host application with
 * apsync_session_disconnect_peer.
 *
 * Transport, today: plain TCP (SpudNet's sockets), so nothing is encrypted
 * and nothing proves who a peer is. That is right for a local network the
 * users trust and for nothing else. APSYNC_TODO.md has what a second
 * transport would bring and where it fits.
 *
 * Nothing has to be set up first. Each session starts the platform's
 * networking for itself (a spudnet_instance of its own, spudnet.h) and
 * gives it back when it is destroyed, whatever else in the process uses
 * the network.
 *
 * Not on every platform. Where SpudNet has no TCP (watchOS), the two calls
 * that make a session return APRESULT_UNSUPPORTED.
 */

/* ---- Conventions ----------------------------------------------------- */

/*
 * Results. Calls return APRESULT (apricore.h): APRESULT_OK, a general code,
 * or an APRESULT_APSYNC_* one. The general codes mean, here:
 *   APRESULT_INVALID_ARGUMENT  also a desc with a field left at a value its
 *                              comment refuses, and a call made on a
 *                              session of the other role
 *   APRESULT_IO                the network, or the system under it, failed
 *   APRESULT_NOT_FOUND         no event, or no such peer
 *   APRESULT_LIMIT             over a limit the desc set
 *   APRESULT_UNSUPPORTED       this platform has no transport ApSync uses,
 *                              or, on Linux, its libcurl isn't thread-safe
 *                              (SpudNet needs one that is)
 *
 * Error messages. The calls that can fail for a reason worth a sentence
 * take an APSYNC_ERROR pointer (NULL to skip it), as aparchive.h's do. What
 * goes wrong later, on the network, arrives as an event instead, with its
 * own message.
 *
 * Struct versions. Every struct here starts with [struct_size], which the
 * caller sets to sizeof(the struct) as it was compiled, on output structs
 * too (APSYNC_EVENT). The rule is aparchive.h's: fields are only added at
 * the end, and a size smaller than the first version fails with
 * APRESULT_INVALID_ARGUMENT. APSYNC_ERROR is frozen and has none.
 *
 * Nothing in a desc has a default. A limit or a time left at 0 is refused
 * unless its comment says what 0 means.
 *
 * Threads. One thread at a time in a session's calls. The exceptions are
 * apsync_session_wait, which may be on a second thread while the first
 * makes the others, and apsync_session_wake, which may come from any
 * thread; neither may race apsync_session_destroy. ApSync never calls back:
 * nothing of the caller's runs on its thread.
 */

#define APSYNC_ERROR_MESSAGE_SIZE 256

typedef struct APSYNC_ERROR {
	char message[APSYNC_ERROR_MESSAGE_SIZE]; /* NUL-terminated, truncated to fit */
} APSYNC_ERROR;

/* ---- Peers ----------------------------------------------------------- */

/* A peer's id within one session. The host gives them out, starting above
 * its own, and never gives one out twice in a session. */
typedef uint32_t APSYNC_PEER;

#define APSYNC_PEER_NONE 0u
/* The host's own id, in every session. */
#define APSYNC_PEER_HOST 1u

/* The most bytes a peer may describe itself with (APSYNC_SESSION_DESC's
 * [peer_info]). */
#define APSYNC_MAX_PEER_INFO_SIZE 4096u

/* ---- Channels -------------------------------------------------------- */

typedef enum APSYNC_CHANNEL_MODE {
	/* Reliable, and in one order for everyone. The message goes to the
	 * host, which numbers it and sends it to every peer, its sender
	 * included. */
	APSYNC_CHANNEL_ORDERED = 1,
	/* Newest value per key. Goes to every other peer and not back to its
	 * sender. Never lost in transit, but a value may be skipped: where a
	 * newer one for the same key from the same peer catches up with it
	 * before it has been sent on, the older is dropped.
	 *
	 * A peer that joins sees a key at its next send and not before. A peer
	 * with something a newcomer should see sends it again when
	 * APSYNC_EVENT_PEER_JOINED arrives. Values are not withdrawn when their
	 * sender leaves; receivers drop them on APSYNC_EVENT_PEER_LEFT. */
	APSYNC_CHANNEL_LATEST = 2,
} APSYNC_CHANNEL_MODE;

/* The most channels a session may have. */
#define APSYNC_MAX_CHANNELS 256u

/* ---- Making a session ------------------------------------------------ */

/* What the host and every joiner have to agree on, and what a session of
 * either role needs. */
typedef struct APSYNC_SESSION_DESC {
	uint32_t struct_size;

	/* Which application this is and which version of its messages it
	 * speaks. A joiner whose pair differs from the host's is turned away
	 * (APSYNC_REASON_REFUSED_APPLICATION): everything else in a session is
	 * bytes only the two of them can read, so a mismatch here is the one
	 * thing ApSync can catch for them. The channel list and
	 * [max_message_size] are part of what a version means; ApSync doesn't
	 * compare them. */
	uint64_t application_id;
	uint32_t application_version;

	/* The session's channels: [channels][i] is how channel i delivers. At
	 * least one, at most APSYNC_MAX_CHANNELS. Copied. */
	const APSYNC_CHANNEL_MODE *channels;
	uint32_t channel_count;

	/* What this peer tells the others about itself - a name, a colour; the
	 * caller's format. Every other peer gets it with
	 * APSYNC_EVENT_PEER_JOINED. May be NULL with a size of 0. At most
	 * APSYNC_MAX_PEER_INFO_SIZE. Copied. */
	const void *peer_info;
	uint32_t peer_info_size;

	/* The largest message this session sends or accepts, in bytes. A peer
	 * that sends a larger one is taken to be broken and dropped
	 * (APSYNC_REASON_PROTOCOL). 0 is refused. */
	uint32_t max_message_size;

	/* How many bytes may wait to be sent to one peer. Hosting: a peer that
	 * falls further behind than this is dropped (APSYNC_REASON_BACKLOG) -
	 * it isn't reading, and an ordered channel can't skip anything to let
	 * it catch up. Joined: apsync_session_send returns APRESULT_LIMIT while
	 * this much is already waiting for the host. A snapshot on its way to
	 * a joiner doesn't count. 0 is refused. */
	uint64_t max_backlog_size;

	/* How long a connection may go without this side sending anything
	 * before it sends a few bytes to show it is alive, and how long it may
	 * go without receiving anything before the other end is taken to be
	 * gone (APSYNC_REASON_TIMED_OUT). Both sides of a connection use their
	 * own; a time-out shorter than the other end's interval drops healthy
	 * peers, so the two are part of what an application version means.
	 * Either may be 0, which turns that half off: with no time-out, a peer
	 * that vanishes without closing is only noticed when the operating
	 * system gives up on it, which can be many minutes. */
	uint32_t heartbeat_interval_ms;
	uint32_t peer_timeout_ms;

	/* How long the opening exchange may take once a connection is up: for
	 * the host, a newcomer saying what it is; for a joiner, the host's
	 * answer. Something that connects and says nothing is dropped after
	 * this. 0 is refused. */
	uint32_t handshake_timeout_ms;
} APSYNC_SESSION_DESC;

typedef struct APSYNC_HOST_DESC {
	uint32_t struct_size;

	/* The local addresses to listen on, numeric, as SpudNet takes them:
	 * "0.0.0.0" for every IPv4 interface, "::" for every IPv6 one,
	 * "127.0.0.1" for this machine only, or an interface's own. One
	 * listener each; listening on both families is two entries. At least
	 * one, at most APSYNC_MAX_LISTEN_ADDRESSES. */
	const char *const *listen_addresses;
	uint32_t listen_address_count;

	/* The port, on every address. 0 has the system pick one per address;
	 * apsync_session_get_port says which. */
	uint16_t port;

	/* How many connections may wait to be accepted before the system turns
	 * new ones away. 0 is refused. */
	uint32_t listen_backlog;

	/* The most peers that may be joined or joining at once, not counting
	 * the host. One more is turned away (APSYNC_REASON_REFUSED_FULL). 0 is
	 * refused. */
	uint32_t max_peers;
} APSYNC_HOST_DESC;

#define APSYNC_MAX_LISTEN_ADDRESSES 8u

typedef enum APSYNC_ADDRESS_FAMILY {
	APSYNC_ADDRESS_FAMILY_ANY = 0, /* whichever the name has */
	APSYNC_ADDRESS_FAMILY_IPV4 = 1,
	APSYNC_ADDRESS_FAMILY_IPV6 = 2,
} APSYNC_ADDRESS_FAMILY;

typedef struct APSYNC_JOIN_DESC {
	uint32_t struct_size;

	/* The host: a name or a numeric address, UTF-8. Copied. */
	const char *host;
	uint16_t port;

	/* Limits a name's addresses to one family. They are tried in the order
	 * the system's resolver gives them, until one connects. */
	APSYNC_ADDRESS_FAMILY family;

	/* How long looking the name up may take, and how long connecting to
	 * each address may. 0 is refused for both. */
	uint32_t resolve_timeout_ms;
	uint32_t connect_timeout_ms;

	/* The largest snapshot this peer will take from the host, in bytes. A
	 * larger one ends the session (APSYNC_REASON_SNAPSHOT_TOO_BIG) before
	 * any of it is kept. 0 is refused. */
	uint64_t max_snapshot_size;
} APSYNC_JOIN_DESC;

typedef struct apsync_session_t *apsync_session;

/* Starts a session and listens for peers. Listening has begun when this
 * returns, and a failure to is this call's: APRESULT_APSYNC_ADDRESS_IN_USE
 * when another listener holds one of the addresses, APRESULT_IO otherwise,
 * with the system's reason in [out_error]. */
APRESULT apsync_session_host(const APSYNC_SESSION_DESC *desc, const APSYNC_HOST_DESC *host_desc,
                             apsync_session *out_session, APSYNC_ERROR *out_error);

/* Starts joining a host and returns at once; nothing has been connected to
 * yet. The rest arrives as events: APSYNC_EVENT_CONNECTED, then
 * APSYNC_EVENT_SNAPSHOT, then the session's traffic - or, if any of it
 * fails, APSYNC_EVENT_DISCONNECTED and nothing more. */
APRESULT apsync_session_join(const APSYNC_SESSION_DESC *desc, const APSYNC_JOIN_DESC *join_desc,
                             apsync_session *out_session, APSYNC_ERROR *out_error);

/* Ends the session for this peer and frees it. What was still waiting to be
 * sent may not be. Doesn't wait on the network; it can wait a moment for
 * ApSync's thread to come back. NULL does nothing. */
void apsync_session_destroy(apsync_session session);

/* Hosting: the port being listened on for [listen_addresses][index] - the
 * desc's, or the one the system picked for 0. */
APRESULT apsync_session_get_port(apsync_session session, uint32_t index, uint16_t *out_port);

/* This peer's id: APSYNC_PEER_HOST when hosting; when joined, the id the
 * host gave, or APSYNC_PEER_NONE until APSYNC_EVENT_CONNECTED has been
 * polled. */
APSYNC_PEER apsync_session_get_local_peer(apsync_session session);

/* ---- Events ---------------------------------------------------------- */

typedef enum APSYNC_EVENT_TYPE {
	APSYNC_EVENT_NONE = 0,

	/* Joined only: the host has let this peer in. [peer] is its id. The
	 * snapshot is still to come and nothing may be sent yet. */
	APSYNC_EVENT_CONNECTED = 1,

	/* Joined only, once: the host application's state. [data] and [size]
	 * are what it passed to apsync_session_send_snapshot, and [sequence]
	 * the number of the last ordered message that state includes. Every
	 * ordered message from here on has a higher one. Sending is allowed
	 * once this has been polled. */
	APSYNC_EVENT_SNAPSHOT = 2,

	/* Hosting only: [peer] has connected and needs a snapshot; [data] is
	 * its peer info. Answer with apsync_session_send_snapshot, or turn it
	 * away with apsync_session_disconnect_peer. Until one or the other the
	 * peer is not in the session: it is sent nothing and nobody is told of
	 * it. */
	APSYNC_EVENT_SNAPSHOT_REQUEST = 3,

	/* [peer] is in the session; [data] is its peer info. Hosting: its
	 * snapshot has been queued. Joined: also sent once for each peer that
	 * was already there, the host among them, right after the snapshot. */
	APSYNC_EVENT_PEER_JOINED = 4,

	/* [peer] is gone, for [reason], with [message] where there is more to
	 * say. Hosting: also for a peer that asked for a snapshot and left
	 * before getting one. Joined: [reason] is the host's, as it saw that
	 * peer go. */
	APSYNC_EVENT_PEER_LEFT = 5,

	/* A message on [channel] from [peer]. Ordered: [sequence] is its place
	 * in the session's one order, counted from 1 across all ordered
	 * channels, and [key] is 0. Latest: [key] is the sender's and
	 * [sequence] is 0. */
	APSYNC_EVENT_MESSAGE = 6,

	/* Joined: the session is over for this peer - it couldn't get in, or
	 * the connection to the host ended. Nothing follows, and only
	 * apsync_session_destroy is left. Hosting: a listener failed and no
	 * longer takes newcomers at its address; the session carries on with
	 * the peers it has. [reason] and [message] say why.
	 *
	 * Either role, with APSYNC_REASON_INTERNAL: ApSync's thread could not
	 * go on, and the session is over. */
	APSYNC_EVENT_DISCONNECTED = 7,
} APSYNC_EVENT_TYPE;

/* Why a peer left or a session ended. */
typedef enum APSYNC_REASON {
	APSYNC_REASON_NONE = 0,
	/* The other end closed the connection. */
	APSYNC_REASON_CLOSED = 1,
	/* The network or the system failed; [message] has the platform's own
	 * code for a log. */
	APSYNC_REASON_NETWORK = 2,
	/* Nothing was heard for the desc's [peer_timeout_ms], the opening
	 * exchange outran [handshake_timeout_ms], or the lookup or every
	 * connection attempt outran its limit. */
	APSYNC_REASON_TIMED_OUT = 3,
	/* The other end sent something that isn't ApSync's, or a message over
	 * [max_message_size], or one the channel list doesn't allow. */
	APSYNC_REASON_PROTOCOL = 4,
	/* The host has a different [application_id] or
	 * [application_version], or an ApSync that speaks another version of
	 * its own wire format. */
	APSYNC_REASON_REFUSED_APPLICATION = 5,
	/* The host has [max_peers] already. */
	APSYNC_REASON_REFUSED_FULL = 6,
	/* The peer fell [max_backlog_size] behind. */
	APSYNC_REASON_BACKLOG = 7,
	/* The host application dropped the peer
	 * (apsync_session_disconnect_peer). The peer it was used on sees
	 * APSYNC_REASON_CLOSED itself. */
	APSYNC_REASON_DISCONNECTED_BY_HOST = 8,
	/* The host's name has no address. */
	APSYNC_REASON_RESOLVE_FAILED = 9,
	/* No address of the host's took the connection. */
	APSYNC_REASON_CONNECT_FAILED = 10,
	/* The snapshot is over the join desc's [max_snapshot_size]. */
	APSYNC_REASON_SNAPSHOT_TOO_BIG = 11,
	/* ApSync ran out of memory, or couldn't start a thread. */
	APSYNC_REASON_INTERNAL = 12,
} APSYNC_REASON;

typedef struct APSYNC_EVENT {
	uint32_t struct_size;
	APSYNC_EVENT_TYPE type;
	APSYNC_PEER peer;
	uint32_t channel;
	uint64_t sequence;
	uint64_t key;
	APSYNC_REASON reason;
	/* The event's bytes, or NULL with a size of 0. ApSync's, and good until
	 * the next apsync_session_poll on this session or its destroy. */
	const void *data;
	uint64_t size;
	/* Never NULL; empty where there is nothing to add. Same lifetime as
	 * [data]. For a log: not for parsing, and not in any fixed language. */
	const char *message;
} APSYNC_EVENT;

/* Takes the next event, in the order things happened. APRESULT_NOT_FOUND
 * when there is none, which is not a failure. Never waits. Events are kept
 * until they are polled, however long that is, so a session that is not
 * polled grows. */
APRESULT apsync_session_poll(apsync_session session, APSYNC_EVENT *out_event);

/* Waits up to [timeout_ms] for there to be an event to poll, for a caller
 * with no frame loop to poll from. APSYNC_WAIT_FOREVER sets no limit.
 * APRESULT_OK when there is one, APRESULT_NOT_FOUND when the time ran out
 * or apsync_session_wake ended the wait first. */
APRESULT apsync_session_wait(apsync_session session, uint32_t timeout_ms);

#define APSYNC_WAIT_FOREVER 0xFFFFFFFFu

/* Ends an apsync_session_wait in progress, or the next one when none is.
 * From any thread. */
void apsync_session_wake(apsync_session session);

/* ---- Sending --------------------------------------------------------- */

/* Sends [size] bytes on [channel]. [key] is the value's key on a latest
 * channel and must be 0 on an ordered one. The data is copied; the call
 * never waits on the network. A size of 0 is a message like any other.
 *
 * Ordered: the message comes back through apsync_session_poll, for this
 * peer too, and this peer's own come back in the order it sent them.
 *
 * APRESULT_APSYNC_NOT_CONNECTED on a joined session before
 * APSYNC_EVENT_SNAPSHOT has been polled or after
 * APSYNC_EVENT_DISCONNECTED has. APRESULT_LIMIT for a message over
 * [max_message_size], or, joined, while [max_backlog_size] is waiting for
 * the host: nothing was queued, and the caller tries again later or gives
 * the session up. */
APRESULT apsync_session_send(apsync_session session, uint32_t channel, uint64_t key, const void *data,
                             uint32_t size);

/* Hosting: answers an APSYNC_EVENT_SNAPSHOT_REQUEST. [data] is the
 * application's state as it stands with every event polled so far applied
 * and nothing else - so it is made, and this called, without polling in
 * between. ApSync notes which ordered message that state ends at and sends
 * [peer] the ones after it. The data is copied. May be NULL with a size of
 * 0, for an application whose state is all in its messages.
 *
 * The peer is in the session from here: it gets APSYNC_EVENT_PEER_JOINED
 * for everyone present, everyone present gets one for it, and this session
 * polls one too. APRESULT_NOT_FOUND when [peer] has no request waiting -
 * also when it has left, which APSYNC_EVENT_PEER_LEFT will say. */
APRESULT apsync_session_send_snapshot(apsync_session session, APSYNC_PEER peer, const void *data, uint64_t size);

/* Hosting: drops [peer], whether it is in the session or still waiting for
 * a snapshot. APSYNC_EVENT_PEER_LEFT follows either way.
 * APRESULT_INVALID_ARGUMENT for APSYNC_PEER_NONE and APSYNC_PEER_HOST. A
 * peer that has already gone is not an error. */
APRESULT apsync_session_disconnect_peer(apsync_session session, APSYNC_PEER peer);

#if __cplusplus
}
#endif

#endif // APSYNC_NET_H
