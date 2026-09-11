#include "PulseCoAPTransaction.h"

namespace pulsecoap {

void TransactionPool::reset() {
    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        slots_[i] = Transaction();
    }
}

uint32_t TransactionPool::computeInitialTimeoutMs(uint32_t seedForJitter) {
    uint32_t x = seedForJitter ? seedForJitter : 0x2545F491u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    const uint32_t base = PULSECOAP_ACK_TIMEOUT_MS;
    const uint32_t upper = (base * PULSECOAP_ACK_RANDOM_FACTOR_NUM) / PULSECOAP_ACK_RANDOM_FACTOR_DEN;
    const uint32_t span = upper > base ? (upper - base) : 0;
    return span > 0 ? base + (x % span) : base;
}

int TransactionPool::start(const Endpoint& remote, uint16_t messageId, const uint8_t* token,
                            uint8_t tokenLen, const uint8_t* message, size_t messageLen,
                            uint32_t nowMs) {
    if (tokenLen > PULSECOAP_MAX_TOKEN_LEN) return -1;
    if (messageLen > PULSECOAP_MAX_MSG_SIZE) return -1;

    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        Transaction& t = slots_[i];
        if (t.state != TransactionState::Free) continue;

        t.state = TransactionState::AwaitingAck;
        t.remote = remote;
        t.messageId = messageId;
        t.tokenLen = tokenLen;
        for (uint8_t k = 0; k < tokenLen; ++k) t.token[k] = token[k];

        t.retriesSent = 0;
        t.currentTimeoutMs = computeInitialTimeoutMs(messageId);
        t.nextRetryAtMs = nowMs + t.currentTimeoutMs;

        t.messageLen = messageLen;
        for (size_t k = 0; k < messageLen; ++k) t.message[k] = message[k];

        return i;
    }
    return -1;
}

bool TransactionPool::complete(const Endpoint& remote, uint16_t messageId) {
    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        Transaction& t = slots_[i];
        if (t.state == TransactionState::AwaitingAck && t.remote == remote && t.messageId == messageId) {
            t.state = TransactionState::Free;
            return true;
        }
    }
    return false;
}

bool TransactionPool::completeByToken(const Endpoint& remote, const uint8_t* token, uint8_t tokenLen) {
    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        Transaction& t = slots_[i];
        if (t.state != TransactionState::AwaitingAck) continue;
        if (t.remote != remote || t.tokenLen != tokenLen) continue;

        bool match = true;
        for (uint8_t k = 0; k < tokenLen; ++k) {
            if (t.token[k] != token[k]) { match = false; break; }
        }
        if (match) {
            t.state = TransactionState::Free;
            return true;
        }
    }
    return false;
}

void TransactionPool::tick(uint32_t nowMs, ResendFn resend, TimeoutFn onTimeout, void* ctx) {
    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        Transaction& t = slots_[i];
        if (t.state != TransactionState::AwaitingAck) continue;

        // Signed-difference comparison so this keeps working correctly
        // across a millis()-style counter wraparound.
        int32_t remaining = static_cast<int32_t>(t.nextRetryAtMs - nowMs);
        if (remaining > 0) continue;

        if (t.retriesSent < PULSECOAP_MAX_RETRANSMIT) {
            t.retriesSent++;
            t.currentTimeoutMs *= 2;
            t.nextRetryAtMs = nowMs + t.currentTimeoutMs;
            if (resend) resend(ctx, t.remote, t.message, t.messageLen);
        } else {
            Endpoint remote = t.remote;
            uint8_t token[PULSECOAP_MAX_TOKEN_LEN];
            uint8_t tokenLen = t.tokenLen;
            for (uint8_t k = 0; k < tokenLen; ++k) token[k] = t.token[k];

            t.state = TransactionState::Free; // free before the callback, in case it starts a new transaction
            if (onTimeout) onTimeout(ctx, remote, token, tokenLen);
        }
    }
}

bool TransactionPool::getToken(const Endpoint& remote, uint16_t messageId,
                               uint8_t* tokenOut, uint8_t& tokenLenOut) const {
    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        const Transaction& t = slots_[i];
        if (t.state == TransactionState::AwaitingAck &&
            t.remote == remote && t.messageId == messageId) {
            for (uint8_t k = 0; k < t.tokenLen; ++k) tokenOut[k] = t.token[k];
            tokenLenOut = t.tokenLen;
            return true;
        }
    }
    return false;
}

uint8_t TransactionPool::activeCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        if (slots_[i].state != TransactionState::Free) count++;
    }
    return count;
}

} // namespace pulsecoap
