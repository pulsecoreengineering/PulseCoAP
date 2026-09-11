// test_path_templates.cpp
// Integration tests for URI-template resource paths (ROADMAP v0.6.0).
//
// Verifies: exact-path fast path, single-parameter templates, multi-parameter
// templates, exact-before-template priority, 404 when no template matches,
// wrong parameter name returns nullptr, segment-count mismatch is rejected.
//
// Check count target: ~28 checks.

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
    printf("%-60s %s\n", name, g_failures == 0 ? "OK" : "FAILED");
}

static Endpoint serverEp() {
    Endpoint e;
    e.ip[0]=127; e.ip[1]=0; e.ip[2]=0; e.ip[3]=1;
    e.port = 5683;
    return e;
}

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
// Capture helpers
// ---------------------------------------------------------------------------

struct Capture {
    int         callCount  = 0;
    Code        code       = Code::Empty;
    std::string payload;
};

static void onResp(const ClientResponse& res, void* ctx) {
    auto* c = static_cast<Capture*>(ctx);
    ++c->callCount;
    c->code = res.code;
    if (res.payload && res.payloadLength > 0)
        c->payload.assign(reinterpret_cast<const char*>(res.payload), res.payloadLength);
}

// ---------------------------------------------------------------------------
// Test 1: exact path still dispatches correctly (regression)
// ---------------------------------------------------------------------------

static void handleExact(const Request& req, Response& res, void* ctx) {
    (void)req;
    auto* s = static_cast<std::string*>(ctx);
    *s = "exact";
    res.code = Code::Content;
    res.setPayload("exact");
}

static void test_exact_path_regression() {
    Fixture f;
    std::string called;
    f.server.addResource("/status", MethodGet, handleExact, &called);

    Capture cap;
    CHECK(f.client.get(serverEp(), "/status", onResp, &cap));
    f.tick(0); f.tick(10);

    CHECK(cap.callCount == 1);
    CHECK(cap.code == Code::Content);
    CHECK(cap.payload == "exact");
    CHECK(called == "exact");

    report("test_exact_path_regression");
}

// ---------------------------------------------------------------------------
// Test 2: single template parameter
// ---------------------------------------------------------------------------

struct ParamCapture {
    int         callCount = 0;
    std::string idValue;
};

static void handleSingleParam(const Request& req, Response& res, void* ctx) {
    auto* c = static_cast<ParamCapture*>(ctx);
    ++c->callCount;
    const char* id = req.pathParam("id");
    if (id) c->idValue = id;
    res.code = Code::Content;
    res.setPayload(id ? id : "missing");
}

static void test_single_template_param() {
    Fixture f;
    ParamCapture cap;
    f.server.addResource("/sensors/:id", MethodGet, handleSingleParam, &cap);

    Capture resp;
    CHECK(f.client.get(serverEp(), "/sensors/42", onResp, &resp));
    f.tick(0); f.tick(10);

    CHECK(resp.callCount == 1);
    CHECK(resp.code == Code::Content);
    CHECK(cap.callCount == 1);
    CHECK(cap.idValue == "42");
    CHECK(resp.payload == "42");

    report("test_single_template_param");
}

// ---------------------------------------------------------------------------
// Test 3: different value for the same template
// ---------------------------------------------------------------------------

static void test_single_param_different_value() {
    Fixture f;
    ParamCapture cap;
    f.server.addResource("/sensors/:id", MethodGet, handleSingleParam, &cap);

    Capture resp;
    CHECK(f.client.get(serverEp(), "/sensors/temperature", onResp, &resp));
    f.tick(0); f.tick(10);

    CHECK(cap.idValue == "temperature");
    CHECK(resp.payload == "temperature");

    report("test_single_param_different_value");
}

// ---------------------------------------------------------------------------
// Test 4: multi-segment template (two parameters)
// ---------------------------------------------------------------------------

struct MultiCapture {
    int         callCount = 0;
    std::string devValue;
    std::string chValue;
    bool        wrongNameNull = false;
};

static void handleMultiParam(const Request& req, Response& res, void* ctx) {
    auto* c = static_cast<MultiCapture*>(ctx);
    ++c->callCount;
    const char* dev = req.pathParam("dev");
    const char* ch  = req.pathParam("ch");
    if (dev) c->devValue = dev;
    if (ch)  c->chValue  = ch;
    c->wrongNameNull = (req.pathParam("nonexistent") == nullptr);
    res.code = Code::Content;
    res.setPayload("ok");
}

static void test_multi_segment_template() {
    Fixture f;
    MultiCapture cap;
    f.server.addResource("/devices/:dev/ch/:ch", MethodGet, handleMultiParam, &cap);

    Capture resp;
    CHECK(f.client.get(serverEp(), "/devices/3/ch/0", onResp, &resp));
    f.tick(0); f.tick(10);

    CHECK(cap.callCount == 1);
    CHECK(cap.devValue == "3");
    CHECK(cap.chValue  == "0");
    CHECK(cap.wrongNameNull);          // req.pathParam("nonexistent") == nullptr
    CHECK(resp.code == Code::Content);

    report("test_multi_segment_template");
}

// ---------------------------------------------------------------------------
// Test 5: exact match wins over template when both could apply
// ---------------------------------------------------------------------------

static std::string g_whichHandlerCalled;

static void handleExactAlias(const Request& req, Response& res, void* /*ctx*/) {
    (void)req;
    g_whichHandlerCalled = "exact";
    res.code = Code::Content;
    res.setPayload("exact");
}

static void handleTemplateAlias(const Request& req, Response& res, void* /*ctx*/) {
    const char* id = req.pathParam("id");
    g_whichHandlerCalled = std::string("template:") + (id ? id : "null");
    res.code = Code::Content;
    res.setPayload("template");
}

static void test_exact_beats_template() {
    Fixture f;
    // Register exact first, then template (order shouldn't matter — exact
    // always wins regardless of registration order).
    f.server.addResource("/nodes/master", MethodGet, handleExactAlias,   nullptr);
    f.server.addResource("/nodes/:id",    MethodGet, handleTemplateAlias, nullptr);

    // GET /nodes/master → should match the exact registration.
    Capture r1;
    g_whichHandlerCalled.clear();
    CHECK(f.client.get(serverEp(), "/nodes/master", onResp, &r1));
    f.tick(0); f.tick(10);
    CHECK(r1.callCount == 1);
    CHECK(g_whichHandlerCalled == "exact");

    // GET /nodes/worker → should fall through to the template.
    Capture r2;
    g_whichHandlerCalled.clear();
    CHECK(f.client.get(serverEp(), "/nodes/worker", onResp, &r2));
    f.tick(20); f.tick(30);
    CHECK(r2.callCount == 1);
    CHECK(g_whichHandlerCalled == "template:worker");

    report("test_exact_beats_template");
}

// ---------------------------------------------------------------------------
// Test 6: 404 when no template matches (segment count mismatch)
// ---------------------------------------------------------------------------

static void handleAny(const Request& /*req*/, Response& res, void* /*ctx*/) {
    res.code = Code::Content;
}

static void test_template_segment_count_mismatch_404() {
    Fixture f;
    f.server.addResource("/sensors/:id", MethodGet, handleAny, nullptr);

    // Path has more segments than template → should NOT match → 4.04.
    Capture resp;
    CHECK(f.client.get(serverEp(), "/sensors/temp/extra", onResp, &resp));
    f.tick(0); f.tick(10);

    CHECK(resp.callCount == 1);
    CHECK(resp.code == Code::NotFound);

    report("test_template_segment_count_mismatch_404");
}

// ---------------------------------------------------------------------------
// Test 7: 404 when no template matches (prefix mismatch)
// ---------------------------------------------------------------------------

static void test_template_prefix_mismatch_404() {
    Fixture f;
    f.server.addResource("/sensors/:id", MethodGet, handleAny, nullptr);

    // "/actuators/3" does not match "/sensors/:id" (prefix differs).
    Capture resp;
    CHECK(f.client.get(serverEp(), "/actuators/3", onResp, &resp));
    f.tick(0); f.tick(10);

    CHECK(resp.callCount == 1);
    CHECK(resp.code == Code::NotFound);

    report("test_template_prefix_mismatch_404");
}

// ---------------------------------------------------------------------------
// Test 8: template with literal middle segment
// ---------------------------------------------------------------------------

struct MidCapture { int calls = 0; std::string val; };

static void handleMid(const Request& req, Response& res, void* ctx) {
    auto* c = static_cast<MidCapture*>(ctx);
    ++c->calls;
    const char* v = req.pathParam("reading");
    if (v) c->val = v;
    res.code = Code::Content;
    res.setPayload("ok");
}

static void test_template_literal_middle_segment() {
    Fixture f;
    MidCapture cap;
    // Pattern: /data/latest/:reading  — "data" and "latest" are literals
    f.server.addResource("/data/latest/:reading", MethodGet, handleMid, &cap);

    // Matching path
    Capture r1;
    CHECK(f.client.get(serverEp(), "/data/latest/voltage", onResp, &r1));
    f.tick(0); f.tick(10);
    CHECK(cap.calls == 1);
    CHECK(cap.val == "voltage");
    CHECK(r1.code == Code::Content);

    // Non-matching path (middle literal "latest" ≠ "old")
    Capture r2;
    CHECK(f.client.get(serverEp(), "/data/old/voltage", onResp, &r2));
    f.tick(20); f.tick(30);
    CHECK(r2.code == Code::NotFound);

    report("test_template_literal_middle_segment");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    printf("=== PulseCoAP URI template path tests ===\n\n");

    test_exact_path_regression();
    test_single_template_param();
    test_single_param_different_value();
    test_multi_segment_template();
    test_exact_beats_template();
    test_template_segment_count_mismatch_404();
    test_template_prefix_mismatch_404();
    test_template_literal_middle_segment();

    printf("\n%d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
