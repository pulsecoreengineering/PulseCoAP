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
// Fires once per responding server during a discover() collection window.
// `server` is the unicast endpoint that replied; `linkFormat`/`length` is
// the raw CoRE Link Format body (Content-Format 40, RFC 6690).
using DiscoverHandler = void (*)(const Endpoint& server,
                                  const uint8_t* linkFormat, size_t length,
                                  void* userContext);

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

    // Multicast resource discovery (RFC 7252 §8)
    // -----------------------------------------------------------------------
    // Sends a NON GET for /.well-known/core to 224.0.1.187:port (the IANA
    // CoAP all-nodes address). Every server on the LAN that has joined the
    // multicast group responds unicast; onDiscover fires once per responder.
    // The slot stays active for PULSECOAP_DISCOVER_TIMEOUT_MS ms, then frees
    // automatically. Returns false if no discover slot is free or the send
    // fails.
    bool discover(DiscoverHandler onDiscover, void* userContext = nullptr,
                  uint16_t port = 5683);
    // Overload for a custom multicast destination (e.g. [FF02::FD]:5683 for
    // IPv6 or an alternate port in tests).
    bool discover(const Endpoint& multicastEp, DiscoverHandler onDiscover,
                  void* userContext = nullptr);

    // Returns an Endpoint for the IANA IPv4 CoAP all-nodes multicast address
    // 224.0.1.187. Callers that need IPv6 can build [FF02::FD] themselves.
    static Endpoint allNodesEndpoint(uint16_t port = 5683);

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

    struct DiscoverSlot {
        bool active = false;
        uint8_t token[PULSECOAP_MAX_TOKEN_LEN] = {};
        uint8_t tokenLen = 0;
        DiscoverHandler onDiscover = nullptr;
        void* userContext = nullptr;
        uint32_t issuedAt = 0; // nowMs when discover() was called; expiry is wraparound-safe
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
    DiscoverSlot discovers_[PULSECOAP_MAX_DISCOVERS];
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
