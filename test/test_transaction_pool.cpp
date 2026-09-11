// Host-side test for TransactionPool's retry/backoff/timeout behaviour.
// Build: g++ -std=c++11 -Wall -Wextra -Isrc test/test_transaction_pool.cpp src/PulseCoAPTransaction.cpp -o test_transaction_pool
//        ./test_transaction_pool
#include <cstdio>
#include <cstring>

#include "../src/PulseCoAPTransaction.h"

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

struct RecordedResend {
    Endpoint remote;
    uint8_t message[PULSECOAP_MAX_MSG_SIZE];
    size_t messageLen = 0;
};

struct RecordedTimeout {
    Endpoint remote;
    uint8_t token[PULSECOAP_MAX_TOKEN_LEN];
    uint8_t tokenLen = 0;
};

struct CallbackLog {
    RecordedResend resends[16];
    int resendCount = 0;
    RecordedTimeout timeouts[16];
    int timeoutCount = 0;
};

void onResend(void* ctx, const Endpoint& remote, const uint8_t* message, size_t messageLen) {
    auto* log = static_cast<CallbackLog*>(ctx);
    RecordedResend& r = log->resends[log->resendCount++];
    r.remote = remote;
    r.messageLen = messageLen;
    std::memcpy(r.message, message, messageLen);
}

void onTimeout(void* ctx, const Endpoint& remote, const uint8_t* token, uint8_t tokenLen) {
    auto* log = static_cast<CallbackLog*>(ctx);
    RecordedTimeout& t = log->timeouts[log->timeoutCount++];
    t.remote = remote;
    t.tokenLen = tokenLen;
    std::memcpy(t.token, token, tokenLen);
}

} // namespace

#define CHECK(cond) check((cond), #cond)

static void test_start_and_explicit_complete() {
    std::printf("test_start_and_explicit_complete\n");

    TransactionPool pool;
    Endpoint remote;
    remote.ip[0] = 10; remote.ip[1] = 0; remote.ip[2] = 0; remote.ip[3] = 5;
    remote.port = 5683;

    const uint8_t token[] = {0x01, 0x02};
    const uint8_t message[] = {0x40, 0x01, 0x00, 0x2A, 0x01, 0x02};

    CHECK(pool.activeCount() == 0);
    int slot = pool.start(remote, /*messageId=*/0x002A, token, 2, message, sizeof(message), /*nowMs=*/1000);
    CHECK(slot >= 0);
    CHECK(pool.activeCount() == 1);

    // Ticking well before the deadline should not resend or time out.
    CallbackLog log;
    pool.tick(1500, onResend, onTimeout, &log);
    CHECK(log.resendCount == 0);
    CHECK(log.timeoutCount == 0);

    // ACK arrives -> transaction should stop being tracked.
    CHECK(pool.complete(remote, 0x002A));
    CHECK(pool.activeCount() == 0);

    // Completing again (already gone) should report false.
    CHECK(!pool.complete(remote, 0x002A));
}

static void test_backoff_doubles_and_then_times_out() {
    std::printf("test_backoff_doubles_and_then_times_out\n");

    TransactionPool pool;
    Endpoint remote;
    remote.ip[0] = 192; remote.ip[1] = 168; remote.ip[2] = 1; remote.ip[3] = 42;
    remote.port = 5683;

    const uint8_t token[] = {0xAA};
    const uint8_t message[] = {0x40, 0x01, 0x00, 0x01, 0xAA};

    uint32_t now = 0;
    int slot = pool.start(remote, 0x0001, token, 1, message, sizeof(message), now);
    CHECK(slot >= 0);

    // PULSECOAP_MAX_RETRANSMIT retransmissions must happen (each doubling
    // the previous timeout, mirrored here step by step since each new
    // deadline is computed from the "now" at the moment of that retry —
    // it isn't simply 2x the previous deadline from time zero), then the
    // transaction must time out and free its slot rather than retry
    // forever.
    CallbackLog log;
    uint32_t currentTimeout = TransactionPool::computeInitialTimeoutMs(0x0001);
    uint32_t nextDeadline = now + currentTimeout; // matches start()'s nextRetryAtMs

    for (int i = 0; i < PULSECOAP_MAX_RETRANSMIT; ++i) {
        now = nextDeadline + 1; // just past the current deadline
        pool.tick(now, onResend, onTimeout, &log);
        CHECK(log.resendCount == i + 1);
        currentTimeout *= 2;          // backoff should have doubled internally too
        nextDeadline = now + currentTimeout; // tick() re-anchors from the "now" it was called with
    }
    CHECK(log.resendCount == PULSECOAP_MAX_RETRANSMIT);
    CHECK(log.timeoutCount == 0);
    CHECK(pool.activeCount() == 1); // still tracked, retries not yet exhausted at the pool level

    // One more tick past the final deadline should exhaust retries, free
    // the slot, and fire the timeout callback with the original token.
    now = nextDeadline + 1;
    pool.tick(now, onResend, onTimeout, &log);
    CHECK(log.timeoutCount == 1);
    CHECK(log.timeouts[0].tokenLen == 1 && log.timeouts[0].token[0] == 0xAA);
    CHECK(pool.activeCount() == 0);
}

static void test_pool_capacity_and_completed_by_token() {
    std::printf("test_pool_capacity_and_completed_by_token\n");

    TransactionPool pool;
    Endpoint remote;
    remote.port = 5683;

    const uint8_t message[] = {0x40, 0x01, 0x00, 0x00};

    // Fill the pool to PULSECOAP_MAX_TRANSACTIONS...
    for (int i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        uint8_t token = static_cast<uint8_t>(i);
        int slot = pool.start(remote, static_cast<uint16_t>(i), &token, 1, message, sizeof(message), 0);
        CHECK(slot >= 0);
    }
    CHECK(pool.activeCount() == PULSECOAP_MAX_TRANSACTIONS);

    // ...one more must fail, since every slot is in use.
    uint8_t overflowToken = 0xFF;
    CHECK(pool.start(remote, 999, &overflowToken, 1, message, sizeof(message), 0) == -1);

    // Freeing one by token should make room again.
    uint8_t firstToken = 0;
    CHECK(pool.completeByToken(remote, &firstToken, 1));
    CHECK(pool.activeCount() == PULSECOAP_MAX_TRANSACTIONS - 1);
    CHECK(pool.start(remote, 1000, &overflowToken, 1, message, sizeof(message), 0) >= 0);
}

int main() {
    test_start_and_explicit_complete();
    test_backoff_doubles_and_then_times_out();
    test_pool_capacity_and_completed_by_token();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
