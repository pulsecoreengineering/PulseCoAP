// PulseCoAPTransportDTLS.h
// DTLS 1.2 transport adapter for PulseCoAP (RFC 6347 / RFC 7252 §9).
//
// Architecture overview
// ---------------------
// This adapter wraps a caller-owned Arduino UDP object (WiFiUDP on ESP32) and
// runs a fixed-size DTLS session table on top of it.  Upper layers —
// PulseCoAPClient and PulseCoAPServer — continue to call the same three-method
// Transport interface: begin(), send(), receive().
//
// Non-blocking handshake strategy
// --------------------------------
// DTLS handshakes are multi-round-trip.  We never block loop() waiting for
// them to complete.  Instead:
//
//   1. The first send() to a new peer allocates a session slot, starts the
//      handshake, buffers the outgoing CoAP packet, and returns FALSE.
//      PulseCoAP's CON retransmit engine will retry after ACK_TIMEOUT_MS (2 s).
//
//   2. Every receive() call drives whichever sessions have incoming data
//      (mbedtls_ssl_handshake is called until it returns 0 or a hard error).
//
//   3. On the next send() retry, if the session is now ESTABLISHED, the
//      packet goes through mbedtls_ssl_write.  If still HANDSHAKING, the
//      buffer is refreshed with the retransmit's bytes and we return FALSE
//      again, driving more handshake steps if incoming data is waiting.
//
// PSK key store
// -------------
// Pre-shared keys (RFC 4279) — no x.509 certificates needed.  Call
// addPsk(identity, key, keyLen) before begin().  For a server, add one
// entry per authorised client.  For a client, add the one entry that
// matches the key configured on the server.
//
// Memory note
// -----------
// PulseCoAP's own protocol code (message codec, routing, observer tracking,
// retransmission) allocates nothing from the heap.  This adapter wraps
// mbedTLS, which DOES use the heap for SSL I/O buffers (the Arduino ESP32
// pre-built SDK ships with CONFIG_MBEDTLS_DYNAMIC_BUFFER=y).  Expect
// ~7–12 KB of heap per active DTLS session during handshake, dropping to
// ~3–5 KB at idle.  The PulseCoAP protocol layer itself stays zero-heap.
//
// Prerequisite: The Arduino ESP32 core's mbedTLS is built with both
// CONFIG_MBEDTLS_SSL_PROTO_DTLS=y and CONFIG_MBEDTLS_KEY_EXCHANGE_PSK=y
// (confirmed in esp32-arduino-lib-builder/configs/defconfig.common, all
// releases).  No custom sdkconfig or lib-builder patch is required.
//
// Only compiled when building under the Arduino framework with
// PULSECOAP_ENABLE_DTLS=1.
#pragma once

#if defined(ARDUINO) && PULSECOAP_ENABLE_DTLS

#include <Udp.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ssl_cookie.h>
#include <mbedtls/net_sockets.h>  // MBEDTLS_ERR_SSL_WANT_READ, WANT_WRITE
#include <Arduino.h>              // millis()

#include "PulseCoAPConfig.h"
#include "PulseCoAPTransport.h"

namespace pulsecoap {

// ---------------------------------------------------------------------------
// PskEntry — one (identity, secret) pair in the static key store
// ---------------------------------------------------------------------------
struct PskEntry {
    char    identity[PULSECOAP_DTLS_IDENTITY_MAX_LEN] = {};
    uint8_t key[PULSECOAP_DTLS_PSK_MAX_LEN]           = {};
    uint8_t keyLen = 0;
    bool    used   = false;
};

// ---------------------------------------------------------------------------
// DtlsTransport
// ---------------------------------------------------------------------------
class DtlsTransport : public Transport {
public:
    // server=true  — expect clients to initiate handshakes (enables DTLS cookie).
    // server=false — this node initiates handshakes outbound (client mode).
    explicit DtlsTransport(UDP& udp, bool server = false)
        : udp_(udp), isServer_(server)
    {
        memset(sessions_,  0, sizeof(sessions_));
        memset(pskStore_,  0, sizeof(pskStore_));
    }

    ~DtlsTransport() {
        for (auto& s : sessions_) {
            if (s.state != Session::IDLE) mbedtls_ssl_free(&s.ssl);
        }
        mbedtls_ssl_config_free(&conf_);
        mbedtls_ctr_drbg_free(&ctrDrbg_);
        mbedtls_entropy_free(&entropy_);
        if (isServer_) mbedtls_ssl_cookie_free(&cookieCtx_);
    }

    // -----------------------------------------------------------------------
    // PSK key store
    // -----------------------------------------------------------------------

    // Add a PSK entry (raw key bytes). Call before begin().
    // Returns false if the store is full or an argument is invalid.
    bool addPsk(const char* identity, const uint8_t* key, size_t keyLen) {
        if (!identity || !key || keyLen == 0 ||
            keyLen > PULSECOAP_DTLS_PSK_MAX_LEN) return false;
        size_t idLen = strlen(identity);
        if (idLen == 0 || idLen >= PULSECOAP_DTLS_IDENTITY_MAX_LEN) return false;
        for (auto& e : pskStore_) {
            if (e.used) continue;
            memcpy(e.identity, identity, idLen + 1);
            memcpy(e.key,      key,      keyLen);
            e.keyLen = static_cast<uint8_t>(keyLen);
            e.used   = true;
            return true;
        }
        return false; // store full
    }

    // Convenience overload for a passphrase string.
    bool addPsk(const char* identity, const char* passphrase) {
        if (!passphrase) return false;
        return addPsk(identity,
                      reinterpret_cast<const uint8_t*>(passphrase),
                      strlen(passphrase));
    }

    // -----------------------------------------------------------------------
    // Transport interface
    // -----------------------------------------------------------------------

    // One-time setup: initialise mbedTLS and bind the UDP socket.
    // CoAP-over-DTLS standard port is 5684; pass that from the sketch.
    bool begin(uint16_t localPort) override {
        mbedtls_entropy_init(&entropy_);
        mbedtls_ctr_drbg_init(&ctrDrbg_);
        mbedtls_ssl_config_init(&conf_);

        const char* pers = "pulsecoap_dtls";
        if (mbedtls_ctr_drbg_seed(&ctrDrbg_, mbedtls_entropy_func,
                                   &entropy_,
                                   reinterpret_cast<const unsigned char*>(pers),
                                   strlen(pers)) != 0) return false;

        int endpoint = isServer_ ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT;
        if (mbedtls_ssl_config_defaults(&conf_, endpoint,
                                        MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0)
            return false;

        mbedtls_ssl_conf_rng(&conf_, mbedtls_ctr_drbg_random, &ctrDrbg_);

        if (isServer_) {
            mbedtls_ssl_cookie_init(&cookieCtx_);
            if (mbedtls_ssl_cookie_setup(&cookieCtx_,
                                          mbedtls_ctr_drbg_random,
                                          &ctrDrbg_) != 0) return false;
            mbedtls_ssl_conf_dtls_cookies(&conf_,
                                           mbedtls_ssl_cookie_write,
                                           mbedtls_ssl_cookie_check,
                                           &cookieCtx_);
            // Server-side PSK lookup callback: resolves per-client key.
            mbedtls_ssl_conf_psk_cb(&conf_, sPskCallback, this);
        } else {
            // Client: configure the single PSK to use for outgoing connections.
            PskEntry* e = firstPsk();
            if (!e) return false;
            if (mbedtls_ssl_conf_psk(
                    &conf_,
                    e->key, e->keyLen,
                    reinterpret_cast<const unsigned char*>(e->identity),
                    strlen(e->identity)) != 0) return false;
        }

        localPort_ = localPort;
        return udp_.begin(localPort) != 0;
    }

    // Encrypt and send one CoAP message to `to`.
    // Returns false while the handshake is in progress — PulseCoAP's CON
    // retransmit engine will call send() again after ACK_TIMEOUT_MS.
    bool send(const Endpoint& to, const uint8_t* data, size_t length) override {
        Session* s = findSession(to);

        if (!s) {
            // First contact with this peer — allocate a session and start hs.
            s = allocSession(to);
            if (!s) return false;
            if (!initSession(*s)) { resetSession(*s); return false; }
            bufferTx(*s, data, length);
            currentSession_ = s;
            driveHandshake(*s);
            currentSession_ = nullptr;
            return false; // handshake in progress
        }

        switch (s->state) {
        case Session::HANDSHAKING:
            // Keep the most-recent packet buffered; drive any available hs steps.
            bufferTx(*s, data, length);
            currentSession_ = s;
            driveHandshake(*s);
            if (s->state == Session::ESTABLISHED && s->txPending) {
                bool ok = sslWrite(*s, s->txBuf, s->txLen);
                s->txPending = false;
                currentSession_ = nullptr;
                return ok;
            }
            currentSession_ = nullptr;
            return false;

        case Session::ESTABLISHED:
            currentSession_ = s;
            {
                bool ok = sslWrite(*s, data, length);
                currentSession_ = nullptr;
                return ok;
            }

        case Session::FAILED:
            // Previous attempt failed — reclaim the slot and try again next call.
            resetSession(*s);
            return false;

        default:
            return false;
        }
    }

    // Non-blocking receive: drives in-progress handshakes and returns a
    // decrypted CoAP datagram if one is ready.
    size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) override {
        tickTimeouts();

        int packetSize = udp_.parsePacket();
        if (packetSize <= 0) return 0;

        // Read the raw (encrypted) UDP record.
        Endpoint rawFrom;
        IPAddress remoteIp = udp_.remoteIP();
        rawFrom.ip[0] = remoteIp[0]; rawFrom.ip[1] = remoteIp[1];
        rawFrom.ip[2] = remoteIp[2]; rawFrom.ip[3] = remoteIp[3];
        rawFrom.port  = udp_.remotePort();

        int toRead = packetSize < static_cast<int>(sizeof(rawBuf_))
                   ? packetSize : static_cast<int>(sizeof(rawBuf_));
        int nRead = udp_.read(rawBuf_, static_cast<size_t>(toRead));
        if (nRead <= 0) return 0;
        rawLen_ = static_cast<size_t>(nRead);

        // Find or create a session for this peer.
        Session* s = findSession(rawFrom);
        if (!s) {
            if (!isServer_) return 0; // unexpected; discard
            s = allocSession(rawFrom);
            if (!s) return 0;
            if (!initSession(*s)) { resetSession(*s); return 0; }
        }

        // Point the bio-recv at this session's raw buffer.
        s->rxPtr      = rawBuf_;
        s->rxLen      = rawLen_;
        s->rxConsumed = 0;
        currentSession_ = s;

        if (s->state == Session::HANDSHAKING) {
            driveHandshake(*s);
            if (s->state == Session::ESTABLISHED && s->txPending) {
                sslWrite(*s, s->txBuf, s->txLen);
                s->txPending = false;
            }
            currentSession_ = nullptr;
            return 0; // no application data yet
        }

        if (s->state == Session::ESTABLISHED) {
            int n = mbedtls_ssl_read(&s->ssl, buffer, capacity);
            currentSession_ = nullptr;
            if (n > 0) {
                from = rawFrom;
                return static_cast<size_t>(n);
            }
        }

        currentSession_ = nullptr;
        return 0;
    }

private:
    // -------------------------------------------------------------------------
    // Internal types (defined before use)
    // -------------------------------------------------------------------------

    // DTLS timer state — required by mbedTLS for its own flight retransmission.
    struct Timer {
        uint32_t startMs = 0;
        uint32_t intMs   = 0;   // intermediate deadline
        uint32_t finMs   = 0;   // final deadline
        bool     armed   = false;
    };

    struct Session {
        enum State : uint8_t { IDLE, HANDSHAKING, ESTABLISHED, FAILED } state = IDLE;
        Endpoint peer;
        mbedtls_ssl_context ssl = {};
        Timer    timer;

        // Pending outgoing packet buffered while handshake is in progress.
        uint8_t txBuf[PULSECOAP_MAX_MSG_SIZE] = {};
        size_t  txLen     = 0;
        bool    txPending = false;

        // Slice of the current incoming raw UDP record fed to bioRecv.
        const uint8_t* rxPtr      = nullptr;
        size_t         rxLen      = 0;
        size_t         rxConsumed = 0;

        uint32_t handshakeStartMs = 0;
    };

    // -------------------------------------------------------------------------
    // Data members
    // -------------------------------------------------------------------------
    UDP&     udp_;
    bool     isServer_;
    uint16_t localPort_ = 0;

    Session  sessions_[PULSECOAP_DTLS_MAX_SESSIONS];
    PskEntry pskStore_[PULSECOAP_DTLS_MAX_PSK_ENTRIES];

    mbedtls_ssl_config       conf_;
    mbedtls_entropy_context  entropy_;
    mbedtls_ctr_drbg_context ctrDrbg_;
    mbedtls_ssl_cookie_ctx   cookieCtx_;   // server only

    // Scratch buffer for the current incoming raw UDP record.
    // One packet at a time is fine because loop() is single-threaded.
    uint8_t  rawBuf_[PULSECOAP_MAX_MSG_SIZE + 64]; // +64 for DTLS record header
    size_t   rawLen_ = 0;

    // The session currently being driven through send() or receive().
    // Set before any mbedTLS call that may invoke the bio callbacks.
    Session* currentSession_ = nullptr;

    // -------------------------------------------------------------------------
    // Session management
    // -------------------------------------------------------------------------
    Session* findSession(const Endpoint& ep) {
        for (auto& s : sessions_)
            if (s.state != Session::IDLE && s.peer == ep) return &s;
        return nullptr;
    }

    Session* allocSession(const Endpoint& ep) {
        // 1. Prefer a genuinely empty slot.
        for (auto& s : sessions_)
            if (s.state == Session::IDLE) { s.peer = ep; return &s; }
        // 2. Reuse a failed slot.
        for (auto& s : sessions_)
            if (s.state == Session::FAILED) { resetSession(s); s.peer = ep; return &s; }
        // 3. Evict an established session (oldest heuristic — first slot found).
        for (auto& s : sessions_)
            if (s.state == Session::ESTABLISHED) { resetSession(s); s.peer = ep; return &s; }
        return nullptr;
    }

    void resetSession(Session& s) {
        if (s.state != Session::IDLE) mbedtls_ssl_free(&s.ssl);
        memset(&s, 0, sizeof(s));
        s.state = Session::IDLE;
    }

    // Initialise mbedTLS context for one session.
    bool initSession(Session& s) {
        mbedtls_ssl_init(&s.ssl);
        if (mbedtls_ssl_setup(&s.ssl, &conf_) != 0) return false;

        // Bio send/recv — ctx is 'this' (the transport); the current session
        // is always in currentSession_ when mbedTLS calls these callbacks.
        mbedtls_ssl_set_bio(&s.ssl, this, sBioSend, sBioRecv, nullptr);

        // DTLS retransmission timer — ctx is the Session*.
        mbedtls_ssl_set_timer_cb(&s.ssl, &s, sTimerSet, sTimerGet);

        if (isServer_) {
            // Bind the client's "transport ID" for cookie validation.
            uint8_t cid[6];
            memcpy(cid, s.peer.ip, 4);
            cid[4] = static_cast<uint8_t>(s.peer.port >> 8);
            cid[5] = static_cast<uint8_t>(s.peer.port);
            mbedtls_ssl_set_client_transport_id(&s.ssl, cid, sizeof(cid));
        }

        s.state             = Session::HANDSHAKING;
        s.handshakeStartMs  = millis();
        return true;
    }

    void bufferTx(Session& s, const uint8_t* data, size_t length) {
        size_t cap = sizeof(s.txBuf);
        size_t n   = length < cap ? length : cap;
        memcpy(s.txBuf, data, n);
        s.txLen     = n;
        s.txPending = true;
    }

    void driveHandshake(Session& s) {
        int ret = mbedtls_ssl_handshake(&s.ssl);
        if (ret == 0)
            s.state = Session::ESTABLISHED;
        else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                 ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            s.state = Session::FAILED;
        // WANT_READ / WANT_WRITE → normal; stay HANDSHAKING.
    }

    bool sslWrite(Session& s, const uint8_t* data, size_t length) {
        int n = mbedtls_ssl_write(&s.ssl, data, length);
        return (n >= 0 && static_cast<size_t>(n) == length);
    }

    void tickTimeouts() {
        uint32_t now = millis();
        for (auto& s : sessions_) {
            if (s.state == Session::HANDSHAKING &&
                (now - s.handshakeStartMs) > PULSECOAP_DTLS_HANDSHAKE_TIMEOUT_MS) {
                resetSession(s);
            }
        }
    }

    PskEntry* firstPsk() {
        for (auto& e : pskStore_) if (e.used) return &e;
        return nullptr;
    }

    // -------------------------------------------------------------------------
    // Static bio callbacks
    // -------------------------------------------------------------------------
    // bioSend: mbedTLS wants to emit encrypted bytes — send them via UDP.
    static int sBioSend(void* ctx, const unsigned char* buf, size_t len) {
        auto* self = static_cast<DtlsTransport*>(ctx);
        Session* s = self->currentSession_;
        if (!s) return MBEDTLS_ERR_NET_SEND_FAILED;
        IPAddress addr(s->peer.ip[0], s->peer.ip[1],
                       s->peer.ip[2], s->peer.ip[3]);
        if (self->udp_.beginPacket(addr, s->peer.port) == 0)
            return MBEDTLS_ERR_NET_SEND_FAILED;
        self->udp_.write(buf, len);
        if (!self->udp_.endPacket()) return MBEDTLS_ERR_NET_SEND_FAILED;
        return static_cast<int>(len);
    }

    // bioRecv: mbedTLS wants to read encrypted bytes — drain from rxBuf.
    static int sBioRecv(void* ctx, unsigned char* buf, size_t len) {
        auto* self = static_cast<DtlsTransport*>(ctx);
        Session* s = self->currentSession_;
        if (!s || !s->rxPtr || s->rxConsumed >= s->rxLen)
            return MBEDTLS_ERR_SSL_WANT_READ;
        size_t avail = s->rxLen - s->rxConsumed;
        size_t give  = avail < len ? avail : len;
        memcpy(buf, s->rxPtr + s->rxConsumed, give);
        s->rxConsumed += give;
        return static_cast<int>(give);
    }

    // -------------------------------------------------------------------------
    // DTLS retransmission timer callbacks
    // -------------------------------------------------------------------------
    static void sTimerSet(void* ctx, uint32_t intMs, uint32_t finMs) {
        auto* t = &static_cast<Session*>(ctx)->timer;
        t->intMs   = intMs;
        t->finMs   = finMs;
        t->startMs = millis();
        t->armed   = (finMs != 0);
    }

    // Returns: -1=cancelled, 0=running, 1=intermediate expired, 2=final expired.
    static int sTimerGet(void* ctx) {
        const auto* t = &static_cast<Session*>(ctx)->timer;
        if (!t->armed) return -1;
        uint32_t elapsed = millis() - t->startMs;
        if (elapsed >= t->finMs) return 2;
        if (elapsed >= t->intMs) return 1;
        return 0;
    }

    // -------------------------------------------------------------------------
    // Server-side PSK lookup callback
    // -------------------------------------------------------------------------
    // mbedTLS calls this during a server-side handshake to resolve the key
    // for the identity the client advertised.
    static int sPskCallback(void* parameter,
                            mbedtls_ssl_context* ssl,
                            const unsigned char* identity,
                            size_t identityLen) {
        auto* self = static_cast<DtlsTransport*>(parameter);
        for (auto& e : self->pskStore_) {
            if (!e.used) continue;
            size_t eLen = strlen(e.identity);
            if (eLen == identityLen &&
                memcmp(e.identity, identity, identityLen) == 0) {
                return mbedtls_ssl_set_hs_psk(ssl, e.key, e.keyLen);
            }
        }
        return -1; // identity not found — handshake fails (peer is rejected)
    }
};

} // namespace pulsecoap

#endif // ARDUINO && PULSECOAP_ENABLE_DTLS
