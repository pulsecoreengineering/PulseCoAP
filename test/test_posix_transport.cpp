// test_posix_transport.cpp
// Integration smoke-tests for PosixUdpTransport — exercises real OS loopback
// sockets on Linux/macOS.
//
// Tests:
//   1. Raw send/receive round-trip over 127.0.0.1
//   2. GET → 2.05 Content exchange via Server + Client
//   3. Observe registration + one notification over real sockets
//   4. Endpoint IPv6 struct semantics (no socket)
//   5. IPv6 raw loopback (::1) — skipped if kernel has no IPv6
//   6. GET over IPv6 (::1) with Server + Client — skipped if no IPv6
//
// Check count target: ~35 checks (plus skipped counts when no IPv6).

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>       // usleep
#include <sys/socket.h>   // AF_INET6 for runtime probe
#include <netinet/in.h>

#include "../src/PulseCoAP.h"
#include "../src/PulseCoAPTransportPosix.h"

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

// Run up to maxIters alternating poll() calls, 1 ms apart. Using real OS time
// here because packets travel through the kernel's loopback device.
static void spin(Server& srv, Client& cli, int maxIters = 30) {
    for (int i = 0; i < maxIters; ++i) {
        srv.poll(static_cast<uint32_t>(i));
        cli.poll(static_cast<uint32_t>(i));
        usleep(1000); // 1 ms
    }
}

static Endpoint loopback(uint16_t port) {
    Endpoint e;
    e.ip[0] = 127; e.ip[1] = 0; e.ip[2] = 0; e.ip[3] = 1;
    e.port = port;
    return e;
}

// ---------------------------------------------------------------------------
// Test 1: raw send / receive round-trip (no CoAP framing)
// ---------------------------------------------------------------------------

static void test_raw_loopback() {
    PosixUdpTransport tx, rx;
    CHECK(tx.begin(0));
    CHECK(rx.begin(0));

    uint16_t rxPort = rx.localPort();
    CHECK(rxPort != 0);
    CHECK(tx.localPort() != 0);
    CHECK(tx.localPort() != rxPort);

    // A minimal CoAP-like byte sequence — just checking the socket plumbing.
    const uint8_t kFrame[] = {0x40, 0x01, 0x12, 0x34}; // CON GET, MID 0x1234
    CHECK(tx.send(loopback(rxPort), kFrame, sizeof(kFrame)));

    usleep(10000); // 10 ms — give the kernel time to deliver the datagram

    uint8_t buf[64] = {};
    Endpoint from{};
    size_t n = rx.receive(buf, sizeof(buf), from);
    CHECK(n == sizeof(kFrame));
    CHECK(memcmp(buf, kFrame, sizeof(kFrame)) == 0);
    CHECK(from.ip[0] == 127 && from.ip[1] == 0 &&
          from.ip[2] == 0   && from.ip[3] == 1);
    CHECK(from.port == tx.localPort());

    // Second call returns 0 — ring is now empty.
    Endpoint dummy{};
    CHECK(rx.receive(buf, sizeof(buf), dummy) == 0);

    report("test_raw_loopback");
}

// ---------------------------------------------------------------------------
// Test 2: GET → 2.05 Content exchange via Server + Client
// ---------------------------------------------------------------------------

struct GetCap {
    int         called  = 0;
    Code        code    = Code::Empty;
    std::string payload;
};

static void onGetResponse(const ClientResponse& res, void* ctx) {
    auto* c = static_cast<GetCap*>(ctx);
    ++c->called;
    c->code = res.code;
    if (res.payload && res.payloadLength > 0)
        c->payload.assign(reinterpret_cast<const char*>(res.payload),
                          res.payloadLength);
}

static void handleGet(const Request& /*req*/, Response& res, void* /*ctx*/) {
    res.code = Code::Content;
    const char* body = "hello";
    res.setPayload(body);
}

static void test_get_response() {
    PosixUdpTransport srvTx, cliTx;
    TransactionPool   txPool;
    Server srv(srvTx);
    Client cli(cliTx, txPool);

    // begin(0) → OS picks ephemeral ports; localPort() reads them back.
    CHECK(srv.begin(0));
    CHECK(cli.begin(0));

    uint16_t srvPort = srvTx.localPort();
    CHECK(srvPort != 0);

    srv.addResource("/hello", MethodGet, handleGet, nullptr);

    GetCap cap;
    CHECK(cli.get(loopback(srvPort), "/hello", onGetResponse, &cap));

    spin(srv, cli);

    CHECK(cap.called == 1);
    CHECK(cap.code   == Code::Content);
    CHECK(cap.payload == "hello");

    report("test_get_response");
}

// ---------------------------------------------------------------------------
// Test 3: Observe registration + one notification over real sockets
// ---------------------------------------------------------------------------

static const char* g_obsVal = "42";

static void handleObs(const Request& /*req*/, Response& res, void* /*ctx*/) {
    res.code = Code::Content;
    res.setPayload(g_obsVal);
}

struct ObsCap {
    int         called      = 0;
    std::string lastPayload;
};

static void onObs(const ClientResponse& res, void* ctx) {
    auto* c = static_cast<ObsCap*>(ctx);
    ++c->called;
    if (res.payload && res.payloadLength > 0)
        c->lastPayload.assign(reinterpret_cast<const char*>(res.payload),
                              res.payloadLength);
}

static void test_observe_over_posix() {
    PosixUdpTransport srvTx, cliTx;
    TransactionPool   txPool;
    Server srv(srvTx);
    Client cli(cliTx, txPool);

    CHECK(srv.begin(0));
    CHECK(cli.begin(0));
    uint16_t srvPort = srvTx.localPort();
    CHECK(srvPort != 0);

    srv.addResource("/obs", MethodGet, handleObs, nullptr, /*observable=*/true);

    ObsCap cap;
    CHECK(cli.observe(loopback(srvPort), "/obs", onObs, &cap));

    // Drive registration + initial response.
    spin(srv, cli, 40);
    CHECK(cap.called >= 1);
    CHECK(cap.lastPayload == "42");

    // Push a notification.
    g_obsVal = "99";
    srv.notify("/obs",
               reinterpret_cast<const uint8_t*>("99"), 2);
    spin(srv, cli, 30);

    CHECK(cap.called >= 2);
    CHECK(cap.lastPayload == "99");

    report("test_observe_over_posix");
}

// ---------------------------------------------------------------------------
// Probe whether the kernel supports IPv6 (runtime check).
// ---------------------------------------------------------------------------

static bool hasIpv6() {
    int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

// ---------------------------------------------------------------------------
// Test 4: Endpoint IPv6 struct semantics (pure struct, no socket)
// ---------------------------------------------------------------------------

static void test_endpoint_ipv6_struct() {
    // Default-constructed Endpoint is IPv4.
    Endpoint ep4;
    CHECK(!ep4.isV6);
    CHECK(ep4.ip[0] == 0 && ep4.port == 0);

    // Build an IPv4 endpoint and compare.
    Endpoint a, b;
    a.ip[0]=192; a.ip[1]=168; a.ip[2]=1; a.ip[3]=10; a.port=5683;
    b = a;
    CHECK(a == b);
    b.ip[3] = 11;
    CHECK(a != b);

    // Build an IPv6 endpoint (::1).
    Endpoint v6a, v6b;
    v6a.isV6 = true;
    v6a.v6[15] = 1;   // ::1
    v6a.port = 5683;
    v6b = v6a;
    CHECK(v6a == v6b);
    v6b.v6[15] = 2;   // ::2
    CHECK(v6a != v6b);

    // IPv4 and IPv6 endpoints with same port are not equal.
    Endpoint v4, v6;
    v4.ip[3] = 1; v4.port = 5683;
    v6.isV6 = true; v6.v6[15] = 1; v6.port = 5683;
    CHECK(v4 != v6);

    report("test_endpoint_ipv6_struct");
}

// ---------------------------------------------------------------------------
// Test 5: IPv6 raw loopback (::1) — skipped if kernel has no IPv6
// ---------------------------------------------------------------------------

static void test_raw_ipv6_loopback() {
    if (!hasIpv6()) {
        printf("%-64s SKIP (no IPv6)\n", "test_raw_ipv6_loopback");
        return;
    }

    PosixUdpTransport tx, rx;
    CHECK(tx.begin(0));
    CHECK(rx.begin(0));

    uint16_t rxPort = rx.localPort();
    CHECK(rxPort != 0);

    // Build IPv6 ::1 destination.
    Endpoint dst;
    dst.isV6   = true;
    dst.v6[15] = 1; // ::1
    dst.port   = rxPort;

    const uint8_t kFrame[] = {0x40, 0x02, 0xAB, 0xCD}; // CON POST, MID 0xABCD
    CHECK(tx.send(dst, kFrame, sizeof(kFrame)));

    usleep(10000); // 10 ms

    uint8_t buf[64] = {};
    Endpoint from{};
    size_t n = rx.receive(buf, sizeof(buf), from);
    CHECK(n == sizeof(kFrame));
    CHECK(memcmp(buf, kFrame, sizeof(kFrame)) == 0);
    // Sender is ::1 — from.isV6 should be true (not IPv4-mapped).
    CHECK(from.isV6);
    CHECK(from.v6[15] == 1);
    // All other bytes of ::1 must be 0.
    bool allZero = true;
    for (int i = 0; i < 15; ++i) if (from.v6[i] != 0) { allZero = false; break; }
    CHECK(allZero);
    CHECK(from.port == tx.localPort());

    report("test_raw_ipv6_loopback");
}

// ---------------------------------------------------------------------------
// Test 6: GET → 2.05 Content over IPv6 with Server + Client
// ---------------------------------------------------------------------------

static void handleGetV6(const Request& /*req*/, Response& res, void* /*ctx*/) {
    res.code = Code::Content;
    res.setPayload("v6ok");
}

static void test_get_over_ipv6() {
    if (!hasIpv6()) {
        printf("%-64s SKIP (no IPv6)\n", "test_get_over_ipv6");
        return;
    }

    PosixUdpTransport srvTx, cliTx;
    TransactionPool   txPool;
    Server srv(srvTx);
    Client cli(cliTx, txPool);

    CHECK(srv.begin(0));
    CHECK(cli.begin(0));

    uint16_t srvPort = srvTx.localPort();
    CHECK(srvPort != 0);

    srv.addResource("/v6", MethodGet, handleGetV6, nullptr);

    // Address server via ::1.
    Endpoint srvEp;
    srvEp.isV6   = true;
    srvEp.v6[15] = 1;
    srvEp.port   = srvPort;

    GetCap cap;
    CHECK(cli.get(srvEp, "/v6", onGetResponse, &cap));

    spin(srv, cli, 40);

    CHECK(cap.called == 1);
    CHECK(cap.code   == Code::Content);
    CHECK(cap.payload == "v6ok");

    report("test_get_over_ipv6");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    printf("=== PulseCoAP POSIX transport tests ===\n\n");

    test_raw_loopback();
    test_get_response();
    test_observe_over_posix();
    test_endpoint_ipv6_struct();
    test_raw_ipv6_loopback();
    test_get_over_ipv6();

    printf("\n%d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
