// End-to-end test: a real Server and a real Client talking over an
// in-process loopback Transport double (no sockets, no Arduino). Exercises
// GET, PUT, and Observe register+notify across the actual wire format.
//
// Build (see test/README.md for the one-liner that builds every suite):
//   g++ -std=c++11 -Wall -Wextra -Isrc test/test_client_server_integration.cpp src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp src/PulseCoAPServer.cpp src/PulseCoAPClient.cpp -o test_client_server_integration
//   ./test_client_server_integration
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/PulseCoAP.h"
#include "LoopbackTransport.h"

using namespace pulsecoap;

namespace {
int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    }
}
} // namespace

#define CHECK(cond) check((cond), #cond)

// --- deferred-response handler context --------------------------------

struct DeferCtx {
    DeferHandle handle = kInvalidDeferHandle;
    bool poolFull = false; // true when the server couldn't allocate a slot
};

// Handler that always tries to defer. If the deferred pool was full
// (req.deferHandle == kInvalidDeferHandle), falls back to an inline
// 5.03 Service Unavailable — the expected fallback pattern.
void handleDeferRequest(const Request& req, Response& res, void* ctx) {
    auto* c = static_cast<DeferCtx*>(ctx);
    if (req.deferHandle != kInvalidDeferHandle) {
        c->handle = req.deferHandle;
        res.deferred = true;
    } else {
        c->poolFull = true;
        res.code = Code::ServiceUnavailable;
        res.setPayload("busy");
    }
}

// --- server-side resource handlers ------------------------------------

void handleGetTemp(const Request& /*req*/, Response& res, void* /*ctx*/) {
    res.code = Code::Content;
    res.contentFormat = ContentFormat::TextPlain;
    res.setPayload("21.5");
}

int g_putCallCount = 0;
std::string g_lastPutPayload;

void handlePutTemp(const Request& req, Response& res, void* /*ctx*/) {
    g_putCallCount++;
    g_lastPutPayload.assign(reinterpret_cast<const char*>(req.message->payload()), req.message->payloadLength());
    res.code = Code::Changed;
}

void handleGetObservable(const Request& /*req*/, Response& res, void* /*ctx*/) {
    res.code = Code::Content;
    res.setPayload("initial");
}

// --- client-side response captures -------------------------------------

struct CapturedResponse {
    std::string payload;
    Code code = Code::Empty;
    ContentFormat contentFormat = ContentFormat::TextPlain;
    int callCount = 0;
};

void onCapture(const ClientResponse& res, void* userContext) {
    auto* captured = static_cast<CapturedResponse*>(userContext);
    captured->payload.assign(reinterpret_cast<const char*>(res.payload), res.payloadLength);
    captured->code = res.code;
    captured->contentFormat = res.contentFormat;
    captured->callCount++;
}

static void test_get_request_response() {
    std::printf("test_get_request_response\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));
    CHECK(server.addResource("/sensors/temp", MethodGet, handleGetTemp));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    CHECK(client.get(serverEp, "/sensors/temp", onCapture, &captured));

    uint32_t now = 0;
    server.poll(now);   // server receives GET, sends piggybacked ACK
    client.poll(now);   // client receives ACK, fires onCapture

    CHECK(captured.callCount == 1);
    CHECK(captured.code == Code::Content);
    CHECK(captured.payload == "21.5");
}

static void test_put_request_delivers_payload() {
    std::printf("test_put_request_delivers_payload\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));
    CHECK(server.addResource("/sensors/temp", MethodPut, handlePutTemp));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    const char* payload = "23.1";
    g_putCallCount = 0;
    CHECK(client.put(serverEp, "/sensors/temp", reinterpret_cast<const uint8_t*>(payload),
                      std::strlen(payload), ContentFormat::TextPlain, onCapture, &captured));

    uint32_t now = 0;
    server.poll(now);
    client.poll(now);

    CHECK(g_putCallCount == 1);
    CHECK(g_lastPutPayload == "23.1");
    CHECK(captured.code == Code::Changed);
}

static void test_observe_register_and_notify() {
    std::printf("test_observe_register_and_notify\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));
    CHECK(server.addResource("/sensors/temp", MethodGet, handleGetObservable,
                              nullptr, /*observable=*/true));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    CHECK(client.observe(serverEp, "/sensors/temp", onCapture, &captured));

    uint32_t now = 0;
    server.poll(now);   // server registers the observer, sends initial value
    client.poll(now);

    CHECK(captured.callCount == 1);
    CHECK(captured.payload == "initial");
    CHECK(server.observerCount() == 1);

    // Server-initiated push: no new request from the client involved.
    const char* updated = "22.0";
    CHECK(server.notify("/sensors/temp", reinterpret_cast<const uint8_t*>(updated), std::strlen(updated)));
    client.poll(now); // client receives the pushed notification

    CHECK(captured.callCount == 2);
    CHECK(captured.payload == "22.0");
}

static void test_well_known_core_basic() {
    std::printf("test_well_known_core_basic\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));
    CHECK(server.addResource("/sensors/temp", MethodGet, handleGetTemp));
    CHECK(server.addResource("/sensors/hum", MethodGet | MethodPut, handleGetTemp));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    CHECK(client.get(serverEp, "/.well-known/core", onCapture, &captured));

    uint32_t now = 0;
    server.poll(now);
    client.poll(now);

    CHECK(captured.callCount == 1);
    CHECK(captured.code == Code::Content);
    // Content-Format must be 40 (application/link-format)
    CHECK(captured.contentFormat == ContentFormat::LinkFormat);
    // Both registered paths must appear; /.well-known/core itself must not.
    std::string body(captured.payload);
    CHECK(body.find("</sensors/temp>") != std::string::npos);
    CHECK(body.find("</sensors/hum>") != std::string::npos);
    CHECK(body.find("/.well-known/core") == std::string::npos);
}

static void test_well_known_core_obs_attribute() {
    std::printf("test_well_known_core_obs_attribute\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));
    CHECK(server.addResource("/sensors/temp", MethodGet, handleGetTemp,
                              nullptr, /*observable=*/true));
    CHECK(server.addResource("/sensors/hum", MethodGet, handleGetTemp,
                              nullptr, /*observable=*/false));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    CHECK(client.get(serverEp, "/.well-known/core", onCapture, &captured));

    uint32_t now = 0;
    server.poll(now);
    client.poll(now);

    std::string body(captured.payload);
    // /sensors/temp is observable — must carry the ;obs attribute
    size_t tempPos = body.find("</sensors/temp>");
    CHECK(tempPos != std::string::npos);
    // Find the next entry boundary (comma) or end of string after tempPos
    size_t nextComma = body.find(',', tempPos);
    std::string tempEntry = (nextComma != std::string::npos)
                                ? body.substr(tempPos, nextComma - tempPos)
                                : body.substr(tempPos);
    CHECK(tempEntry.find(";obs") != std::string::npos);

    // /sensors/hum is NOT observable — must NOT carry ;obs
    size_t humPos = body.find("</sensors/hum>");
    CHECK(humPos != std::string::npos);
    size_t nextCommaHum = body.find(',', humPos);
    std::string humEntry = (nextCommaHum != std::string::npos)
                               ? body.substr(humPos, nextCommaHum - humPos)
                               : body.substr(humPos);
    CHECK(humEntry.find(";obs") == std::string::npos);
}

static void test_well_known_core_rt_attribute() {
    std::printf("test_well_known_core_rt_attribute\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));
    CHECK(server.addResource("/sensors/temp", MethodGet, handleGetTemp));
    CHECK(server.setResourceType("/sensors/temp", "temperature-c"));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    CHECK(client.get(serverEp, "/.well-known/core", onCapture, &captured));

    uint32_t now = 0;
    server.poll(now);
    client.poll(now);

    std::string body(captured.payload);
    CHECK(body.find("rt=\"temperature-c\"") != std::string::npos);
    // setResourceType on a non-existent path should return false
    CHECK(!server.setResourceType("/does/not/exist", "foo"));
}

// Helper: build a raw RST frame for a given message ID and inject it into
// a transport's inbox. `from` must match the remote address the receiver
// tracks for the relevant transaction/observer — both sides key their state
// on the (remote address, message-ID) pair.
static void injectRst(LoopbackTransport& into, uint16_t messageId, const Endpoint& from) {
    Message rst;
    rst.setType(MessageType::Reset);
    rst.setCode(Code::Empty);
    rst.setMessageId(messageId);
    // RST carries no token (RFC 7252 §3: TKL=0 for empty RST/ACK)
    uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
    size_t len = rst.encode(buf, sizeof(buf));
    into.injectPacket(buf, len, from);
}

static void test_rst_removes_observer() {
    std::printf("test_rst_removes_observer\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));
    CHECK(server.addResource("/sensors/temp", MethodGet, handleGetObservable,
                              nullptr, /*observable=*/true));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    CHECK(client.observe(serverEp, "/sensors/temp", onCapture, &captured));

    uint32_t now = 0;
    server.poll(now); // registers observer, sends initial value (message ID 2 — ID 1 is .well-known/core's ACK)
    client.poll(now); // client receives initial value

    CHECK(captured.callCount == 1);
    CHECK(server.observerCount() == 1);

    // Server sends a notification and captures the message ID it used.
    // We'll need it to construct the RST. Since the server sends NON with
    // nextMessageId_ auto-incremented, send notify and read its message ID
    // from the client's inbox before poll().
    const char* updated = "99.0";
    CHECK(server.notify("/sensors/temp",
                         reinterpret_cast<const uint8_t*>(updated), std::strlen(updated)));
    // The notification is now sitting in clientTransport's inbox.
    // Peek at the raw bytes to extract the message ID (bytes 2-3, big-endian).
    uint8_t peekBuf[PULSECOAP_MAX_MSG_SIZE];
    Endpoint peekFrom;
    size_t peekLen = clientTransport.peek(peekBuf, sizeof(peekBuf), peekFrom);
    CHECK(peekLen >= 4);
    uint16_t notifyMsgId = static_cast<uint16_t>((peekBuf[2] << 8) | peekBuf[3]);

    // Client receives the notification normally.
    client.poll(now);
    CHECK(captured.callCount == 2);

    // Observer sends RST for that notification back to the server.
    // The RST must appear to come from the client's address as the server
    // knows it (127.0.0.1 : clientTransport local port = 0).
    Endpoint clientEp;
    clientEp.ip[0] = 127; clientEp.ip[1] = 0; clientEp.ip[2] = 0; clientEp.ip[3] = 1;
    clientEp.port = 0; // clientTransport was begin(0)
    injectRst(serverTransport, notifyMsgId, clientEp);
    server.poll(now); // server processes RST, removes observer
    CHECK(server.observerCount() == 0);

    // Subsequent notify() should be a no-op for that observer.
    CHECK(server.notify("/sensors/temp",
                         reinterpret_cast<const uint8_t*>(updated), std::strlen(updated)));
    client.poll(now);
    CHECK(captured.callCount == 2); // no additional fire
}

static void test_rst_fires_client_timeout_handler() {
    std::printf("test_rst_fires_client_timeout_handler\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    // No server — the transport just discards anything sent to it; we inject RST manually.
    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    int timeoutFired = 0;
    client.setTimeoutHandler([](void* ctx) { (*static_cast<int*>(ctx))++; });
    // TimeoutHandler doesn't carry userContext — wire it in via the lambda capture.
    // Use a static to bridge: set it before the call, read after.
    static int* g_timeoutCounter = &timeoutFired;
    client.setTimeoutHandler([](void* /*ctx*/) { (*g_timeoutCounter)++; });

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    CHECK(client.get(serverEp, "/sensors/temp", onCapture, &captured));
    // The GET was sent with message ID 1 (nextMessageId_ starts at 1).
    uint16_t sentMsgId = 1;

    CHECK(clientTxns.activeCount() == 1); // one CON in flight

    // Simulate the server sending RST back. The RST must appear to come
    // from serverEp so it matches the transaction's remote address.
    injectRst(clientTransport, sentMsgId, serverEp);
    uint32_t now = 0;
    client.poll(now); // should process RST, complete transaction, fire TimeoutHandler

    CHECK(clientTxns.activeCount() == 0); // transaction freed
    CHECK(timeoutFired == 1);             // TimeoutHandler fired once, not after retransmits
    CHECK(captured.callCount == 0);       // onResponse was NOT called
}

// ---------------------------------------------------------------------------
// CON Observe notification tests — RFC 7641 §4.5
// ---------------------------------------------------------------------------

// CON notify: client receives the notification, ACKs it, and the server's
// txPool_ entry is retired — no retransmissions after the ACK.
static void test_con_notify_acked_by_client() {
    std::printf("test_con_notify_acked_by_client\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));
    CHECK(server.addResource("/sensors/temp", MethodGet, handleGetObservable,
                              nullptr, /*observable=*/true));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    CHECK(client.observe(serverEp, "/sensors/temp", onCapture, &captured));

    uint32_t now = 0;
    server.poll(now);   // registers observer, sends piggybacked ACK + initial value
    client.poll(now);   // receives ACK+initial, fires onCapture (callCount=1)
    CHECK(captured.callCount == 1);
    CHECK(server.observerCount() == 1);

    // Push a CON notification — should be retransmitted if not ACK'd.
    const char* updated = "99.9";
    CHECK(server.notify("/sensors/temp",
                         reinterpret_cast<const uint8_t*>(updated), std::strlen(updated),
                         ContentFormat::TextPlain, /*confirmable=*/true));

    // Client receives CON notification, automatically ACKs it, fires callback.
    client.poll(now);
    CHECK(captured.callCount == 2);
    CHECK(captured.payload == "99.9");

    // Server receives the client's ACK and retires the txPool_ entry.
    server.poll(now);

    // Advance well past MAX_RETRANSMIT timeout: the observer must still be
    // registered (it ACK'd, so it was not evicted), and NO duplicate must fire.
    uint32_t farFuture = 60000;
    server.poll(farFuture);
    client.poll(farFuture);
    CHECK(captured.callCount == 2);       // no duplicate from retransmission
    CHECK(server.observerCount() == 1);   // observer still alive
}

// CON notify timeout: if the peer never ACKs, the server evicts the observer
// after MAX_RETRANSMIT retries — RFC 7641 §4.5 dead-peer detection.
static void test_con_notify_timeout_removes_observer() {
    std::printf("test_con_notify_timeout_removes_observer\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));
    CHECK(server.addResource("/sensors/temp", MethodGet, handleGetObservable,
                              nullptr, /*observable=*/true));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    CHECK(client.observe(serverEp, "/sensors/temp", onCapture, &captured));

    uint32_t now = 0;
    server.poll(now);   // registers observer, sends piggybacked ACK + initial value
    client.poll(now);   // client receives initial value (callCount=1); goes silent after this
    CHECK(captured.callCount == 1);
    CHECK(server.observerCount() == 1);

    // Push a CON notification to the now-silent peer.
    const char* updated = "dead";
    CHECK(server.notify("/sensors/temp",
                         reinterpret_cast<const uint8_t*>(updated), std::strlen(updated),
                         ContentFormat::TextPlain, /*confirmable=*/true));

    // Drive the server through retransmissions. txPool_ uses
    // computeInitialTimeoutMs(seed=notificationMsgId=1) — a deterministic
    // xorshift32 giving an initial timeout of ~2369 ms. With MAX_RETRANSMIT=4
    // and exponential doubling, the timeout fires around t=73 000 ms.
    // Poll in 1 s steps to 80 s to guarantee we cross that point.
    for (uint32_t t = 1000; t <= 80000; t += 1000) {
        server.poll(t);
        if (server.observerCount() == 0) break; // evicted early — done
    }

    CHECK(server.observerCount() == 0);

    // A subsequent notify() is a no-op (nobody registered any more).
    CHECK(server.notify("/sensors/temp",
                         reinterpret_cast<const uint8_t*>(updated), std::strlen(updated)));
    // The client's callback count must remain at 1 — only the initial value
    // was delivered; the CON notification and its retransmissions are sitting
    // unconsumed in the inbox because the client went silent.
    CHECK(captured.callCount == 1);
}

// ---------------------------------------------------------------------------
// Separate (non-piggybacked) response tests — RFC 7252 §5.2.2
// ---------------------------------------------------------------------------

// CON request → server defers (sends empty ACK) → server calls respond() →
// separate CON response → client ACKs it → onCapture fires exactly once.
static void test_deferred_con_response() {
    std::printf("test_deferred_con_response\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));

    DeferCtx deferCtx;
    CHECK(server.addResource("/slow", MethodGet, handleDeferRequest, &deferCtx));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    // CON GET — starts a transaction that should complete on the empty ACK.
    CHECK(client.get(serverEp, "/slow", onCapture, &captured, /*confirmable=*/true));
    CHECK(clientTxns.activeCount() == 1);

    uint32_t now = 0;
    // Server receives CON GET, handler defers, server sends empty ACK.
    server.poll(now);
    CHECK(deferCtx.handle != kInvalidDeferHandle);

    // Client receives the empty ACK — CON transaction stops, no callback yet.
    client.poll(now);
    CHECK(clientTxns.activeCount() == 0);  // transaction completed by empty ACK
    CHECK(captured.callCount == 0);         // response not delivered yet

    // Server produces the answer and sends a separate CON response.
    const char* answer = "42.0";
    CHECK(server.respond(deferCtx.handle, Code::Content,
                          reinterpret_cast<const uint8_t*>(answer), std::strlen(answer)));

    // Client receives separate CON, ACKs it, fires onCapture.
    client.poll(now);
    CHECK(captured.callCount == 1);
    CHECK(captured.code == Code::Content);
    CHECK(captured.payload == "42.0");

    // Server receives the client's ACK and retires the txPool_ entry.
    server.poll(now);
    // If we advance time well past MAX_RETRANSMIT and poll again, no
    // retransmission should occur (the entry was completed by the ACK).
    uint32_t farFuture = 60000;
    server.poll(farFuture);
    // The client should NOT receive a duplicate — the server sent nothing.
    client.poll(farFuture);
    CHECK(captured.callCount == 1);  // still exactly one, no duplicate
}

// NON request → server defers (no empty ACK needed) → server calls respond() →
// separate NON response → onCapture fires without any ACK exchange.
static void test_deferred_non_response() {
    std::printf("test_deferred_non_response\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));

    DeferCtx deferCtx;
    CHECK(server.addResource("/slow", MethodGet, handleDeferRequest, &deferCtx));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    CapturedResponse captured;
    // NON GET — no transaction registered, nothing to stop.
    CHECK(client.get(serverEp, "/slow", onCapture, &captured, /*confirmable=*/false));
    CHECK(clientTxns.activeCount() == 0);

    uint32_t now = 0;
    // Server receives NON GET, handler defers (no empty ACK is sent for NON).
    server.poll(now);
    CHECK(deferCtx.handle != kInvalidDeferHandle);

    // Client transport is empty — no ACK was sent.
    client.poll(now);
    CHECK(captured.callCount == 0);

    // Server answers asynchronously — sends separate NON response.
    const char* answer = "7.3";
    CHECK(server.respond(deferCtx.handle, Code::Content,
                          reinterpret_cast<const uint8_t*>(answer), std::strlen(answer)));

    // Client receives the NON response and fires the callback.
    client.poll(now);
    CHECK(captured.callCount == 1);
    CHECK(captured.code == Code::Content);
    CHECK(captured.payload == "7.3");
}

// respond() must return false for handles that are invalid or already consumed.
static void test_respond_invalid_handle() {
    std::printf("test_respond_invalid_handle\n");

    LoopbackTransport serverTransport, clientTransport;
    serverTransport.connectPeer(&clientTransport);
    clientTransport.connectPeer(&serverTransport);

    Server server(serverTransport);
    CHECK(server.begin(5683));

    DeferCtx deferCtx;
    CHECK(server.addResource("/slow", MethodGet, handleDeferRequest, &deferCtx));

    TransactionPool clientTxns;
    Client client(clientTransport, clientTxns);
    CHECK(client.begin(0));

    Endpoint serverEp;
    serverEp.ip[0] = 127; serverEp.ip[1] = 0; serverEp.ip[2] = 0; serverEp.ip[3] = 1;
    serverEp.port = 5683;

    // kInvalidDeferHandle is always rejected.
    CHECK(!server.respond(kInvalidDeferHandle, Code::Content, "x"));

    // An out-of-range handle (>= PULSECOAP_MAX_DEFERRED) is rejected.
    CHECK(!server.respond(PULSECOAP_MAX_DEFERRED, Code::Content, "x"));

    // A valid handle is consumed exactly once; a second call with the same
    // handle must return false (slot already freed by the first call).
    CapturedResponse captured;
    CHECK(client.get(serverEp, "/slow", onCapture, &captured));
    uint32_t now = 0;
    server.poll(now); // handler defers
    client.poll(now); // empty ACK consumed

    CHECK(deferCtx.handle != kInvalidDeferHandle);
    DeferHandle h = deferCtx.handle;

    // First respond() — succeeds.
    CHECK(server.respond(h, Code::Content, "ok"));
    // Second respond() with the same handle — slot is inactive, must fail.
    CHECK(!server.respond(h, Code::Content, "dup"));

    // Receive and verify the (single) response.
    client.poll(now);
    CHECK(captured.callCount == 1);
    CHECK(captured.payload == "ok");
}

int main() {
    test_get_request_response();
    test_put_request_delivers_payload();
    test_observe_register_and_notify();
    test_well_known_core_basic();
    test_well_known_core_obs_attribute();
    test_well_known_core_rt_attribute();
    test_rst_removes_observer();
    test_rst_fires_client_timeout_handler();
    test_con_notify_acked_by_client();
    test_con_notify_timeout_removes_observer();
    test_deferred_con_response();
    test_deferred_non_response();
    test_respond_invalid_handle();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
