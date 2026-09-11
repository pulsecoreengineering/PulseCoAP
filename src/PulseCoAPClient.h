// PulseCoAPClient.h
// CoAP client role (RFC 7252): GET/PUT/POST/DELETE against a remote
// resource, plus Observe (RFC 7641) registration for push-style updates.
// Confirmable requests are handed to a TransactionPool for retry/backoff,
// so a lost request or ACK is retried automatically rather than hanging.
#pragma once

#include "PulseCoAPConfig.h"

#if PULSECOAP_ENABLE_CLIENT

#include <stdint.h>

#include "PulseCoAPBlockwise.h"
#include "PulseCoAPMessage.h"
#include "PulseCoAPTransaction.h"
#include "PulseCoAPTransport.h"
#include "PulseCoAPTypes.h"

namespace pulsecoap {

struct ClientResponse {
    Code code;
    const uint8_t* payload;
    size_t payloadLength;
    ContentFormat contentFormat;
};

// Fires once for get/put/post/del; fires once per notification (including
// the initial value) for observe().
using ResponseHandler = void (*)(const ClientResponse& res, void* userContext);
// Fires if a Confirmable request exhausts PULSECOAP_MAX_RETRANSMIT retries
// with no ACK/response.
using TimeoutHandler = void (*)(void* userContext);

class Client {
public:
    Client(Transport& transport, TransactionPool& transactions)
        : transport_(transport), transactions_(transactions) {}

    // localPort = 0 lets the underlying transport pick an ephemeral port.
    bool begin(uint16_t localPort = 0) { return transport_.begin(localPort); }

    bool get(const Endpoint& server, const char* path, ResponseHandler onResponse,
              void* userContext = nullptr, bool confirmable = true);
    bool put(const Endpoint& server, const char* path, const uint8_t* payload, size_t payloadLength,
              ContentFormat contentFormat, ResponseHandler onResponse, void* userContext = nullptr,
              bool confirmable = true);
    bool post(const Endpoint& server, const char* path, const uint8_t* payload, size_t payloadLength,
               ContentFormat contentFormat, ResponseHandler onResponse, void* userContext = nullptr,
               bool confirmable = true);
    bool del(const Endpoint& server, const char* path, ResponseHandler onResponse,
              void* userContext = nullptr, bool confirmable = true);

    // Registers Observe on `path` (sent Confirmable, so registration
    // itself is retried if lost). `onResponse` fires for the initial
    // response and every later notification. Multiple resources on the
    // same server can be observed simultaneously — each observe occupies
    // its own pending slot and is tracked by (server, path).
    bool observe(const Endpoint& server, const char* path, ResponseHandler onResponse,
                  void* userContext = nullptr);
    // Sends a GET with Observe=1 to deregister the specific (server, path)
    // subscription (RFC 7641 §3.6). Matches by path so two simultaneous
    // observes on the same server are cancelled independently.
    bool cancelObserve(const Endpoint& server, const char* path);

    void setTimeoutHandler(TimeoutHandler onTimeout) { onTimeout_ = onTimeout; }

    // Receives any waiting response/notification and drives retransmission.
    // Call regularly (e.g. every loop() iteration).
    void poll(uint32_t nowMs);

private:
    struct PendingRequest {
        bool active = false;
        Endpoint server;
        uint8_t token[PULSECOAP_MAX_TOKEN_LEN] = {0};
        uint8_t tokenLen = 0;
        ResponseHandler onResponse = nullptr;
        void* userContext = nullptr;
        bool observing = false;
        // Stored so cancelObserve() can distinguish two simultaneous observes
        // on the same server (different paths, different slots/tokens).
        char path[PULSECOAP_MAX_URI_PATH_LEN] = {};
    };

    bool sendRequest(const Endpoint& server, const char* path, Code method,
                       const uint8_t* payload, size_t payloadLength, ContentFormat contentFormat,
                       bool confirmable, bool observeRegister, bool observeDeregister,
                       ResponseHandler onResponse, void* userContext, bool markObserving);
    int findPendingByToken(const uint8_t* token, uint8_t tokenLen) const;
    int allocatePending();
    uint8_t nextToken();

    static void resendTrampoline(void* ctx, const Endpoint& remote, const uint8_t* message, size_t messageLen);
    static void timeoutTrampoline(void* ctx, const Endpoint& remote, const uint8_t* token, uint8_t tokenLen);

#if PULSECOAP_ENABLE_BLOCKWISE
    // Send one Block1 upload block. Called for the first block and each time
    // a 2.31 Continue is received. Returns false if encoding or send fails.
    bool sendBlock1(int slot);

    // Send a follow-up GET for the next Block2 (NUM+1). Reuses the existing
    // pending slot and token.
    bool sendBlock2Request(int slot);
#endif

    Transport& transport_;
    TransactionPool& transactions_;
    PendingRequest pending_[PULSECOAP_MAX_TRANSACTIONS];
    uint16_t nextMessageId_ = 1;
    uint8_t nextTokenByte_ = 1;
    uint32_t lastNowMs_ = 0; // updated by poll(); used as the "now" for requests sent between polls
    TimeoutHandler onTimeout_ = nullptr;
    uint8_t rxBuffer_[PULSECOAP_MAX_MSG_SIZE];

#if PULSECOAP_ENABLE_BLOCKWISE
    Block1UploadState block1up_[PULSECOAP_MAX_TRANSACTIONS];
    Block2RecvState   block2recv_[PULSECOAP_MAX_TRANSACTIONS];
#endif
};

} // namespace pulsecoap

#endif // PULSECOAP_ENABLE_CLIENT
