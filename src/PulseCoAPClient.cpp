#include "PulseCoAPClient.h"

#if PULSECOAP_ENABLE_CLIENT

#include <string.h>

#include "PulseCoAPBlockwise.h"
#include "PulseCoAPUri.h"

namespace pulsecoap {

int Client::allocatePending() {
    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        if (!pending_[i].active) {
            pending_[i] = PendingRequest();
            pending_[i].active = true;
            return i;
        }
    }
    return -1;
}

int Client::findPendingByToken(const uint8_t* token, uint8_t tokenLen) const {
    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        const PendingRequest& p = pending_[i];
        if (p.active && p.tokenLen == tokenLen && memcmp(p.token, token, tokenLen) == 0) return i;
    }
    return -1;
}

uint8_t Client::nextToken() {
    return nextTokenByte_++;
}

bool Client::sendRequest(const Endpoint& server, const char* path, Code method,
                          const uint8_t* payload, size_t payloadLength, ContentFormat contentFormat,
                          bool confirmable, bool observeRegister, bool observeDeregister,
                          ResponseHandler onResponse, void* userContext, bool markObserving) {
    int slot = allocatePending();
    if (slot < 0) return false;

    uint8_t token = nextToken();

    Message req;
    req.setType(confirmable ? MessageType::Confirmable : MessageType::NonConfirmable);
    req.setMessageId(nextMessageId_++);
    req.setToken(&token, 1);
    req.setCode(method);

    // Ascending option-number order: Observe(6) < Uri-Path(11) < Content-Format(12).
    if (observeRegister) {
        req.addOptionUint(static_cast<uint16_t>(OptionNumber::Observe), kObserveRegister);
    } else if (observeDeregister) {
        req.addOptionUint(static_cast<uint16_t>(OptionNumber::Observe), kObserveDeregister);
    }

    if (!uri::addUriPathOptions(req, path)) {
        pending_[slot].active = false;
        return false;
    }

    if (payload && payloadLength > 0) {
        req.addOptionUint(static_cast<uint16_t>(OptionNumber::ContentFormat),
                           static_cast<uint32_t>(contentFormat));
        req.setPayload(payload, payloadLength);
    }

    uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
    size_t len = req.encode(buf, sizeof(buf));
    if (len == 0) {
        pending_[slot].active = false;
        return false;
    }

    if (!transport_.send(server, buf, len)) {
        pending_[slot].active = false;
        return false;
    }

    if (confirmable) {
        transactions_.start(server, req.messageId(), &token, 1, buf, len, lastNowMs_);
    }

    PendingRequest& p = pending_[slot];
    p.server = server;
    p.token[0] = token;
    p.tokenLen = 1;
    p.onResponse = onResponse;
    p.userContext = userContext;
    p.observing = markObserving;
    {
        size_t plen = strlen(path);
        if (plen < PULSECOAP_MAX_URI_PATH_LEN)
            memcpy(p.path, path, plen + 1);
        else
            p.path[0] = '\0';
    }

#if PULSECOAP_ENABLE_BLOCKWISE
    block1up_[slot].active   = false;
    block2recv_[slot].active = false;
    // Pre-store path so Block2 follow-up GETs can re-use it.
    {
        size_t plen = strlen(path);
        if (plen < PULSECOAP_MAX_URI_PATH_LEN)
            memcpy(block2recv_[slot].path, path, plen + 1);
        else
            block2recv_[slot].path[0] = '\0';
    }
#endif
    return true;
}

#if PULSECOAP_ENABLE_BLOCKWISE

bool Client::sendBlock1(int slot) {
    Block1UploadState& up = block1up_[slot];
    uint16_t sz = static_cast<uint16_t>(1u << (up.szx + 4));
    size_t offset = static_cast<size_t>(up.nextNum) * sz;
    if (offset >= up.fullLen) return false;

    size_t remaining = up.fullLen - offset;
    size_t blockLen  = (remaining > sz) ? sz : remaining;
    bool   more      = (remaining > sz);

    Message req;
    req.setType(MessageType::Confirmable); // always CON for reliability
    req.setMessageId(nextMessageId_++);
    req.setToken(&up.token, 1);
    req.setCode(up.method);

    if (!uri::addUriPathOptions(req, up.path)) return false;

    req.addOptionUint(static_cast<uint16_t>(OptionNumber::ContentFormat),
                      static_cast<uint32_t>(up.contentFormat));
    req.addOptionUint(static_cast<uint16_t>(OptionNumber::Block1),
                      BlockOption::toOptionValue(up.nextNum, more, up.szx));
    req.setPayload(up.fullPayload + offset, blockLen);

    uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
    size_t len = req.encode(buf, sizeof(buf));
    if (len == 0) return false;

    if (!transport_.send(up.server, buf, len)) return false;

    transactions_.start(up.server, req.messageId(), &up.token, 1, buf, len, lastNowMs_);
    up.waitingForContinue = true;
    return true;
}

bool Client::sendBlock2Request(int slot) {
    Block2RecvState& recv = block2recv_[slot];
    PendingRequest&  p    = pending_[slot];

    Message req;
    req.setType(MessageType::Confirmable);
    req.setMessageId(nextMessageId_++);
    req.setToken(p.token, p.tokenLen);
    req.setCode(Code::Get);

    if (!uri::addUriPathOptions(req, recv.path)) return false;

    // Option order: Block2(23) > UriPath(11) but we must add in ascending order.
    // UriPath was just added above; Block2 is 23 > 11, so it goes after.
    req.addOptionUint(static_cast<uint16_t>(OptionNumber::Block2),
                      BlockOption::toOptionValue(recv.nextNum, false, recv.szx));

    uint8_t buf[PULSECOAP_MAX_MSG_SIZE];
    size_t len = req.encode(buf, sizeof(buf));
    if (len == 0) return false;

    if (!transport_.send(recv.server, buf, len)) return false;

    transactions_.start(recv.server, req.messageId(), p.token, p.tokenLen, buf, len, lastNowMs_);
    return true;
}

#endif // PULSECOAP_ENABLE_BLOCKWISE

bool Client::get(const Endpoint& server, const char* path, ResponseHandler onResponse,
                  void* userContext, bool confirmable) {
    return sendRequest(server, path, Code::Get, nullptr, 0, ContentFormat::TextPlain,
                        confirmable, false, false, onResponse, userContext, false);
}

bool Client::put(const Endpoint& server, const char* path, const uint8_t* payload, size_t payloadLength,
                  ContentFormat contentFormat, ResponseHandler onResponse, void* userContext,
                  bool confirmable) {
#if PULSECOAP_ENABLE_BLOCKWISE
    uint16_t sz = static_cast<uint16_t>(1u << (PULSECOAP_BLOCK_SZX + 4));
    if (payloadLength > sz) {
        int slot = allocatePending();
        if (slot < 0) return false;
        uint8_t tok = nextToken();
        pending_[slot].server      = server;
        pending_[slot].token[0]    = tok;
        pending_[slot].tokenLen    = 1;
        pending_[slot].onResponse  = onResponse;
        pending_[slot].userContext = userContext;
        pending_[slot].observing   = false;

        Block1UploadState& up = block1up_[slot];
        up.active        = true;
        up.fullPayload   = payload;
        up.fullLen       = payloadLength;
        up.nextNum       = 0;
        up.szx           = PULSECOAP_BLOCK_SZX;
        up.contentFormat = contentFormat;
        up.method        = Code::Put;
        up.server        = server;
        up.token         = tok;
        size_t pathLen = strlen(path);
        if (pathLen >= PULSECOAP_MAX_URI_PATH_LEN) { pending_[slot].active = false; up.active = false; return false; }
        memcpy(up.path, path, pathLen + 1);
        memcpy(pending_[slot].path, path, pathLen + 1);
        block2recv_[slot].active = false;

        if (!sendBlock1(slot)) {
            pending_[slot].active = false;
            up.active = false;
            return false;
        }
        return true;
    }
#endif
    return sendRequest(server, path, Code::Put, payload, payloadLength, contentFormat,
                        confirmable, false, false, onResponse, userContext, false);
}

bool Client::post(const Endpoint& server, const char* path, const uint8_t* payload, size_t payloadLength,
                   ContentFormat contentFormat, ResponseHandler onResponse, void* userContext,
                   bool confirmable) {
#if PULSECOAP_ENABLE_BLOCKWISE
    uint16_t sz = static_cast<uint16_t>(1u << (PULSECOAP_BLOCK_SZX + 4));
    if (payloadLength > sz) {
        int slot = allocatePending();
        if (slot < 0) return false;
        uint8_t tok = nextToken();
        pending_[slot].server      = server;
        pending_[slot].token[0]    = tok;
        pending_[slot].tokenLen    = 1;
        pending_[slot].onResponse  = onResponse;
        pending_[slot].userContext = userContext;
        pending_[slot].observing   = false;

        Block1UploadState& up = block1up_[slot];
        up.active        = true;
        up.fullPayload   = payload;
        up.fullLen       = payloadLength;
        up.nextNum       = 0;
        up.szx           = PULSECOAP_BLOCK_SZX;
        up.contentFormat = contentFormat;
        up.method        = Code::Post;
        up.server        = server;
        up.token         = tok;
        size_t pathLen = strlen(path);
        if (pathLen >= PULSECOAP_MAX_URI_PATH_LEN) { pending_[slot].active = false; up.active = false; return false; }
        memcpy(up.path, path, pathLen + 1);
        memcpy(pending_[slot].path, path, pathLen + 1);
        block2recv_[slot].active = false;

        if (!sendBlock1(slot)) {
            pending_[slot].active = false;
            up.active = false;
            return false;
        }
        return true;
    }
#endif
    return sendRequest(server, path, Code::Post, payload, payloadLength, contentFormat,
                        confirmable, false, false, onResponse, userContext, false);
}

bool Client::del(const Endpoint& server, const char* path, ResponseHandler onResponse,
                  void* userContext, bool confirmable) {
    return sendRequest(server, path, Code::Delete, nullptr, 0, ContentFormat::TextPlain,
                        confirmable, false, false, onResponse, userContext, false);
}

bool Client::observe(const Endpoint& server, const char* path, ResponseHandler onResponse,
                      void* userContext) {
    return sendRequest(server, path, Code::Get, nullptr, 0, ContentFormat::TextPlain,
                        /*confirmable=*/true, /*observeRegister=*/true, /*observeDeregister=*/false,
                        onResponse, userContext, /*markObserving=*/true);
}

bool Client::cancelObserve(const Endpoint& server, const char* path) {
    for (uint8_t i = 0; i < PULSECOAP_MAX_TRANSACTIONS; ++i) {
        PendingRequest& p = pending_[i];
        if (p.active && p.observing && p.server == server && strcmp(p.path, path) == 0) {
            ResponseHandler handler = p.onResponse;
            void* ctx = p.userContext;
            p.active = false; // free the observing slot; the deregister GET gets its own token/slot
            return sendRequest(server, path, Code::Get, nullptr, 0, ContentFormat::TextPlain,
                                true, false, /*observeDeregister=*/true, handler, ctx, false);
        }
    }
    return false;
}

void Client::resendTrampoline(void* ctx, const Endpoint& remote, const uint8_t* message, size_t messageLen) {
    auto* self = static_cast<Client*>(ctx);
    self->transport_.send(remote, message, messageLen);
}

void Client::timeoutTrampoline(void* ctx, const Endpoint& remote, const uint8_t* token, uint8_t tokenLen) {
    (void)remote;
    auto* self = static_cast<Client*>(ctx);
    int slot = self->findPendingByToken(token, tokenLen);
    void* userContext = (slot >= 0) ? self->pending_[slot].userContext : nullptr;
    if (slot >= 0) self->pending_[slot].active = false;
    if (self->onTimeout_) self->onTimeout_(userContext);
}

void Client::poll(uint32_t nowMs) {
    lastNowMs_ = nowMs;

    Endpoint from;
    size_t len = transport_.receive(rxBuffer_, sizeof(rxBuffer_), from);
    if (len > 0) {
        Message msg;
        if (msg.decode(rxBuffer_, len) == DecodeError::None) {
            // RST: the remote rejected a message we sent. Complete the
            // transaction immediately (no further retries) and notify the
            // caller via TimeoutHandler. getToken() must come first — the
            // slot is freed by complete() and the token would be lost.
            if (msg.type() == MessageType::Reset) {
                uint8_t rstToken[PULSECOAP_MAX_TOKEN_LEN];
                uint8_t rstTokenLen = 0;
                transactions_.getToken(from, msg.messageId(), rstToken, rstTokenLen);
                transactions_.complete(from, msg.messageId());
                if (rstTokenLen > 0) {
                    int slot = findPendingByToken(rstToken, rstTokenLen);
                    if (slot >= 0) {
                        void* ctx = pending_[slot].userContext;
                        pending_[slot].active = false;
                        if (onTimeout_) onTimeout_(ctx);
                    }
                }
                return;
            }

            if (msg.type() == MessageType::Acknowledgement) {
                transactions_.complete(from, msg.messageId());
            }

            // A piggybacked response (ACK/NON carrying a response code) or
            // a notification both carry the original request's token.
            // A CON separate response (RFC 7252 §5.2.2) also arrives here —
            // ACK it immediately so the server stops retransmitting it, even
            // if the local pending slot has already been freed.
            if (msg.code() != Code::Empty && !isMethodCode(msg.code())) {
                if (msg.type() == MessageType::Confirmable) {
                    Message ack;
                    ack.setType(MessageType::Acknowledgement);
                    ack.setCode(Code::Empty);
                    ack.setMessageId(msg.messageId());
                    uint8_t ackBuf[PULSECOAP_MAX_MSG_SIZE];
                    size_t ackLen = ack.encode(ackBuf, sizeof(ackBuf));
                    if (ackLen > 0) transport_.send(from, ackBuf, ackLen);
                }
                int slot = findPendingByToken(msg.token(), msg.tokenLength());
                if (slot >= 0) {
                    PendingRequest& p = pending_[slot];

#if PULSECOAP_ENABLE_BLOCKWISE
                    // --- Block1: server sent 2.31 Continue → send next block ---
                    if (msg.code() == Code::Continue && block1up_[slot].active) {
                        Block1UploadState& up = block1up_[slot];
                        up.nextNum++;        // advance to the block just ack'd + 1
                        up.waitingForContinue = false;
                        if (!sendBlock1(slot)) {
                            // Upload failed partway; surface as timeout
                            up.active = false;
                            p.active  = false;
                            if (onTimeout_) onTimeout_(p.userContext);
                        }
                        // Don't fire onResponse yet; wait for the final response.
                        goto next_poll; // skip normal response dispatch
                    }

                    // --- Block1 upload done: final response from server ---
                    if (block1up_[slot].active) {
                        block1up_[slot].active = false;
                        // Fall through to fire onResponse normally.
                    }

                    // --- Block2: server fragmented the response ---
                    {
                        const Option* b2opt = msg.findOption(
                            static_cast<uint16_t>(OptionNumber::Block2));
                        if (b2opt) {
                            BlockOption b2;
                            if (BlockOption::fromOption(*b2opt, b2)) {
                                Block2RecvState& recv = block2recv_[slot];
                                if (!recv.active) {
                                    // First block — initialise the receive state.
                                    // recv.path was already stored by sendRequest().
                                    recv.active       = true;
                                    recv.nextNum      = 0;
                                    recv.szx          = b2.szx;
                                    recv.assembledLen = 0;
                                    recv.server       = from;
                                    const Option* cf = msg.findOption(
                                        static_cast<uint16_t>(OptionNumber::ContentFormat));
                                    recv.contentFormat = cf
                                        ? static_cast<ContentFormat>(Message::optionAsUint(*cf))
                                        : ContentFormat::TextPlain;
                                }

                                if (b2.num == recv.nextNum) {
                                    // Accumulate this block.
                                    size_t avail = PULSECOAP_BLOCK2_MAX_BODY - recv.assembledLen;
                                    size_t copyLen = msg.payloadLength() < avail
                                                     ? msg.payloadLength() : avail;
                                    memcpy(recv.buffer + recv.assembledLen,
                                           msg.payload(), copyLen);
                                    recv.assembledLen += static_cast<uint32_t>(copyLen);
                                    recv.nextNum++;
                                }

                                if (b2.m) {
                                    // More blocks to come — request the next one.
                                    sendBlock2Request(slot);
                                    goto next_poll;
                                }

                                // Final block — fire callback with full assembled payload.
                                if (p.onResponse) {
                                    ClientResponse res;
                                    res.code          = msg.code();
                                    res.payload       = recv.buffer;
                                    res.payloadLength = recv.assembledLen;
                                    res.contentFormat = recv.contentFormat;
                                    p.onResponse(res, p.userContext);
                                }
                                recv.active = false;
                                if (!p.observing) p.active = false;
                                goto next_poll;
                            }
                        }
                    }
#endif // PULSECOAP_ENABLE_BLOCKWISE

                    if (p.onResponse) {
                        ClientResponse res;
                        res.code = msg.code();
                        res.payload = msg.payload();
                        res.payloadLength = msg.payloadLength();
                        const Option* cf = msg.findOption(static_cast<uint16_t>(OptionNumber::ContentFormat));
                        res.contentFormat = cf ? static_cast<ContentFormat>(Message::optionAsUint(*cf))
                                                : ContentFormat::TextPlain;
                        p.onResponse(res, p.userContext);
                    }
                    if (!p.observing) p.active = false; // one-shot request done; an observe stays registered
                }
#if PULSECOAP_ENABLE_BLOCKWISE
                next_poll:;
#endif
            }
        }
    }

    transactions_.tick(nowMs, &Client::resendTrampoline, &Client::timeoutTrampoline, this);
}

} // namespace pulsecoap

#endif // PULSECOAP_ENABLE_CLIENT
