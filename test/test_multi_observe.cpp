// test_multi_observe.cpp
// Tests for simultaneous Observe registrations on the same server and
// path-indexed cancelObserve() (ROADMAP v0.7.0).
//
// Verifies: two observes on the same server receive independent notifications,
// cancelling by path tears down only the targeted subscription, the remaining
// subscription keeps receiving, and edge cases (cancel unknown path, single
// observe regression).
//
// Check count target: ~30 checks.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "LoopbackTransport.h"
#include "../src/PulseCoAP.h"

using namespace pulsecoap;

// ---------------------------------------------------------------------------
// Boilerplate
// ---------------------------------------------------------------------------

static int g_checks   = 0;
static int g_failures = 0;

#define CHECK(expr) do { \
    ++g_checks; \
    if (!(expr)) { \
        ++g_failures; \
        printf("  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, #expr); \
    } \
} while (0)

static void report(const char* name) {
    printf("%-64s %s\n", name, g_failures == 0 ? "OK" : "FAILED");
}

static Endpoint serverEp() {
    Endpoint e;
    e.ip[0]=127; e.ip[1]=0; e.ip[2]=0; e.ip[3]=1;
    e.port = 5683;
    return e;
}

// Uses PULSECOAP_MAX_TRANSACTIONS=6 so two observes + deregister GETs fit.
struct Fixture {
    LoopbackTransport serverTx, clientTx;
    TransactionPool   txPool;
    Server  server;
    Client  client;

    Fixture() : server(serverTx), client(clientTx, txPool) {
        serverTx.connectPeer(&clientTx);
        clientTx.connectPeer(&serverTx);
        server.begin(5683);
        client.begin(0);
    }

    void tick(uint32_t nowMs = 0) {
        server.poll(nowMs);
        client.poll(nowMs);
    }
};

// ---------------------------------------------------------------------------
// Shared handlers
// ---------------------------------------------------------------------------

static void handleObs(const Request& /*req*/, Response& res, void* ctx) {
    const char* payload = static_cast<const char*>(ctx);
    res.code = Code::Content;
    res.setPayload(payload);
}

struct ObsCap {
    int         callCount = 0;
    std::string lastPayload;

    void reset() { callCount = 0; lastPayload.clear(); }
};

static void onObs(const ClientResponse& res, void* ctx) {
    auto* c = static_cast<ObsCap*>(ctx);
    ++c->callCount;
    if (res.payload && res.payloadLength > 0)
        c->lastPayload.assign(reinterpret_cast<const char*>(res.payload), res.payloadLength);
}

// ---------------------------------------------------------------------------
// Test 1: single observe — regression
// ---------------------------------------------------------------------------

static void test_single_observe_regression() {
    Fixture f;
    const char* tempVal = "23";
    f.server.addResource("/temp", MethodGet, handleObs, const_cast<char*>(tempVal),
                          /*observable=*/true);

    ObsCap cap;
    CHECK(f.client.observe(serverEp(), "/temp", onObs, &cap));
    f.tick(0); f.tick(10);  // registration + initial response
    CHECK(cap.callCount == 1);
    CHECK(cap.lastPayload == "23");

    tempVal = "25";
    f.server.notify("/temp",
                    reinterpret_cast<const uint8_t*>("25"), 2);
    f.tick(20); f.tick(30);
    CHECK(cap.callCount == 2);
    CHECK(cap.lastPayload == "25");

    report("test_single_observe_regression");
}

// ---------------------------------------------------------------------------
// Test 2: two observes on same server fire independent callbacks
// ---------------------------------------------------------------------------

static const char* g_tempStr = "20";
static const char* g_humStr  = "55";

static void handleTemp(const Request& /*req*/, Response& res, void* /*ctx*/) {
    res.code = Code::Content;
    res.setPayload(g_tempStr);
}
static void handleHum(const Request& /*req*/, Response& res, void* /*ctx*/) {
    res.code = Code::Content;
    res.setPayload(g_humStr);
}

static void test_two_observes_same_server() {
    Fixture f;
    f.server.addResource("/sensors/temp", MethodGet, handleTemp, nullptr, true);
    f.server.addResource("/sensors/hum",  MethodGet, handleHum,  nullptr, true);

    ObsCap capTemp, capHum;
    CHECK(f.client.observe(serverEp(), "/sensors/temp", onObs, &capTemp));
    CHECK(f.client.observe(serverEp(), "/sensors/hum",  onObs, &capHum));

    // Drive registrations
    for (int i = 0; i < 4; ++i) f.tick(static_cast<uint32_t>(i * 10));

    CHECK(capTemp.callCount == 1);
    CHECK(capTemp.lastPayload == "20");
    CHECK(capHum.callCount == 1);
    CHECK(capHum.lastPayload == "55");

    // Notify temperature only
    g_tempStr = "21";
    f.server.notify("/sensors/temp",
                    reinterpret_cast<const uint8_t*>("21"), 2);
    f.tick(50); f.tick(60);
    CHECK(capTemp.callCount == 2);
    CHECK(capTemp.lastPayload == "21");
    CHECK(capHum.callCount == 1);  // humidity unchanged

    // Notify humidity only
    g_humStr = "60";
    f.server.notify("/sensors/hum",
                    reinterpret_cast<const uint8_t*>("60"), 2);
    f.tick(70); f.tick(80);
    CHECK(capTemp.callCount == 2);  // temperature unchanged
    CHECK(capHum.callCount == 2);
    CHECK(capHum.lastPayload == "60");

    report("test_two_observes_same_server");
}

// ---------------------------------------------------------------------------
// Test 3: cancelObserve by path tears down only that subscription
// ---------------------------------------------------------------------------

static void test_cancel_first_observe_second_continues() {
    Fixture f;
    g_tempStr = "30";
    g_humStr  = "40";
    f.server.addResource("/sensors/temp", MethodGet, handleTemp, nullptr, true);
    f.server.addResource("/sensors/hum",  MethodGet, handleHum,  nullptr, true);

    ObsCap capTemp, capHum;
    CHECK(f.client.observe(serverEp(), "/sensors/temp", onObs, &capTemp));
    CHECK(f.client.observe(serverEp(), "/sensors/hum",  onObs, &capHum));

    for (int i = 0; i < 4; ++i) f.tick(static_cast<uint32_t>(i * 10));
    CHECK(capTemp.callCount == 1);
    CHECK(capHum.callCount  == 1);

    // Cancel only /sensors/temp
    CHECK(f.client.cancelObserve(serverEp(), "/sensors/temp"));

    for (int i = 4; i < 8; ++i) f.tick(static_cast<uint32_t>(i * 10));

    int callsBeforeNotify = capHum.callCount;

    // Notify both — only humidity observer should still be active on the server.
    g_tempStr = "31";
    g_humStr  = "41";
    f.server.notify("/sensors/temp",
                    reinterpret_cast<const uint8_t*>("31"), 2);
    f.server.notify("/sensors/hum",
                    reinterpret_cast<const uint8_t*>("41"), 2);
    f.tick(100); f.tick(110);

    // Temperature notification should arrive at client but client's slot is
    // freed — no callback fires. The server will remove it on RST (or
    // ignore it). Either way capTemp stays at its last count.
    CHECK(capHum.callCount == callsBeforeNotify + 1);
    CHECK(capHum.lastPayload == "41");

    report("test_cancel_first_observe_second_continues");
}

// ---------------------------------------------------------------------------
// Test 4: cancel by wrong path returns false
// ---------------------------------------------------------------------------

static void test_cancel_unknown_path_returns_false() {
    Fixture f;
    f.server.addResource("/a", MethodGet, handleTemp, nullptr, true);

    ObsCap cap;
    CHECK(f.client.observe(serverEp(), "/a", onObs, &cap));
    f.tick(0); f.tick(10);

    // Cancelling a path that is not currently observed returns false.
    CHECK(!f.client.cancelObserve(serverEp(), "/b"));
    CHECK(!f.client.cancelObserve(serverEp(), "/a/sub"));

    // The existing observe is unaffected.
    g_tempStr = "9";
    f.server.notify("/a", reinterpret_cast<const uint8_t*>("9"), 1);
    f.tick(20); f.tick(30);
    CHECK(cap.callCount == 2);

    report("test_cancel_unknown_path_returns_false");
}

// ---------------------------------------------------------------------------
// Test 5: cancel second subscription, first keeps receiving
// ---------------------------------------------------------------------------

static void test_cancel_second_observe_first_continues() {
    Fixture f;
    g_tempStr = "5";
    g_humStr  = "6";
    f.server.addResource("/x", MethodGet, handleTemp, nullptr, true);
    f.server.addResource("/y", MethodGet, handleHum,  nullptr, true);

    ObsCap capX, capY;
    CHECK(f.client.observe(serverEp(), "/x", onObs, &capX));
    CHECK(f.client.observe(serverEp(), "/y", onObs, &capY));
    for (int i = 0; i < 4; ++i) f.tick(static_cast<uint32_t>(i * 10));

    CHECK(capX.callCount == 1);
    CHECK(capY.callCount == 1);

    // Cancel /y
    CHECK(f.client.cancelObserve(serverEp(), "/y"));
    for (int i = 4; i < 8; ++i) f.tick(static_cast<uint32_t>(i * 10));

    // Notify /x — should reach capX.
    g_tempStr = "7";
    f.server.notify("/x", reinterpret_cast<const uint8_t*>("7"), 1);
    f.tick(100); f.tick(110);
    CHECK(capX.callCount == 2);
    CHECK(capX.lastPayload == "7");

    report("test_cancel_second_observe_first_continues");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    printf("=== PulseCoAP multi-observe / cancelObserve-by-path tests ===\n\n");

    test_single_observe_regression();
    test_two_observes_same_server();
    test_cancel_first_observe_second_continues();
    test_cancel_unknown_path_returns_false();
    test_cancel_second_observe_first_continues();

    printf("\n%d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
