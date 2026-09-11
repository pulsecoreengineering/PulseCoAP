#include "PulseCoAPServer.h"

#if PULSECOAP_ENABLE_SERVER

#include "PulseCoAPBlockwise.h"
#include "PulseCoAPUri.h"

namespace pulsecoap {

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

bool Server::begin(uint16_t localPort) {
    if (!transport_.begin(localPort)) return false;
    linkFormatBuffer_[0] = '\0';
    // Auto-register RFC 6690 resource discovery. Done here so begin() can
    // still return false cleanly if the transport fails, and so the user's
    // addResource() calls (which come after) don't consume the slot we need.
    addResource("/.well-known/core", MethodGet, &Server::wellKnownCoreHandler, this);
    return true;
}

// ---------------------------------------------------------------------------
// Resource management
// ---------------------------------------------------------------------------

bool Server::addResource(const char* path, uint8_t methods, ResourceHandler handler,
                          void* userContext, bool observable) {
    if (!path || !handler) return false;
    if (resourceCount_ >= PULSECOAP_MAX_RESOURCES) return false;

    Resource& r = resources_[resourceCount_++];
    r.path = path;
    r.methods = methods;
    r.handler = handler;
    r.userContext = userContext;
    r.observable = observable;
    r.isTemplate = (strstr(path, "/:") != nullptr); // any segment starting with ':'
    r.observeSequence = 0;
    r.resourceType = nullptr;
    return true;
}

bool Server::setResourceType(const char* path, const char* rt) {
    int idx = findResource(path);
    if (idx < 0) return false;
    resources_[idx].resourceType = rt;
    return true;
}

// ---------------------------------------------------------------------------
// URI template matching
// ---------------------------------------------------------------------------

// Segment-by-segment template match. Segments starting with ':' in `tmpl`
// are wildcards that capture any non-empty path segment from `path`. Both
// strings must be fully consumed for a match (no partial matches). Captured
// (name, value) pairs are written into `params[0..maxParams-1]`; `count`
// is set to the number of captures. Returns true on a successful match.
static bool matchTemplate(const char* tmpl, const char* path,
                           PathParamEntry* params, uint8_t& count,
                           uint8_t maxParams) {
    count = 0;
    const char* t = tmpl;
    const char* p = path;

    while (*t || *p) {
        // Consume separator '/'
        if (*t == '/') {
            if (*p != '/') return false;
            ++t; ++p;
            continue;
        }
        // Both strings ran out simultaneously — matched.
        if (!*t && !*p) break;
        // One ran out before the other.
        if (!*t || !*p) return false;

        if (*t == ':') {
            // Template parameter: name is t+1 .. next '/' or '\0'
            const char* nameStart = t + 1;
            while (*t && *t != '/') ++t;
            size_t nameLen = static_cast<size_t>(t - nameStart);

            // Value is p .. next '/' or '\0'
            const char* valStart = p;
            while (*p && *p != '/') ++p;
            size_t valLen = static_cast<size_t>(p - valStart);

            if (valLen == 0) return false; // empty segment doesn't match a param

            if (params && count < maxParams) {
                PathParamEntry& e = params[count];
                size_t nCopy = nameLen < PULSECOAP_MAX_PATH_PARAM_LEN - 1
                             ? nameLen : PULSECOAP_MAX_PATH_PARAM_LEN - 1;
                size_t vCopy = valLen  < PULSECOAP_MAX_PATH_PARAM_LEN - 1
                             ? valLen  : PULSECOAP_MAX_PATH_PARAM_LEN - 1;
                memcpy(e.name,  nameStart, nCopy); e.name[nCopy]  = '\0';
                memcpy(e.value, valStart,  vCopy); e.value[vCopy] = '\0';
            }
            ++count;
        } else {
            // Literal segment: must match exactly.
            const char* ts = t;
            const char* ps = p;
            while (*t && *t != '/') ++t;
            while (*p && *p != '/') ++p;
            size_t tLen = static_cast<size_t>(t - ts);
            size_t pLen = static_cast<size_t>(p - ps);
            if (tLen != pLen || memcmp(ts, ps, tLen) != 0) return false;
        }
    }
    return !*t && !*p; // both exhausted → full match
}

int Server::findResource(const char* path,
                          PathParamEntry* params, uint8_t* paramCount) const {
    // Phase 1: exact match (zero-overhead fast path for the common case).
    for (uint8_t i = 0; i < resourceCount_; ++i) {
        if (!resources_[i].isTemplate && strcmp(resources_[i].path, path) == 0) {
            if (paramCount) *paramCount = 0;
            return i;
        }
    }
    // Phase 2: URI template match — first template resource that matches wins.
    for (uint8_t i = 0; i < resourceCount_; ++i) {
        if (!resources_[i].isTemplate) continue;
        PathParamEntry tmp[PULSECOAP_MAX_PATH_PARAMS];
        uint8_t count = 0;
        if (matchTemplate(resources_[i].path, path,
                          params ? tmp : nullptr, count,
                          PULSECOAP_MAX_PATH_PARAMS)) {
            if (params && paramCount) {
                *paramCount = count;
                for (uint8_t j = 0; j < count; ++j) params[j] = tmp[j];
            } else if (paramCount) {
                *paramCount = count;
            }
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// .well-known/core (RFC 6690 CoRE Link Format)
// ---------------------------------------------------------------------------

void Server::buildLinkFormat() {
    linkFormatBuffer_[0] = '\0';
    size_t pos = 0;
    const size_t cap = PULSECOAP_MAX_LINK_FORMAT_LEN;
    bool first = true;

    for (uint8_t i = 0; i < resourceCount_; ++i) {
        const Resource& r = resources_[i];
        if (strcmp(r.path, "/.well-known/core") == 0) continue;

        if (!first) {
            if (pos + 1 >= cap) break;
            linkFormatBuffer_[pos++] = ',';
        }
        first = false;

        size_t pathLen = strlen(r.path);
        if (pos + pathLen + 2 >= cap) break;
        linkFormatBuffer_[pos++] = '<';
        memcpy(&linkFormatBuffer_[pos], r.path, pathLen);
        pos += pathLen;
        linkFormatBuffer_[pos++] = '>';

        if (r.resourceType) {
            size_t rtLen = strlen(r.resourceType);
            if (pos + 6 + rtLen < cap) {
                memcpy(&linkFormatBuffer_[pos], ";rt=\"", 5);
                pos += 5;
                memcpy(&linkFormatBuffer_[pos], r.resourceType, rtLen);
                pos += rtLen;
                linkFormatBuffer_[pos++] = '"';
            }
        }

        if (r.observable) {
            if (pos + 4 < cap) {
                memcpy(&linkFormatBuffer_[pos], ";obs", 4);
                pos += 4;
            }
        }
    }
    linkFormatBuffer_[pos] = '\0';
}

void Server::wellKnownCoreHandler(const Request& /*req*/, Response& res, void* ctx) {
    Server* self = static_cast<Server*>(ctx);
    self->buildLinkFormat();
    res.code = Code::Content;
    res.contentFormat = ContentFormat::LinkFormat;
    res.setPayload(self->linkFormatBuffer_);
}

// ---------------------------------------------------------------------------
// Observe
// ---------------------------------------------------------------------------

void Server::registerObserver(uint8_t resourceIndex, const Endpoint& remote,
                               const uint8_t* token, uint8_t tokenLen) {
    for (uint8_t i = 0; i < PULSECOAP_MAX_OBSERVERS; ++i) {
        Observer& o = observers_[i];
        if (o.active && o.remote == remote && o.tokenLen == tokenLen &&
            memcmp(o.token, token, tokenLen) == 0) {
            o.resourceIndex = resourceIndex;
            return; // refresh in place, no new slot consumed
        }
    }
    for (uint8_t i = 0; i < PULSECOAP_MAX_OBSERVERS; ++i) {
        Observer& o = observers_[i];
        if (!o.active) {
            o.active = true;
            o.resourceIndex = resourceIndex;
            o.remote = remote;
            o.tokenLen = tokenLen;
            memcpy(o.token, token, tokenLen);
            return;
        }
    }
    // Pool full — silently drop; the client's GET still gets an ordinary response.
}

void Server::removeObserver(const Endpoint& remote, const uint8_t* token, uint8_t tokenLen) {
    for (uint8_t i = 0; i < PULSECOAP_MAX_OBSERVERS; ++i) {
        Observer& o = observers_[i];
        if (o.active && o.remote == remote && o.tokenLen == tokenLen &&
            memcmp(o.token, token, tokenLen) == 0) {
            o.active = false;
        }
    }
}

uint8_t Server::observerCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < PULSECOAP_MAX_OBSERVERS; ++i) {
        if (observers_[i].active) count++;
    }
    return count;
}

bool Server::notify(const char* path, const uint8_t* payload, size_t payloadLength,
                     ContentFormat contentFormat, bool confirmable) {
    int idx = findResource(path);
    if (idx < 0 || !resources_[idx].observable) return false;

    resources_[idx].observeSequence++;
    uint32_t seq = resources_[idx].observeSequence;

    for (uint8_t i = 0; i < PULSECOAP_MAX_OBSERVERS; ++i) {
        Observer& o = observers_[i];
        if (!o.active || o.resourceIndex != static_cast<uint8_t>(idx)) continue;

        Message notification;
        notification.setType(confirmable ? MessageType::Confirmable : MessageType::NonConfirmable);
        o.lastSentMessageId = nextMessageId_;
        notification.setMessageId(nextMessageId_++);
        notification.setToken(o.token, o.tokenLen);
        notification.setCode(Code::Content);
        notification.addOptionUint(static_cast<uint16_t>(OptionNumber::Observe), seq);
        notification.addOptionUint(static_cast<uint16_t>(OptionNumber::ContentFormat),
                                    static_cast<uint32_t>(contentFormat));
        notification.setPayload(payload, payloadLength);

        uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
        size_t len = notification.encode(buf, sizeof(buf));
        if (len > 0) {
            transport_.send(o.remote, buf, len);
            if (confirmable) {
                // Track for retransmission; on timeout the observer is removed
                // via observeTimeoutTrampoline — dead-peer detection (RFC 7641 §4.5).
                txPool_.start(o.remote, notification.messageId(),
                              o.token, o.tokenLen, buf, len, lastNowMs_);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Separate (non-piggybacked) responses
// ---------------------------------------------------------------------------

int Server::allocateDeferred() {
    for (uint8_t i = 0; i < PULSECOAP_MAX_DEFERRED; ++i) {
        if (!deferred_[i].active) return i;
    }
    return -1;
}

void Server::separateResendTrampoline(void* ctx, const Endpoint& remote,
                                       const uint8_t* message, size_t messageLen) {
    static_cast<Server*>(ctx)->transport_.send(remote, message, messageLen);
}

void Server::observeTimeoutTrampoline(void* ctx, const Endpoint& remote,
                                       const uint8_t* token, uint8_t tokenLen) {
    // Called when a CON notification (or separate CON response) exhausts
    // MAX_RETRANSMIT with no ACK. For a CON notification the token matches
    // the observer's registration token — remove it (dead peer). For a
    // separate CON response the token is the original request's token; no
    // observer carries that token, so removeObserver() is a harmless no-op.
    static_cast<Server*>(ctx)->removeObserver(remote, token, tokenLen);
}

// ---------------------------------------------------------------------------
// Block-wise transfer helpers (RFC 7959)
// ---------------------------------------------------------------------------

#if PULSECOAP_ENABLE_BLOCKWISE

int Server::findBlock1Session(const Endpoint& remote, const uint8_t* token,
                               uint8_t tokenLen) const {
    for (uint8_t i = 0; i < PULSECOAP_MAX_BLOCK1_SESSIONS; ++i) {
        const Block1Session& s = block1sessions_[i];
        if (!s.active) continue;
        if (!(s.remote == remote)) continue;
        if (s.tokenLen != tokenLen) continue;
        if (memcmp(s.token, token, tokenLen) == 0) return i;
    }
    return -1;
}

int Server::allocateBlock1Session(const Endpoint& remote, const uint8_t* token,
                                   uint8_t tokenLen) {
    for (uint8_t i = 0; i < PULSECOAP_MAX_BLOCK1_SESSIONS; ++i) {
        Block1Session& s = block1sessions_[i];
        if (s.active) continue;
        s.active          = true;
        s.remote          = remote;
        s.tokenLen        = tokenLen;
        memcpy(s.token, token, tokenLen);
        s.nextExpectedNum = 0;
        s.assembledLen    = 0;
        return i;
    }
    return -1;
}

void Server::freeBlock1Session(int idx) {
    if (idx >= 0 && idx < PULSECOAP_MAX_BLOCK1_SESSIONS)
        block1sessions_[idx].active = false;
}

void Server::sendBlock2Response(const Message& request, const Endpoint& to, Code code,
                                 const uint8_t* fullPayload, size_t fullPayloadLength,
                                 ContentFormat contentFormat, int32_t observeValue,
                                 uint32_t num, bool more, uint8_t szx) {
    uint16_t sz     = static_cast<uint16_t>(1u << (szx + 4));
    size_t   offset = static_cast<size_t>(num) * sz;
    if (offset >= fullPayloadLength) return; // caller already checked, but be safe

    size_t remaining = fullPayloadLength - offset;
    size_t blockLen  = (remaining > sz) ? sz : remaining;

    Message response;
    response.setType(request.type() == MessageType::Confirmable
                     ? MessageType::Acknowledgement
                     : MessageType::NonConfirmable);
    response.setMessageId(request.type() == MessageType::Confirmable
                          ? request.messageId()
                          : nextMessageId_++);
    response.setToken(request.token(), request.tokenLength());
    response.setCode(code);

    // Option order (ascending): Observe(6), ContentFormat(12), Block2(23)
    if (observeValue >= 0) {
        response.addOptionUint(static_cast<uint16_t>(OptionNumber::Observe),
                                static_cast<uint32_t>(observeValue));
    }
    response.addOptionUint(static_cast<uint16_t>(OptionNumber::ContentFormat),
                            static_cast<uint32_t>(contentFormat));
    response.addOptionUint(static_cast<uint16_t>(OptionNumber::Block2),
                            BlockOption::toOptionValue(num, more, szx));
    response.setPayload(fullPayload + offset, blockLen);

    uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
    size_t len = response.encode(buf, sizeof(buf));
    if (len > 0) transport_.send(to, buf, len);
}

#endif // PULSECOAP_ENABLE_BLOCKWISE

bool Server::respond(DeferHandle handle, Code code, const uint8_t* payload,
                     size_t payloadLength, ContentFormat contentFormat) {
    if (handle >= PULSECOAP_MAX_DEFERRED) return false;
    DeferredRequest& dr = deferred_[handle];
    if (!dr.active) return false;

    Message response;
    // RFC 7252 §5.2.2: separate response to a CON is itself CON; to a NON
    // the server may send NON (we always do for simplicity).
    response.setType(dr.wasConfirmable ? MessageType::Confirmable
                                       : MessageType::NonConfirmable);
    response.setMessageId(nextMessageId_++);
    response.setToken(dr.token, dr.tokenLen);
    response.setCode(code);
    if (payload && payloadLength > 0) {
        response.addOptionUint(static_cast<uint16_t>(OptionNumber::ContentFormat),
                                static_cast<uint32_t>(contentFormat));
        response.setPayload(payload, payloadLength);
    }

    uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
    size_t len = response.encode(buf, sizeof(buf));
    if (len > 0) {
        transport_.send(dr.remote, buf, len);
        if (dr.wasConfirmable) {
            // Register for retransmission so a lost separate response is retried.
            txPool_.start(dr.remote, response.messageId(),
                          dr.token, dr.tokenLen, buf, len, lastNowMs_);
        }
    }

    dr.active = false;
    return len > 0;
}

// ---------------------------------------------------------------------------
// Wire encoding helpers
// ---------------------------------------------------------------------------

void Server::sendEmptyAck(uint16_t messageId, const Endpoint& to) {
    Message ack;
    ack.setType(MessageType::Acknowledgement);
    ack.setCode(Code::Empty);
    ack.setMessageId(messageId);
    uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
    size_t len = ack.encode(buf, sizeof(buf));
    if (len > 0) transport_.send(to, buf, len);
}

void Server::sendPiggybackedResponse(const Message& request, const Endpoint& to, Code code,
                                      const uint8_t* payload, size_t payloadLength,
                                      ContentFormat contentFormat, int32_t observeValue) {
    Message response;
    response.setType(request.type() == MessageType::Confirmable ? MessageType::Acknowledgement
                                                                   : MessageType::NonConfirmable);
    response.setMessageId(request.type() == MessageType::Confirmable ? request.messageId()
                                                                       : nextMessageId_++);
    response.setToken(request.token(), request.tokenLength());
    response.setCode(code);

    if (observeValue >= 0) {
        response.addOptionUint(static_cast<uint16_t>(OptionNumber::Observe),
                                static_cast<uint32_t>(observeValue));
    }
    if (payload && payloadLength > 0) {
        response.addOptionUint(static_cast<uint16_t>(OptionNumber::ContentFormat),
                                static_cast<uint32_t>(contentFormat));
        response.setPayload(payload, payloadLength);
    }

    uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
    size_t len = response.encode(buf, sizeof(buf));
    if (len > 0) transport_.send(to, buf, len);
}

// ---------------------------------------------------------------------------
// Main dispatch loop
// ---------------------------------------------------------------------------

void Server::handleRequest(const Message& msg, const Endpoint& from) {
    char path[PULSECOAP_MAX_URI_PATH_LEN];
    if (!uri::joinUriPath(msg, path, sizeof(path))) {
        sendPiggybackedResponse(msg, from, Code::BadOption, nullptr, 0, ContentFormat::TextPlain, -1);
        return;
    }

    PathParamEntry params[PULSECOAP_MAX_PATH_PARAMS];
    uint8_t        paramCount = 0;
    int idx = findResource(path, params, &paramCount);
    if (idx < 0) {
        sendPiggybackedResponse(msg, from, Code::NotFound, nullptr, 0, ContentFormat::TextPlain, -1);
        return;
    }
    Resource& resource = resources_[idx];

    uint8_t requestedMask = 0;
    switch (msg.code()) {
        case Code::Get:    requestedMask = MethodGet;    break;
        case Code::Post:   requestedMask = MethodPost;   break;
        case Code::Put:    requestedMask = MethodPut;    break;
        case Code::Delete: requestedMask = MethodDelete; break;
        default:
            sendPiggybackedResponse(msg, from, Code::NotImplemented, nullptr, 0,
                                    ContentFormat::TextPlain, -1);
            return;
    }
    if ((resource.methods & requestedMask) == 0) {
        sendPiggybackedResponse(msg, from, Code::MethodNotAllowed, nullptr, 0,
                                ContentFormat::TextPlain, -1);
        return;
    }

    // RFC 7641: GET with Observe=0 registers, Observe=1 deregisters.
    int32_t observeValueToSend = -1;
    const Option* observeOpt = msg.findOption(static_cast<uint16_t>(OptionNumber::Observe));
    if (resource.observable && msg.code() == Code::Get && observeOpt) {
        uint32_t observeRequest = Message::optionAsUint(*observeOpt);
        if (observeRequest == kObserveRegister) {
            registerObserver(static_cast<uint8_t>(idx), from, msg.token(), msg.tokenLength());
            observeValueToSend = static_cast<int32_t>(resource.observeSequence);
            for (uint8_t i = 0; i < PULSECOAP_MAX_OBSERVERS; ++i) {
                Observer& o = observers_[i];
                if (o.active && o.remote == from && o.tokenLen == msg.tokenLength() &&
                    memcmp(o.token, msg.token(), o.tokenLen) == 0) {
                    o.lastSentMessageId = msg.messageId();
                    break;
                }
            }
        } else {
            removeObserver(from, msg.token(), msg.tokenLength());
        }
    }

#if PULSECOAP_ENABLE_BLOCKWISE
    // -----------------------------------------------------------------
    // Block1: client is uploading a large payload in pieces (RFC 7959).
    // Handle before the normal deferred/handler path.
    // -----------------------------------------------------------------
    const Option* block1Opt = msg.findOption(static_cast<uint16_t>(OptionNumber::Block1));
    if (block1Opt && (msg.code() == Code::Put || msg.code() == Code::Post)) {
        BlockOption b1;
        if (!BlockOption::fromOption(*block1Opt, b1)) {
            sendPiggybackedResponse(msg, from, Code::BadOption,
                                    nullptr, 0, ContentFormat::TextPlain, -1);
            return;
        }

        int sIdx = findBlock1Session(from, msg.token(), msg.tokenLength());
        if (sIdx < 0) {
            if (b1.num != 0) {
                // We missed earlier blocks.
                sendPiggybackedResponse(msg, from, Code::RequestEntityIncomplete,
                                        nullptr, 0, ContentFormat::TextPlain, -1);
                return;
            }
            sIdx = allocateBlock1Session(from, msg.token(), msg.tokenLength());
            if (sIdx < 0) {
                sendPiggybackedResponse(msg, from, Code::ServiceUnavailable,
                                        nullptr, 0, ContentFormat::TextPlain, -1);
                return;
            }
        }

        Block1Session& sess = block1sessions_[sIdx];
        if (b1.num != sess.nextExpectedNum) {
            // Duplicate or out-of-order; echo the last-seen block to let the
            // client know where we are without resetting the session.
            Message cont;
            cont.setType(msg.type() == MessageType::Confirmable
                         ? MessageType::Acknowledgement : MessageType::NonConfirmable);
            cont.setMessageId(msg.type() == MessageType::Confirmable
                              ? msg.messageId() : nextMessageId_++);
            cont.setToken(msg.token(), msg.tokenLength());
            cont.setCode(Code::Continue);
            cont.addOptionUint(static_cast<uint16_t>(OptionNumber::Block1),
                               BlockOption::toOptionValue(sess.nextExpectedNum - 1,
                                                          true, b1.szx));
            uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
            size_t len = cont.encode(buf, sizeof(buf));
            if (len > 0) transport_.send(from, buf, len);
            return;
        }

        size_t blockLen = msg.payloadLength();
        if (sess.assembledLen + blockLen > PULSECOAP_BLOCK1_MAX_BODY) {
            freeBlock1Session(sIdx);
            sendPiggybackedResponse(msg, from, Code::RequestEntityTooLarge,
                                    nullptr, 0, ContentFormat::TextPlain, -1);
            return;
        }
        memcpy(sess.buffer + sess.assembledLen, msg.payload(), blockLen);
        sess.assembledLen += static_cast<uint32_t>(blockLen);
        sess.nextExpectedNum++;

        if (b1.m) {
            // More blocks to come — respond with 2.31 Continue.
            Message cont;
            cont.setType(msg.type() == MessageType::Confirmable
                         ? MessageType::Acknowledgement : MessageType::NonConfirmable);
            cont.setMessageId(msg.type() == MessageType::Confirmable
                              ? msg.messageId() : nextMessageId_++);
            cont.setToken(msg.token(), msg.tokenLength());
            cont.setCode(Code::Continue);
            cont.addOptionUint(static_cast<uint16_t>(OptionNumber::Block1),
                               BlockOption::toOptionValue(b1.num, false, b1.szx));
            uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
            size_t len = cont.encode(buf, sizeof(buf));
            if (len > 0) transport_.send(from, buf, len);
            return;
        }

        // Final block received — fall through to the normal handler path,
        // but with the assembled payload injected via req.block1Body.
        // The session is freed once the handler has run.
        int deferIdx2 = allocateDeferred();
        if (deferIdx2 >= 0) {
            DeferredRequest& dr = deferred_[deferIdx2];
            dr.active = true;
            dr.remote = from;
            dr.tokenLen = msg.tokenLength();
            memcpy(dr.token, msg.token(), dr.tokenLen);
            dr.wasConfirmable = (msg.type() == MessageType::Confirmable);
        }

        Request req;
        req.method          = msg.code();
        req.message         = &msg;
        req.remote          = from;
        req.deferHandle     = (deferIdx2 >= 0) ? static_cast<DeferHandle>(deferIdx2)
                                               : kInvalidDeferHandle;
        req.block1Body       = sess.buffer;
        req.block1BodyLength = sess.assembledLen;
        req.paramCount_      = paramCount;
        for (uint8_t pi = 0; pi < paramCount; ++pi) req.params_[pi] = params[pi];

        Response res;
        resource.handler(req, res, resource.userContext);
        freeBlock1Session(sIdx);

        if (res.deferred && deferIdx2 >= 0) {
            if (msg.type() == MessageType::Confirmable)
                sendEmptyAck(msg.messageId(), from);
            return;
        }
        if (deferIdx2 >= 0) deferred_[deferIdx2].active = false;
        sendPiggybackedResponse(msg, from, res.code, res.payload, res.payloadLength,
                                 res.contentFormat, observeValueToSend);
        return;
    }
#endif // PULSECOAP_ENABLE_BLOCKWISE

    // Allocate a deferred slot upfront. If the handler doesn't set
    // res.deferred, we free it again immediately after — zero waste.
    int deferIdx = allocateDeferred();
    if (deferIdx >= 0) {
        DeferredRequest& dr = deferred_[deferIdx];
        dr.active = true;
        dr.remote = from;
        dr.tokenLen = msg.tokenLength();
        memcpy(dr.token, msg.token(), dr.tokenLen);
        dr.wasConfirmable = (msg.type() == MessageType::Confirmable);
    }

    Request req;
    req.method = msg.code();
    req.message = &msg;
    req.remote = from;
    req.deferHandle = (deferIdx >= 0) ? static_cast<DeferHandle>(deferIdx) : kInvalidDeferHandle;
    req.paramCount_ = paramCount;
    for (uint8_t pi = 0; pi < paramCount; ++pi) req.params_[pi] = params[pi];

    Response res;
    resource.handler(req, res, resource.userContext);

    if (res.deferred && deferIdx >= 0) {
        // Handler claimed the slot — send empty ACK for CON to stop retransmission.
        if (msg.type() == MessageType::Confirmable) {
            sendEmptyAck(msg.messageId(), from);
        }
        // Leave deferred_[deferIdx].active = true; respond() will free it later.
        return;
    }

    // Not deferred (or deferred pool was full): free the slot and reply inline.
    if (deferIdx >= 0) deferred_[deferIdx].active = false;

#if PULSECOAP_ENABLE_BLOCKWISE
    // Block2: if the response payload is larger than one block, fragment it.
    if (res.payload && res.payloadLength > 0) {
        const Option* block2Req = msg.findOption(static_cast<uint16_t>(OptionNumber::Block2));
        BlockOption b2;
        bool hasB2Req = block2Req && BlockOption::fromOption(*block2Req, b2);
        uint8_t szx = hasB2Req ? b2.szx : static_cast<uint8_t>(PULSECOAP_BLOCK_SZX);
        uint16_t sz = static_cast<uint16_t>(1u << (szx + 4));

        uint32_t num = hasB2Req ? b2.num : 0;
        size_t offset = static_cast<size_t>(num) * sz;

        if (res.payloadLength > sz || hasB2Req) {
            // Need block-wise. Validate the requested block is in range.
            if (offset >= res.payloadLength) {
                sendPiggybackedResponse(msg, from, Code::BadRequest,
                                        nullptr, 0, ContentFormat::TextPlain, -1);
                return;
            }
            size_t remaining = res.payloadLength - offset;
            bool more = (remaining > sz);
            sendBlock2Response(msg, from, res.code, res.payload, res.payloadLength,
                               res.contentFormat, observeValueToSend, num, more, szx);
            return;
        }
    }
#endif // PULSECOAP_ENABLE_BLOCKWISE

    sendPiggybackedResponse(msg, from, res.code, res.payload, res.payloadLength,
                             res.contentFormat, observeValueToSend);
}

void Server::poll(uint32_t nowMs) {
    lastNowMs_ = nowMs;

    // Drive retransmission of in-flight CON messages (separate responses and
    // CON notifications). On timeout, observeTimeoutTrampoline removes dead
    // observers; for separate responses it is a harmless no-op.
    txPool_.tick(nowMs, &Server::separateResendTrampoline,
                 &Server::observeTimeoutTrampoline, this);

    Endpoint from;
    size_t len = transport_.receive(rxBuffer_, sizeof(rxBuffer_), from);
    if (len == 0) return;

    Message msg;
    if (msg.decode(rxBuffer_, len) != DecodeError::None) return;

    if (msg.type() == MessageType::Reset) {
        uint16_t rstId = msg.messageId();
        for (uint8_t i = 0; i < PULSECOAP_MAX_OBSERVERS; ++i) {
            Observer& o = observers_[i];
            if (o.active && o.remote == from && o.lastSentMessageId == rstId) {
                o.active = false;
                break;
            }
        }
        return;
    }

    if (msg.type() == MessageType::Acknowledgement) {
        // ACK for a separate CON response we sent.
        txPool_.complete(from, msg.messageId());
        return;
    }

    if (isMethodCode(msg.code())) {
        handleRequest(msg, from);
    }
}

} // namespace pulsecoap

#endif // PULSECOAP_ENABLE_SERVER
