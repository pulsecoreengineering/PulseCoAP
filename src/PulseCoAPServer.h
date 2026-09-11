// PulseCoAPServer.h
// CoAP server role (RFC 7252): resource registration/dispatch, Observe
// (RFC 7641) push notifications, .well-known/core resource discovery
// (RFC 6690), separate (non-piggybacked) responses so handlers that
// need async work can ACK immediately and reply later, and URI-template
// resource paths so one handler can cover /sensors/:id, /devices/:d/ch/:c, etc.
#pragma once

#include "PulseCoAPConfig.h"

#if PULSECOAP_ENABLE_SERVER

#include <stdint.h>
#include <string.h>

#include "PulseCoAPBlockwise.h"
#include "PulseCoAPMessage.h"
#include "PulseCoAPTransaction.h"
#include "PulseCoAPTransport.h"
#include "PulseCoAPTypes.h"

namespace pulsecoap {

// ---------------------------------------------------------------------------
// Path parameter (URI template match result)
// ---------------------------------------------------------------------------

// A single matched template segment, e.g. name="id", value="42" for a
// resource registered as "/sensors/:id" when the request path is
// "/sensors/42". Both strings are NUL-terminated and bounded by
// PULSECOAP_MAX_PATH_PARAM_LEN (including the terminator).
struct PathParamEntry {
    char name [PULSECOAP_MAX_PATH_PARAM_LEN] = {};
    char value[PULSECOAP_MAX_PATH_PARAM_LEN] = {};
};

// ---------------------------------------------------------------------------
enum MethodMask : uint8_t {
    MethodGet    = 1 << 0,
    MethodPost   = 1 << 1,
    MethodPut    = 1 << 2,
    MethodDelete = 1 << 3,
};

// Opaque handle the handler stores and passes to server.respond() later.
// 0xFF means invalid / not allocated.
using DeferHandle = uint8_t;
static constexpr DeferHandle kInvalidDeferHandle = 0xFF;

struct Request {
    Code method;
    const Message* message; // full decoded request — read extra options/payload from here
    Endpoint remote;
    // Store this and pass it to server.respond() to send a separate response.
    // Only meaningful if the handler sets res.deferred = true.
    DeferHandle deferHandle = kInvalidDeferHandle;

    // When PULSECOAP_ENABLE_BLOCKWISE is set and this is the final block of a
    // Block1 (client-upload) transfer, block1Body points to the fully assembled
    // payload and block1BodyLength is its size. Handlers should prefer these
    // over message->payload()/message->payloadLength() when non-null.
    const uint8_t* block1Body       = nullptr;
    size_t         block1BodyLength = 0;

    // Convenience: returns the effective payload regardless of block-wise or not.
    const uint8_t* payload()       const { return block1Body ? block1Body : message->payload(); }
    size_t         payloadLength() const { return block1Body ? block1BodyLength : message->payloadLength(); }

    // Returns the value of the named URI template parameter, or nullptr if
    // the name is not in the matched template. For example, if the resource
    // was registered as "/sensors/:id" and the request path is "/sensors/42",
    // then pathParam("id") returns "42". The pointer remains valid for the
    // lifetime of this Request object (i.e., inside the handler call).
    const char* pathParam(const char* name) const {
        for (uint8_t i = 0; i < paramCount_; ++i)
            if (strcmp(params_[i].name, name) == 0)
                return params_[i].value;
        return nullptr;
    }

    // Internal: set by Server::handleRequest() before invoking the handler.
    // Handlers should not read these directly — use pathParam() instead.
    PathParamEntry params_[PULSECOAP_MAX_PATH_PARAMS];
    uint8_t        paramCount_ = 0;
};

struct Response {
    Code code = Code::Content;
    ContentFormat contentFormat = ContentFormat::TextPlain;
    const uint8_t* payload = nullptr;
    size_t payloadLength = 0;

    // Set true to defer the response: the server immediately sends an empty
    // ACK (for a CON request) so the client stops retransmitting, and waits
    // for the handler to call server.respond(req.deferHandle, ...) later.
    // For a NON request, setting deferred simply delays the response with
    // no intermediate empty ACK (nothing to stop retransmission of a NON).
    bool deferred = false;

    void setPayload(const uint8_t* data, size_t length) {
        payload = data;
        payloadLength = length;
    }
    void setPayload(const char* text) {
        payload = reinterpret_cast<const uint8_t*>(text);
        payloadLength = text ? strlen(text) : 0;
    }
};

using ResourceHandler = void (*)(const Request& req, Response& res, void* userContext);

class Server {
public:
    explicit Server(Transport& transport) : transport_(transport) {}

    bool begin(uint16_t localPort = 5683);

    // `path` must start with '/' (e.g. "/sensors/temp") and stay valid for
    // the life of the Server — pass a string literal or a static buffer,
    // it is not copied. Set observable=true to let clients Observe this
    // resource; then notify() pushes updates to whoever is observing it.
    bool addResource(const char* path, uint8_t methods, ResourceHandler handler,
                      void* userContext = nullptr, bool observable = false);

    // Attaches an optional CoRE Link Format rt= (resource-type) attribute to a
    // previously registered resource. `path` must match an existing resource;
    // `rt` is stored by pointer (caller keeps it alive). Returns false if the
    // path isn't found. Call after addResource(), before the first poll().
    bool setResourceType(const char* path, const char* rt);

    // Receives and dispatches any waiting request, and drives retransmission
    // of in-flight separate responses. Call regularly (e.g. every loop()).
    void poll(uint32_t nowMs);

    // Sends the deferred response for `handle` (obtained from req.deferHandle
    // inside a ResourceHandler that set res.deferred = true). Sends as CON
    // if the original request was Confirmable (RFC 7252 §5.2.2), NON
    // otherwise. Returns false if handle is invalid or already consumed.
    bool respond(DeferHandle handle, Code code, const uint8_t* payload,
                 size_t payloadLength,
                 ContentFormat contentFormat = ContentFormat::TextPlain);

    // Convenience overload for plain-text string payloads.
    bool respond(DeferHandle handle, Code code, const char* text,
                 ContentFormat contentFormat = ContentFormat::TextPlain) {
        return respond(handle, code,
                       reinterpret_cast<const uint8_t*>(text),
                       text ? strlen(text) : 0, contentFormat);
    }

    // Pushes `payload` to every client currently observing `path`. No-op
    // (returns false) if the resource isn't registered or isn't observable,
    // but never an error if it simply has zero observers yet.
    //
    // `confirmable` (default false) controls the reliability of the push:
    // - false (NON): fire-and-forget; no retransmission, no dead-peer detection.
    // - true  (CON): retransmitted until ACK'd per RFC 7252 §4.8. If the
    //   peer never ACKs (MAX_RETRANSMIT retries exhausted), it is silently
    //   removed from the observer list — the recommended dead-peer-detection
    //   technique from RFC 7641 §4.5. Send CON periodically to clean up
    //   stale registrations without waiting for an explicit RST.
    bool notify(const char* path, const uint8_t* payload, size_t payloadLength,
                ContentFormat contentFormat = ContentFormat::TextPlain,
                bool confirmable = false);

    uint8_t observerCount() const;

private:
    struct Resource {
        const char* path = nullptr;
        uint8_t methods = 0;
        ResourceHandler handler = nullptr;
        void* userContext = nullptr;
        bool observable = false;
        bool isTemplate = false;  // true when path contains a ":param" segment
        uint32_t observeSequence = 0;
        const char* resourceType = nullptr; // optional rt= for .well-known/core; pointer, not copied
    };

    struct Observer {
        bool active = false;
        uint8_t resourceIndex = 0;
        Endpoint remote;
        uint8_t token[PULSECOAP_MAX_TOKEN_LEN] = {0};
        uint8_t tokenLen = 0;
        // Message ID of the last packet we sent to this observer — used to
        // match an incoming RST to the right slot.
        uint16_t lastSentMessageId = 0;
    };

    // Tracks a request whose handler returned res.deferred=true.
    struct DeferredRequest {
        bool active = false;
        Endpoint remote;
        uint8_t token[PULSECOAP_MAX_TOKEN_LEN] = {0};
        uint8_t tokenLen = 0;
        bool wasConfirmable = false; // determines separate-response type
    };

    void handleRequest(const Message& msg, const Endpoint& from);
    void sendPiggybackedResponse(const Message& request, const Endpoint& to, Code code,
                                  const uint8_t* payload, size_t payloadLength,
                                  ContentFormat contentFormat, int32_t observeValue);
    void sendEmptyAck(uint16_t messageId, const Endpoint& to);
    // Exact match first, then template match. If `params`/`paramCount` are
    // non-null and a template resource matches, the extracted parameters are
    // written there (up to PULSECOAP_MAX_PATH_PARAMS entries).
    int  findResource(const char* path,
                      PathParamEntry* params = nullptr,
                      uint8_t* paramCount = nullptr) const;
    int  allocateDeferred();
    void registerObserver(uint8_t resourceIndex, const Endpoint& remote,
                           const uint8_t* token, uint8_t tokenLen);
    void removeObserver(const Endpoint& remote, const uint8_t* token, uint8_t tokenLen);

    void buildLinkFormat();
    static void wellKnownCoreHandler(const Request& req, Response& res, void* ctx);
    static void separateResendTrampoline(void* ctx, const Endpoint& remote,
                                         const uint8_t* message, size_t messageLen);
    // Called by txPool_ when a CON notification exhausts MAX_RETRANSMIT
    // retries — removes the dead observer so future notify() calls skip it.
    // Also wired as the timeout for separate CON responses (harmless no-op
    // there: no observer token matches a one-shot-request token).
    static void observeTimeoutTrampoline(void* ctx, const Endpoint& remote,
                                         const uint8_t* token, uint8_t tokenLen);

#if PULSECOAP_ENABLE_BLOCKWISE
    // Block2: sends a slice of a large payload with the Block2 option attached.
    void sendBlock2Response(const Message& request, const Endpoint& to, Code code,
                             const uint8_t* fullPayload, size_t fullPayloadLength,
                             ContentFormat contentFormat, int32_t observeValue,
                             uint32_t num, bool more, uint8_t szx);

    // Block1: reassembly session management.
    int  findBlock1Session(const Endpoint& remote, const uint8_t* token, uint8_t tokenLen) const;
    int  allocateBlock1Session(const Endpoint& remote, const uint8_t* token, uint8_t tokenLen);
    void freeBlock1Session(int idx);
#endif

    Transport& transport_;
    Resource resources_[PULSECOAP_MAX_RESOURCES];
    uint8_t resourceCount_ = 0;
    Observer observers_[PULSECOAP_MAX_OBSERVERS];
    DeferredRequest deferred_[PULSECOAP_MAX_DEFERRED];
    TransactionPool txPool_; // retransmits in-flight separate CON responses
    uint32_t lastNowMs_ = 0;
    uint16_t nextMessageId_ = 1;
    uint8_t rxBuffer_[PULSECOAP_MAX_MSG_SIZE];
    char linkFormatBuffer_[PULSECOAP_MAX_LINK_FORMAT_LEN];

#if PULSECOAP_ENABLE_BLOCKWISE
    Block1Session block1sessions_[PULSECOAP_MAX_BLOCK1_SESSIONS];
#endif
};

} // namespace pulsecoap

#endif // PULSECOAP_ENABLE_SERVER
