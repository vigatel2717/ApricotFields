// Headless tests for ApSync (include/sync/apsyncnet.h): a host and its
// joiners in one process, talking over the loopback interface.
//
// Written in C on purpose: apsyncnet.h is a C front end, and this proves it
// compiles and links as one.
//
// Loopback is not a network (SPUDNET_TODO.md, "Not covered by the run"):
// nothing here is slow, lost or reordered, so this checks what ApSync does
// with its messages and says nothing about how it stands up to a real link.

#include "sync/apsyncnet.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static const char *current_test = "";

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// How long any one step may take before the test gives it up.
#define PATIENCE_MS 10000u

#define TEST_APPLICATION_ID 0x41505359544553ull
#define TEST_APPLICATION_VERSION 3u

#define CHANNEL_EDITS 0u
#define CHANNEL_PRESENCE 1u

static const APSYNC_CHANNEL_MODE test_channels[2] = {APSYNC_CHANNEL_ORDERED, APSYNC_CHANNEL_LATEST};

static APSYNC_SESSION_DESC session_desc(const char *name, uint32_t application_version)
{
    APSYNC_SESSION_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = (uint32_t)sizeof(desc);
    desc.application_id = TEST_APPLICATION_ID;
    desc.application_version = application_version;
    desc.channels = test_channels;
    desc.channel_count = 2;
    desc.peer_info = name;
    desc.peer_info_size = (uint32_t)strlen(name);
    desc.max_message_size = 1024;
    desc.max_backlog_size = 1024 * 1024;
    desc.heartbeat_interval_ms = 200;
    desc.peer_timeout_ms = 5000;
    desc.handshake_timeout_ms = 5000;
    return desc;
}

static apsync_session host_session(uint16_t *out_port)
{
    static const char *const addresses[1] = {"127.0.0.1"};
    APSYNC_SESSION_DESC desc = session_desc("host", TEST_APPLICATION_VERSION);
    APSYNC_HOST_DESC host_desc;
    memset(&host_desc, 0, sizeof(host_desc));
    host_desc.struct_size = (uint32_t)sizeof(host_desc);
    host_desc.listen_addresses = addresses;
    host_desc.listen_address_count = 1;
    host_desc.port = 0; // the system picks
    host_desc.listen_backlog = 8;
    host_desc.max_peers = 2;

    apsync_session session = NULL;
    APSYNC_ERROR error;
    APRESULT result = apsync_session_host(&desc, &host_desc, &session, &error);
    if (result != APRESULT_OK)
        printf("apsync_session_host: %d %s\n", (int)result, error.message);
    CHECK(result == APRESULT_OK && session != NULL);
    *out_port = 0;
    CHECK(apsync_session_get_port(session, 0, out_port) == APRESULT_OK && *out_port != 0);
    return session;
}

static apsync_session join_session(const char *name, uint32_t application_version, uint16_t port)
{
    APSYNC_SESSION_DESC desc = session_desc(name, application_version);
    APSYNC_JOIN_DESC join_desc;
    memset(&join_desc, 0, sizeof(join_desc));
    join_desc.struct_size = (uint32_t)sizeof(join_desc);
    join_desc.host = "127.0.0.1";
    join_desc.port = port;
    join_desc.family = APSYNC_ADDRESS_FAMILY_IPV4;
    join_desc.resolve_timeout_ms = 5000;
    join_desc.connect_timeout_ms = 5000;
    join_desc.max_snapshot_size = 1024 * 1024;

    apsync_session session = NULL;
    APSYNC_ERROR error;
    APRESULT result = apsync_session_join(&desc, &join_desc, &session, &error);
    if (result != APRESULT_OK)
        printf("apsync_session_join: %d %s\n", (int)result, error.message);
    CHECK(result == APRESULT_OK && session != NULL);
    return session;
}

// The session's next event, waited for. False when none came in time.
static bool next_event(apsync_session session, APSYNC_EVENT *event)
{
    memset(event, 0, sizeof(*event));
    event->struct_size = (uint32_t)sizeof(*event);
    if (apsync_session_wait(session, PATIENCE_MS) != APRESULT_OK)
        return false;
    return apsync_session_poll(session, event) == APRESULT_OK;
}

static bool event_is(const APSYNC_EVENT *event, const char *text)
{
    size_t size = strlen(text);
    return event->size == size && (size == 0 || memcmp(event->data, text, size) == 0);
}

static APRESULT send_text(apsync_session session, uint32_t channel, uint64_t key, const char *text)
{
    return apsync_session_send(session, channel, key, text, (uint32_t)strlen(text));
}

static void test_arguments(void)
{
    current_test = "arguments";
    apsync_session session = NULL;
    APSYNC_ERROR error;
    APSYNC_SESSION_DESC desc = session_desc("x", TEST_APPLICATION_VERSION);
    APSYNC_JOIN_DESC join_desc;
    memset(&join_desc, 0, sizeof(join_desc));
    join_desc.struct_size = (uint32_t)sizeof(join_desc);
    join_desc.host = "127.0.0.1";
    join_desc.port = 1;
    join_desc.resolve_timeout_ms = 1000;
    join_desc.connect_timeout_ms = 1000;
    join_desc.max_snapshot_size = 1024;

    CHECK(apsync_session_join(NULL, &join_desc, &session, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(apsync_session_join(&desc, NULL, &session, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(apsync_session_join(&desc, &join_desc, NULL, &error) == APRESULT_INVALID_ARGUMENT);

    // Nothing has a default: a limit left at 0 is refused.
    APSYNC_SESSION_DESC no_limit = desc;
    no_limit.max_message_size = 0;
    CHECK(apsync_session_join(&no_limit, &join_desc, &session, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(session == NULL && error.message[0] != '\0');
    APSYNC_SESSION_DESC no_channels = desc;
    no_channels.channel_count = 0;
    CHECK(apsync_session_join(&no_channels, &join_desc, &session, NULL) == APRESULT_INVALID_ARGUMENT);
    APSYNC_JOIN_DESC no_time = join_desc;
    no_time.connect_timeout_ms = 0;
    CHECK(apsync_session_join(&desc, &no_time, &session, NULL) == APRESULT_INVALID_ARGUMENT);
    APSYNC_SESSION_DESC too_small = desc;
    too_small.struct_size = 8;
    CHECK(apsync_session_join(&too_small, &join_desc, &session, NULL) == APRESULT_INVALID_ARGUMENT);

    APSYNC_EVENT event;
    memset(&event, 0, sizeof(event));
    event.struct_size = (uint32_t)sizeof(event);
    CHECK(apsync_session_poll(NULL, &event) == APRESULT_INVALID_ARGUMENT);
    CHECK(apsync_session_send(NULL, 0, 0, "a", 1) == APRESULT_INVALID_ARGUMENT);
    CHECK(apsync_session_get_local_peer(NULL) == APSYNC_PEER_NONE);
    apsync_session_wake(NULL);
    apsync_session_destroy(NULL);
}

// A joiner arrives while the host has one ordered message applied and one
// numbered but not yet polled. It has to get the snapshot that includes the
// first, and then the second, and nothing twice.
static void test_join_and_messages(void)
{
    current_test = "join_and_messages";
    uint16_t port = 0;
    apsync_session host = host_session(&port);
    APSYNC_EVENT event;

    CHECK(apsync_session_get_local_peer(host) == APSYNC_PEER_HOST);
    memset(&event, 0, sizeof(event));
    CHECK(apsync_session_poll(host, &event) == APRESULT_INVALID_ARGUMENT); // struct_size not set
    event.struct_size = (uint32_t)sizeof(event);
    CHECK(apsync_session_poll(host, &event) == APRESULT_NOT_FOUND);

    // The host's own ordered message comes back to it, numbered.
    CHECK(send_text(host, CHANNEL_EDITS, 0, "one") == APRESULT_OK);
    CHECK(next_event(host, &event));
    CHECK(event.type == APSYNC_EVENT_MESSAGE && event.peer == APSYNC_PEER_HOST && event.channel == CHANNEL_EDITS);
    CHECK(event.sequence == 1 && event_is(&event, "one"));

    apsync_session joiner = join_session("ada", TEST_APPLICATION_VERSION, port);
    CHECK(apsync_session_get_local_peer(joiner) == APSYNC_PEER_NONE);
    CHECK(send_text(joiner, CHANNEL_EDITS, 0, "early") == APRESULT_APSYNC_NOT_CONNECTED);

    CHECK(next_event(host, &event));
    CHECK(event.type == APSYNC_EVENT_SNAPSHOT_REQUEST && event_is(&event, "ada"));
    APSYNC_PEER ada = event.peer;
    CHECK(ada > APSYNC_PEER_HOST);

    // Numbered by the host's thread, but not polled: not in the snapshot.
    CHECK(send_text(host, CHANNEL_EDITS, 0, "two") == APRESULT_OK);
    CHECK(apsync_session_send_snapshot(host, ada, "state after one", 15) == APRESULT_OK);
    CHECK(apsync_session_send_snapshot(host, ada, "again", 5) == APRESULT_NOT_FOUND);
    CHECK(apsync_session_send_snapshot(host, 999, "nobody", 6) == APRESULT_NOT_FOUND);

    // The host sees "two" come back and the joiner arrive, in whichever
    // order its thread took the two commands - which is the order sent.
    CHECK(next_event(host, &event));
    CHECK(event.type == APSYNC_EVENT_MESSAGE && event.sequence == 2 && event_is(&event, "two"));
    CHECK(next_event(host, &event));
    CHECK(event.type == APSYNC_EVENT_PEER_JOINED && event.peer == ada && event_is(&event, "ada"));

    CHECK(next_event(joiner, &event));
    CHECK(event.type == APSYNC_EVENT_CONNECTED && event.peer == ada);
    CHECK(apsync_session_get_local_peer(joiner) == ada);
    CHECK(next_event(joiner, &event));
    CHECK(event.type == APSYNC_EVENT_SNAPSHOT && event.sequence == 1 && event_is(&event, "state after one"));
    CHECK(next_event(joiner, &event));
    CHECK(event.type == APSYNC_EVENT_PEER_JOINED && event.peer == APSYNC_PEER_HOST && event_is(&event, "host"));
    CHECK(next_event(joiner, &event));
    CHECK(event.type == APSYNC_EVENT_MESSAGE && event.sequence == 2 && event.peer == APSYNC_PEER_HOST);
    CHECK(event_is(&event, "two"));

    // A joiner's ordered message goes to the host for its number and comes
    // back to both.
    CHECK(send_text(joiner, CHANNEL_EDITS, 0, "three") == APRESULT_OK);
    CHECK(next_event(host, &event));
    CHECK(event.type == APSYNC_EVENT_MESSAGE && event.sequence == 3 && event.peer == ada && event_is(&event, "three"));
    CHECK(next_event(joiner, &event));
    CHECK(event.type == APSYNC_EVENT_MESSAGE && event.sequence == 3 && event.peer == ada && event_is(&event, "three"));

    // A latest value goes to the others and not back to its sender.
    CHECK(send_text(joiner, CHANNEL_PRESENCE, 7, "cursor") == APRESULT_OK);
    CHECK(next_event(host, &event));
    CHECK(event.type == APSYNC_EVENT_MESSAGE && event.channel == CHANNEL_PRESENCE && event.peer == ada);
    CHECK(event.key == 7 && event.sequence == 0 && event_is(&event, "cursor"));
    CHECK(send_text(host, CHANNEL_PRESENCE, 9, "") == APRESULT_OK);
    CHECK(next_event(joiner, &event));
    CHECK(event.type == APSYNC_EVENT_MESSAGE && event.channel == CHANNEL_PRESENCE && event.peer == APSYNC_PEER_HOST);
    CHECK(event.key == 9 && event.size == 0);
    event.struct_size = (uint32_t)sizeof(event);
    CHECK(apsync_session_poll(host, &event) == APRESULT_NOT_FOUND);

    // What a send refuses.
    char big[2048];
    memset(big, 'x', sizeof(big));
    CHECK(apsync_session_send(joiner, CHANNEL_EDITS, 0, big, sizeof(big)) == APRESULT_LIMIT);
    CHECK(send_text(joiner, 2, 0, "no such channel") == APRESULT_INVALID_ARGUMENT);
    CHECK(send_text(joiner, CHANNEL_EDITS, 5, "a key on an ordered channel") == APRESULT_INVALID_ARGUMENT);
    CHECK(apsync_session_send_snapshot(joiner, ada, "x", 1) == APRESULT_INVALID_ARGUMENT);
    CHECK(apsync_session_disconnect_peer(host, APSYNC_PEER_HOST) == APRESULT_INVALID_ARGUMENT);

    // A wake ends a wait that has nothing to report.
    apsync_session_wake(host);
    CHECK(apsync_session_wait(host, PATIENCE_MS) == APRESULT_NOT_FOUND);

    // The joiner goes; the host is told.
    apsync_session_destroy(joiner);
    CHECK(next_event(host, &event));
    CHECK(event.type == APSYNC_EVENT_PEER_LEFT && event.peer == ada);
    CHECK(event.message != NULL);

    apsync_session_destroy(host);
}

// A second joiner sees the first; a peer the host drops is told nothing and
// the others are told why.
static void test_roster_and_disconnect(void)
{
    current_test = "roster_and_disconnect";
    uint16_t port = 0;
    apsync_session host = host_session(&port);
    APSYNC_EVENT event;

    apsync_session first = join_session("ada", TEST_APPLICATION_VERSION, port);
    CHECK(next_event(host, &event) && event.type == APSYNC_EVENT_SNAPSHOT_REQUEST);
    APSYNC_PEER ada = event.peer;
    CHECK(apsync_session_send_snapshot(host, ada, NULL, 0) == APRESULT_OK);
    CHECK(next_event(host, &event) && event.type == APSYNC_EVENT_PEER_JOINED && event.peer == ada);
    CHECK(next_event(first, &event) && event.type == APSYNC_EVENT_CONNECTED);
    CHECK(next_event(first, &event) && event.type == APSYNC_EVENT_SNAPSHOT && event.size == 0 && event.sequence == 0);
    CHECK(next_event(first, &event) && event.type == APSYNC_EVENT_PEER_JOINED && event.peer == APSYNC_PEER_HOST);

    apsync_session second = join_session("bo", TEST_APPLICATION_VERSION, port);
    CHECK(next_event(host, &event) && event.type == APSYNC_EVENT_SNAPSHOT_REQUEST && event_is(&event, "bo"));
    APSYNC_PEER bo = event.peer;
    CHECK(bo != ada && bo > APSYNC_PEER_HOST);
    CHECK(apsync_session_send_snapshot(host, bo, "s", 1) == APRESULT_OK);
    CHECK(next_event(host, &event) && event.type == APSYNC_EVENT_PEER_JOINED && event.peer == bo);

    CHECK(next_event(second, &event) && event.type == APSYNC_EVENT_CONNECTED && event.peer == bo);
    CHECK(next_event(second, &event) && event.type == APSYNC_EVENT_SNAPSHOT && event_is(&event, "s"));
    CHECK(next_event(second, &event) && event.type == APSYNC_EVENT_PEER_JOINED && event.peer == APSYNC_PEER_HOST);
    CHECK(next_event(second, &event) && event.type == APSYNC_EVENT_PEER_JOINED && event.peer == ada);
    CHECK(event_is(&event, "ada"));
    CHECK(next_event(first, &event) && event.type == APSYNC_EVENT_PEER_JOINED && event.peer == bo);
    CHECK(event_is(&event, "bo"));

    // max_peers is 2: a third is turned away.
    apsync_session third = join_session("cy", TEST_APPLICATION_VERSION, port);
    // The refusal goes out just ahead of the close, and which of the two the
    // joiner makes out first is the network's to say.
    CHECK(next_event(third, &event) && event.type == APSYNC_EVENT_DISCONNECTED);
    CHECK(event.reason == APSYNC_REASON_REFUSED_FULL || event.reason == APSYNC_REASON_CLOSED ||
          event.reason == APSYNC_REASON_NETWORK);
    CHECK(send_text(third, CHANNEL_EDITS, 0, "x") == APRESULT_APSYNC_NOT_CONNECTED);
    apsync_session_destroy(third);

    // One joiner's latest value reaches the other through the host.
    CHECK(send_text(first, CHANNEL_PRESENCE, 1, "here") == APRESULT_OK);
    CHECK(next_event(second, &event) && event.type == APSYNC_EVENT_MESSAGE && event.peer == ada);
    CHECK(event.key == 1 && event_is(&event, "here"));
    CHECK(next_event(host, &event) && event.type == APSYNC_EVENT_MESSAGE && event.peer == ada);

    CHECK(apsync_session_disconnect_peer(host, ada) == APRESULT_OK);
    CHECK(next_event(host, &event) && event.type == APSYNC_EVENT_PEER_LEFT && event.peer == ada);
    CHECK(event.reason == APSYNC_REASON_DISCONNECTED_BY_HOST);
    CHECK(next_event(second, &event) && event.type == APSYNC_EVENT_PEER_LEFT && event.peer == ada);
    CHECK(event.reason == APSYNC_REASON_DISCONNECTED_BY_HOST);
    CHECK(next_event(first, &event) && event.type == APSYNC_EVENT_DISCONNECTED);
    CHECK(event.reason == APSYNC_REASON_CLOSED || event.reason == APSYNC_REASON_NETWORK);

    // The host goes; whoever is left is told the session is over.
    apsync_session_destroy(host);
    CHECK(next_event(second, &event) && event.type == APSYNC_EVENT_DISCONNECTED);
    apsync_session_destroy(first);
    apsync_session_destroy(second);
}

static void test_refusals(void)
{
    current_test = "refusals";
    uint16_t port = 0;
    apsync_session host = host_session(&port);
    APSYNC_EVENT event;

    // Another version of the application's messages.
    apsync_session other = join_session("old", TEST_APPLICATION_VERSION + 1, port);
    CHECK(next_event(other, &event) && event.type == APSYNC_EVENT_DISCONNECTED);
    CHECK(event.reason == APSYNC_REASON_REFUSED_APPLICATION || event.reason == APSYNC_REASON_CLOSED ||
          event.reason == APSYNC_REASON_NETWORK);
    apsync_session_destroy(other);
    event.struct_size = (uint32_t)sizeof(event);
    CHECK(apsync_session_poll(host, &event) == APRESULT_NOT_FOUND); // the host never heard of it

    // A port the first host already has.
    static const char *const addresses[1] = {"127.0.0.1"};
    APSYNC_SESSION_DESC desc = session_desc("second host", TEST_APPLICATION_VERSION);
    APSYNC_HOST_DESC host_desc;
    memset(&host_desc, 0, sizeof(host_desc));
    host_desc.struct_size = (uint32_t)sizeof(host_desc);
    host_desc.listen_addresses = addresses;
    host_desc.listen_address_count = 1;
    host_desc.port = port;
    host_desc.listen_backlog = 8;
    host_desc.max_peers = 2;
    apsync_session second = NULL;
    APSYNC_ERROR error;
    CHECK(apsync_session_host(&desc, &host_desc, &second, &error) == APRESULT_APSYNC_ADDRESS_IN_USE);
    CHECK(second == NULL && error.message[0] != '\0');

    // Destroyed while a join is still under way, and with nobody listening.
    apsync_session_destroy(host);
    apsync_session nobody = join_session("late", TEST_APPLICATION_VERSION, port);
    apsync_session_destroy(nobody);
    nobody = join_session("late", TEST_APPLICATION_VERSION, port);
    CHECK(next_event(nobody, &event) && event.type == APSYNC_EVENT_DISCONNECTED);
    CHECK(event.reason == APSYNC_REASON_CONNECT_FAILED || event.reason == APSYNC_REASON_TIMED_OUT);
    apsync_session_destroy(nobody);
}

int main(void)
{
    test_arguments();
    test_join_and_messages();
    test_roster_and_disconnect();
    test_refusals();
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("apsync_tests: all passed\n");
    return 0;
}
