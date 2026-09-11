// PulseCoAPTransaction.h
// Reliability layer for Confirmable (CON) messages: a fixed pool that
// tracks in-flight messages awaiting ACK and retransmits them with the
// exponential backoff RFC 7252 §4.8 specifies. Shared by both
// PulseCoAPClient (CON requests awaiting a response) and PulseCoAPServer
// (CON responses/notifications awaiting an ACK).
//
// Timing is caller-driven: you pass "now" (typically millis()) into
// start()/tick() rather than this class reading a clock itself. That keeps
// it host-testable with no Arduino dependency, and lets it share whatever
// timebase the rest of your firmware already uses.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "PulseCoAPConfig.h"
#include "PulseCoAPTransport.h"

namespace pulsecoap {

enum class TransactionState : uint8_t {
    Free = 0,
    AwaitingAck
};

struct Transaction {
    TransactionState state = TransactionState::Free;
    Endpoint remote;
    uint16_t messageId = 0;

    uint8_t token[PULSECOAP_MAX_TOKEN_LEN] = {0};
    uint8_t tokenLen = 0;

    uint8_t retriesSent = 0;         // retransmissions sent so far (0 = only the original send)
    uint32_t currentTimeoutMs = 0;   // doubles on each retry (RFC 7252 §4.8 backoff)
    uint32_t nextRetryAtMs = 0;

    // Raw encoded bytes, kept so a retransmit doesn't need to re-encode —
    // and so the caller's original buffer is free to be reused immediately.
    uint8_t message[PULSECOAP_MAX_MSG_SIZE];
    size_t messageLen = 0;
};

class TransactionPool {
public:
    TransactionPool() { reset(); }

    void reset();

    // Begins tracking a just-sent Confirmable message. Copies `message`
    // into the pool's own buffer (unlike PulseCoAPMessage's option/payload
    // pointers, this data must outlive the caller's send buffer). Returns
    // the slot index on success, -1 if the pool is full or the inputs
    // don't fit the configured limits.
    int start(const Endpoint& remote, uint16_t messageId, const uint8_t* token,
              uint8_t tokenLen, const uint8_t* message, size_t messageLen,
              uint32_t nowMs);

    // Stops tracking the transaction matching (remote, messageId) — call
    // this when an ACK or RST for that message ID arrives.
    bool complete(const Endpoint& remote, uint16_t messageId);

    // Stops tracking the transaction matching (remote, token) — useful for
    // matching a separate (non-piggybacked) response that arrives with a
    // different message ID but the original request's token.
    bool completeByToken(const Endpoint& remote, const uint8_t* token, uint8_t tokenLen);

    // Looks up the token stored for the in-flight transaction identified by
    // (remote, messageId). Returns true and fills tokenOut/tokenLenOut on a
    // match. Used by the client to recover the pending-request token from an
    // incoming RST (which carries only the message ID, not the original token).
    // Must be called BEFORE complete() — the slot is already freed after that.
    bool getToken(const Endpoint& remote, uint16_t messageId,
                  uint8_t* tokenOut, uint8_t& tokenLenOut) const;

    // Function-pointer + context callbacks (no std::function — this stays
    // allocation-free and ABI-stable on small targets).
    using ResendFn = void (*)(void* ctx, const Endpoint& remote, const uint8_t* message, size_t messageLen);
    using TimeoutFn = void (*)(void* ctx, const Endpoint& remote, const uint8_t* token, uint8_t tokenLen);

    // Advances the pool's clock to `nowMs`. Any transaction whose retry
    // deadline has passed either gets resent (backoff doubled, retriesSent
    // incremented) or, once PULSECOAP_MAX_RETRANSMIT is exhausted, is freed
    // and reported via `onTimeout`. Call this regularly from your main
    // loop/task. `onTimeout` may be nullptr.
    void tick(uint32_t nowMs, ResendFn resend, TimeoutFn onTimeout, void* ctx);

    uint8_t activeCount() const;

    // RFC 7252 §4.8: initial timeout is randomized within
    // [ACK_TIMEOUT, ACK_TIMEOUT * ACK_RANDOM_FACTOR]. This is a pure
    // function of `seedForJitter` (xorshift32, not a hardware RNG) so it
    // stays deterministic and host-testable; pass something that varies
    // per message (e.g. the message ID or a free-running counter).
    static uint32_t computeInitialTimeoutMs(uint32_t seedForJitter);

private:
    Transaction slots_[PULSECOAP_MAX_TRANSACTIONS];
};

} // namespace pulsecoap
