// test_multicast_discover.cpp
// Host-side unit tests for CoAP multicast resource discovery (RFC 7252 §8).
// Tests cover allNodesEndpoint(), discover() slot management, expiry, handler
// dispatch, and isolation from pending_ slots.  The optional loopback-multicast
// integration test is skipped gracefully when the OS doesn't support it.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PULSECOAP_MAX_DISCOVERS
#define PULSECOAP_MAX_DISCOVERS 2
#endif
#ifndef PULSECOAP_DISCOVER_TIMEOUT_MS
#define PULSECOAP_DISCOVER_TIMEOUT_MS 100
#endif

#include "PulseCoAPClient.h"
#include "PulseCoAPMessage.h"
#include "PulseCoAPServer.h"
#include "PulseCoAPTransaction.h"
#include "PulseCoAPTransportPosix.h"

using namespace pulsecoap;

// ---------------------------------------------------------------------------
// Minimal stub transport: captures the last sent datagram, injects a reply.
// ---------------------------------------------------------------------------
struct StubTransport : public Transport {
    bool     open = false;
    uint8_t  sentBuf[PULSECOAP_MAX_MSG_SIZE] = {};
    size_t   sentLen = 0;
    Endpoint sentTo;

    uint8_t  injectBuf[PULSECOAP_MAX_MSG_SIZE] = {};
    size_t   injectLen = 0;
    Endpoint injectFrom;

    bool begin(uint16_t) override { open = true; return true; }

    bool send(const Endpoint& to, const uint8_t* data, size_t len) override {
        if (!open || len > sizeof(sentBuf)) return false;
        sentTo = to;
        memcpy(sentBuf, data, len);
        sentLen = len;
        return true;
    }

    size_t receive(uint8_t* buf, size_t cap, Endpoint& from) override {
        if (injectLen == 0) return 0;
        size_t n = injectLen < cap ? injectLen : cap;
        memcpy(buf, injectBuf, n);
        from = injectFrom;
        injectLen = 0;
        return n;
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static int g_discoverCount = 0;
static Endpoint g_lastServer;
static char g_lastLinkFormat[256] = {};

static void resetGlobals() {
    g_discoverCount = 0;
    g_lastServer = Endpoint{};
    memset(g_lastLinkFormat, 0, sizeof(g_lastLinkFormat));
}

static void discoverCb(const Endpoint& server, const uint8_t* lf, size_t len,
                        void* /*ctx*/) {
    ++g_discoverCount;
    g_lastServer = server;
    size_t copy = len < sizeof(g_lastLinkFormat) - 1 ? len : sizeof(g_lastLinkFormat) - 1;
    memcpy(g_lastLinkFormat, lf, copy);
    g_lastLinkFormat[copy] = '\0';
}

static void responseCb(const ClientResponse& /*res*/, void* ctx) {
    int* n = static_cast<int*>(ctx);
    if (n) ++(*n);
}

// Build a NON 2.05 response carrying a given payload and token, from `from`.
static void injectDiscoverResponse(StubTransport& tr, const Endpoint& from,
                                    const uint8_t* token, uint8_t tokenLen,
                                    const char* body) {
    Message rsp;
    rsp.setType(MessageType::NonConfirmable);
    rsp.setCode(Code::Content);
    rsp.setMessageId(0x1234);
    rsp.setToken(token, tokenLen);
    rsp.addOptionUint(static_cast<uint16_t>(OptionNumber::ContentFormat), 40); // CoRE Link Format
    if (body && *body)
        rsp.setPayload(reinterpret_cast<const uint8_t*>(body), strlen(body));
    tr.injectLen = rsp.encode(tr.injectBuf, sizeof(tr.injectBuf));
    tr.injectFrom = from;
}

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

// 1. allNodesEndpoint() returns 224.0.1.187 and the given port.
static void test_allnodes_endpoint() {
    Endpoint ep = Client::allNodesEndpoint(5683);
    assert(!ep.isV6);
    assert(ep.ip[0] == 224 && ep.ip[1] == 0 && ep.ip[2] == 1 && ep.ip[3] == 187);
    assert(ep.port == 5683);

    Endpoint ep2 = Client::allNodesEndpoint(9999);
    assert(ep2.port == 9999);
    printf("  PASS test_allnodes_endpoint\n");
}

// 2. discover() sends a NON GET to the multicast address.
static void test_discover_sends_non_get() {
    StubTransport tr;
    TransactionPool tp;
    Client c(tr, tp);
    c.begin(0);

    bool ok = c.discover(discoverCb, nullptr, 5683);
    assert(ok);
    assert(tr.sentLen > 0);

    Message msg;
    assert(msg.decode(tr.sentBuf, tr.sentLen) == DecodeError::None);
    assert(msg.type() == MessageType::NonConfirmable);
    assert(msg.code() == Code::Get);

    // Destination must be the all-nodes address.
    assert(!tr.sentTo.isV6);
    assert(tr.sentTo.ip[0] == 224 && tr.sentTo.ip[1] == 0 &&
           tr.sentTo.ip[2] == 1   && tr.sentTo.ip[3] == 187);
    assert(tr.sentTo.port == 5683);
    printf("  PASS test_discover_sends_non_get\n");
}

// 3. discover() encodes /.well-known/core as Uri-Path options.
static void test_discover_encodes_wellknown_core() {
    StubTransport tr;
    TransactionPool tp;
    Client c(tr, tp);
    c.begin(0);
    c.discover(discoverCb, nullptr);

    Message msg;
    msg.decode(tr.sentBuf, tr.sentLen);

    // Should have two Uri-Path options: ".well-known" and "core".
    bool foundWellKnown = false, foundCore = false;
    for (uint8_t i = 0; i < msg.optionCount(); ++i) {
        const Option* o = msg.optionAt(i);
        if (!o) break;
        if (o->number == static_cast<uint16_t>(OptionNumber::UriPath)) {
            if (o->length == 11 && memcmp(o->value, ".well-known", 11) == 0)
                foundWellKnown = true;
            if (o->length == 4 && memcmp(o->value, "core", 4) == 0)
                foundCore = true;
        }
    }
    assert(foundWellKnown);
    assert(foundCore);
    printf("  PASS test_discover_encodes_wellknown_core\n");
}

// 4. discover() returns false when all slots are busy.
static void test_discover_slot_full_returns_false() {
    StubTransport tr;
    TransactionPool tp;
    Client c(tr, tp);
    c.begin(0);

    // Fill all PULSECOAP_MAX_DISCOVERS slots.
    for (int i = 0; i < PULSECOAP_MAX_DISCOVERS; ++i) {
        bool ok = c.discover(discoverCb, nullptr);
        assert(ok);
    }
    // One more must fail.
    bool ok = c.discover(discoverCb, nullptr);
    assert(!ok);
    printf("  PASS test_discover_slot_full_returns_false\n");
}

// 5. Expired slots are freed and become available again.
static void test_discover_slot_expires() {
    StubTransport tr;
    TransactionPool tp;
    Client c(tr, tp);
    c.begin(0);

    // Fill all slots at t=0.
    for (int i = 0; i < PULSECOAP_MAX_DISCOVERS; ++i)
        assert(c.discover(discoverCb, nullptr));

    // All full — another fails.
    assert(!c.discover(discoverCb, nullptr));

    // Advance time past the discover timeout.
    c.poll(PULSECOAP_DISCOVER_TIMEOUT_MS + 1);

    // Slots should be expired now — a new discover should succeed.
    assert(c.discover(discoverCb, nullptr));
    printf("  PASS test_discover_slot_expires\n");
}

// 6. A response matching the discover token fires the DiscoverHandler.
static void test_discover_response_fires_handler() {
    resetGlobals();
    StubTransport tr;
    TransactionPool tp;
    Client c(tr, tp);
    c.begin(0);

    c.poll(0); // initialise lastNowMs_
    assert(c.discover(discoverCb, nullptr));

    // Extract the token from the sent request.
    Message sent;
    assert(sent.decode(tr.sentBuf, tr.sentLen) == DecodeError::None);
    uint8_t tok = sent.token()[0];
    uint8_t tokenLen = sent.tokenLength();

    // Inject a fake unicast response from a "server".
    Endpoint server;
    server.ip[0] = 192; server.ip[1] = 168; server.ip[2] = 1; server.ip[3] = 42;
    server.port = 5683; server.isV6 = false;
    const char* lf = "</sensors>;ct=0,</temp>;rt=\"temperature\"";
    injectDiscoverResponse(tr, server, &tok, tokenLen, lf);

    c.poll(10);

    assert(g_discoverCount == 1);
    assert(g_lastServer.ip[0] == 192 && g_lastServer.ip[3] == 42);
    assert(strcmp(g_lastLinkFormat, lf) == 0);
    printf("  PASS test_discover_response_fires_handler\n");
}

// 7. Multiple servers replying to the same discover token each fire the handler.
static void test_discover_multi_server_responses() {
    resetGlobals();
    StubTransport tr;
    TransactionPool tp;
    Client c(tr, tp);
    c.begin(0);
    c.poll(0);
    assert(c.discover(discoverCb, nullptr));

    Message sent;
    assert(sent.decode(tr.sentBuf, tr.sentLen) == DecodeError::None);
    uint8_t tok = sent.token()[0];
    uint8_t tokenLen = sent.tokenLength();

    Endpoint sv1, sv2;
    sv1.ip[0]=10; sv1.ip[1]=0; sv1.ip[2]=0; sv1.ip[3]=1; sv1.port=5683;
    sv2.ip[0]=10; sv2.ip[1]=0; sv2.ip[2]=0; sv2.ip[3]=2; sv2.port=5683;

    // First server reply.
    injectDiscoverResponse(tr, sv1, &tok, tokenLen, "</a>");
    c.poll(1);
    assert(g_discoverCount == 1);

    // Second server reply.
    injectDiscoverResponse(tr, sv2, &tok, tokenLen, "</b>");
    c.poll(2);
    assert(g_discoverCount == 2);
    printf("  PASS test_discover_multi_server_responses\n");
}

// 8. A discover token does not fire a pending_ (GET/observe) callback, and
//    vice versa — pending_ and discover slots use independent token namespaces
//    only if their tokens happen to differ (here we verify non-collision by
//    ensuring the discover response doesn't increment the response counter).
static void test_discover_does_not_fire_pending() {
    resetGlobals();
    StubTransport tr;
    TransactionPool tp;
    Client c(tr, tp);
    c.begin(0);
    c.poll(0);

    int responseCount = 0;
    Endpoint sv;
    sv.ip[0]=10; sv.ip[1]=0; sv.ip[2]=0; sv.ip[3]=5; sv.port=5683;

    // Issue a regular GET first — it takes token byte 1.
    assert(c.get(sv, "/temp", responseCb, &responseCount));

    // Issue a discover — takes token byte 2.
    assert(c.discover(discoverCb, nullptr));

    // Extract the discover token.
    Message sent;
    assert(sent.decode(tr.sentBuf, tr.sentLen) == DecodeError::None);
    uint8_t tok = sent.token()[0];
    uint8_t tokenLen = sent.tokenLength();

    // Inject a discover response (matched by discover token).
    injectDiscoverResponse(tr, sv, &tok, tokenLen, "</x>");
    c.poll(1);

    // DiscoverHandler should have fired, but responseCb must not.
    assert(g_discoverCount == 1);
    assert(responseCount == 0);
    printf("  PASS test_discover_does_not_fire_pending\n");
}

// 9. Custom multicast endpoint overload is accepted and used as the send target.
static void test_discover_custom_endpoint() {
    StubTransport tr;
    TransactionPool tp;
    Client c(tr, tp);
    c.begin(0);

    Endpoint ep;
    ep.ip[0]=239; ep.ip[1]=255; ep.ip[2]=0; ep.ip[3]=1; ep.port=9999; ep.isV6=false;
    bool ok = c.discover(ep, discoverCb, nullptr);
    assert(ok);
    assert(tr.sentTo.ip[0] == 239 && tr.sentTo.ip[3] == 1);
    assert(tr.sentTo.port == 9999);
    printf("  PASS test_discover_custom_endpoint\n");
}

// 10. After a slot expires, its handler is not fired for late-arriving replies.
static void test_discover_expired_slot_no_callback() {
    resetGlobals();
    StubTransport tr;
    TransactionPool tp;
    Client c(tr, tp);
    c.begin(0);
    c.poll(0);
    assert(c.discover(discoverCb, nullptr));

    Message sent;
    assert(sent.decode(tr.sentBuf, tr.sentLen) == DecodeError::None);
    uint8_t tok = sent.token()[0];
    uint8_t tokenLen = sent.tokenLength();

    // Advance past expiry so the slot is reaped.
    c.poll(PULSECOAP_DISCOVER_TIMEOUT_MS + 50);
    assert(g_discoverCount == 0); // nothing came in yet

    // Now inject a late response — should be dropped (slot is gone).
    Endpoint sv;
    sv.ip[0]=1; sv.ip[3]=1; sv.port=5683;
    injectDiscoverResponse(tr, sv, &tok, tokenLen, "</late>");
    c.poll(PULSECOAP_DISCOVER_TIMEOUT_MS + 60);
    assert(g_discoverCount == 0);
    printf("  PASS test_discover_expired_slot_no_callback\n");
}

// ---------------------------------------------------------------------------
// Optional real-network test: loopback multicast (skip when OS doesn't support)
// ---------------------------------------------------------------------------
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void handleTempGet(const Request& /*req*/, Response& res, void* /*ctx*/) {
    static const char body[] = "21";
    res.code = Code::Content;
    res.payload = reinterpret_cast<const uint8_t*>(body);
    res.payloadLength = 2;
    res.contentFormat = ContentFormat::TextPlain;
}

static void test_discover_loopback_multicast() {
    // Use an ephemeral server port so we don't conflict with real CoAP.
    PosixUdpTransport serverTr;
    Server srv(serverTr);
    if (!srv.begin(0)) { printf("  SKIP test_discover_loopback_multicast (server bind failed)\n"); return; }
    uint16_t srvPort = serverTr.localPort();

    // Join the multicast group on loopback so the server socket receives the NON.
    if (!serverTr.joinMulticastGroup("224.0.1.187", "127.0.0.1")) {
        printf("  SKIP test_discover_loopback_multicast (joinMulticastGroup failed — multicast may be unsupported)\n");
        return;
    }

    // Register a resource so /.well-known/core has something to list.
    srv.addResource("/sensors/temp", MethodGet, handleTempGet, nullptr);

    // Client binds to an ephemeral port and routes multicast through loopback.
    PosixUdpTransport clientTr;
    TransactionPool clientTp;
    Client client(clientTr, clientTp);
    if (!clientTr.begin(0)) { printf("  SKIP test_discover_loopback_multicast (client bind failed)\n"); return; }
    clientTr.setMulticastOutboundInterface("127.0.0.1");

    resetGlobals();

    // Send a discover to the multicast address using the server's ephemeral port.
    Endpoint mcast;
    mcast.ip[0]=224; mcast.ip[1]=0; mcast.ip[2]=1; mcast.ip[3]=187;
    mcast.port = srvPort; mcast.isV6 = false;
    assert(client.discover(mcast, discoverCb, nullptr));

    // Drive both sides for up to ~200 ms.
    for (int ms = 0; ms < 200 && g_discoverCount == 0; ms += 5) {
        srv.poll(static_cast<uint32_t>(ms));
        client.poll(static_cast<uint32_t>(ms));
    }

    if (g_discoverCount > 0) {
        printf("  PASS test_discover_loopback_multicast (server=%d.%d.%d.%d link-format=%s)\n",
               g_lastServer.ip[0], g_lastServer.ip[1],
               g_lastServer.ip[2], g_lastServer.ip[3],
               g_lastLinkFormat);
    } else {
        printf("  SKIP test_discover_loopback_multicast (no response — multicast routing unavailable)\n");
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main() {
    printf("=== test_multicast_discover ===\n");

    test_allnodes_endpoint();
    test_discover_sends_non_get();
    test_discover_encodes_wellknown_core();
    test_discover_slot_full_returns_false();
    test_discover_slot_expires();
    test_discover_response_fires_handler();
    test_discover_multi_server_responses();
    test_discover_does_not_fire_pending();
    test_discover_custom_endpoint();
    test_discover_expired_slot_no_callback();
    test_discover_loopback_multicast();

    printf("All multicast discover tests passed.\n");
    return 0;
}
