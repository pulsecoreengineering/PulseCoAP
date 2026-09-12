// PulseCoAPConfig.h
// Compile-time configuration for PulseCoAP.
//
// Every knob here can be overridden by the application by #define-ing it
// BEFORE #include <PulseCoAP.h> (e.g. in a build flag or a config header
// included first). Nothing in this library allocates from the heap; every
// buffer is sized from these constants at compile time, following the same
// convention as PulseHSM / PulseVars (PULSEHSM_MAX_STATES, etc).
#pragma once

// ---------------------------------------------------------------------------
// Message sizing
// ---------------------------------------------------------------------------

// Maximum size, in bytes, of a single CoAP message (header + token + options
// + payload) that PulseCoAP will encode or decode. RFC 7252 recommends
// keeping messages within a single IP datagram to avoid fragmentation;
// 256 bytes is a reasonable default for constrained links. Raise this if you
// need bigger payloads and aren't using block-wise transfer.
#ifndef PULSECOAP_MAX_MSG_SIZE
#define PULSECOAP_MAX_MSG_SIZE 256
#endif

// Maximum number of options PulseCoAP will index per message. RFC 7252
// places no hard cap; 16 comfortably covers Uri-Path (multiple segments),
// Uri-Query, Content-Format, Observe, ETag, Block1/Block2 in one message.
#ifndef PULSECOAP_MAX_OPTIONS
#define PULSECOAP_MAX_OPTIONS 16
#endif

// CoAP tokens are 0-8 bytes (RFC 7252 §3). This is fixed by the spec, not
// really a tuning knob, but exposed for clarity / static_assert use.
#ifndef PULSECOAP_MAX_TOKEN_LEN
#define PULSECOAP_MAX_TOKEN_LEN 8
#endif

// ---------------------------------------------------------------------------
// Reliability layer (transactions / retransmission)
// ---------------------------------------------------------------------------

// How many Confirmable messages can be in flight (awaiting ACK) at once,
// client- or server-side. Fixed pool, no heap.
#ifndef PULSECOAP_MAX_TRANSACTIONS
#define PULSECOAP_MAX_TRANSACTIONS 4
#endif

// RFC 7252 §4.8 default timing. ACK_TIMEOUT is the base retransmit interval;
// actual first timeout is randomized within
// [ACK_TIMEOUT, ACK_TIMEOUT * ACK_RANDOM_FACTOR], then doubled on each retry.
#ifndef PULSECOAP_ACK_TIMEOUT_MS
#define PULSECOAP_ACK_TIMEOUT_MS 2000
#endif

// ACK_RANDOM_FACTOR expressed as a fraction (NUM/DEN) to avoid pulling in
// floating point on platforms that would rather not link libm. Default 1.5.
#ifndef PULSECOAP_ACK_RANDOM_FACTOR_NUM
#define PULSECOAP_ACK_RANDOM_FACTOR_NUM 3
#endif
#ifndef PULSECOAP_ACK_RANDOM_FACTOR_DEN
#define PULSECOAP_ACK_RANDOM_FACTOR_DEN 2
#endif

// RFC 7252 default MAX_RETRANSMIT.
#ifndef PULSECOAP_MAX_RETRANSMIT
#define PULSECOAP_MAX_RETRANSMIT 4
#endif

// ---------------------------------------------------------------------------
// Server role sizing
// ---------------------------------------------------------------------------

#ifndef PULSECOAP_MAX_RESOURCES
#define PULSECOAP_MAX_RESOURCES 8
#endif

#ifndef PULSECOAP_MAX_OBSERVERS
#define PULSECOAP_MAX_OBSERVERS 8
#endif

#ifndef PULSECOAP_MAX_URI_PATH_LEN
#define PULSECOAP_MAX_URI_PATH_LEN 64
#endif

// Maximum number of requests the Server can hold in a "deferred" state at
// once — i.e. requests whose handler returned res.deferred=true so the
// server sent an empty ACK and will call respond() later. Each slot costs
// ~(4 + PULSECOAP_MAX_TOKEN_LEN) bytes. Default 2 is enough for most
// single-CPU constrained devices that process one sensor at a time.
#ifndef PULSECOAP_MAX_DEFERRED
#define PULSECOAP_MAX_DEFERRED 2
#endif

// Maximum byte length of the CoRE Link Format body that Server serialises
// and returns from GET /.well-known/core. One entry takes roughly
// len(path) + 10 bytes; 256 bytes covers ~8 resources at typical path
// lengths. Raise if you register many resources with long paths or rt= values.
#ifndef PULSECOAP_MAX_LINK_FORMAT_LEN
#define PULSECOAP_MAX_LINK_FORMAT_LEN 256
#endif

// ---------------------------------------------------------------------------
// Role selection
// ---------------------------------------------------------------------------
// Both client and server are compiled in by default. Define ONE of these
// before including PulseCoAP.h to strip the other half out of the build:
//
//   #define PULSECOAP_ROLE_CLIENT_ONLY   // drop PulseCoAPServer
//   #define PULSECOAP_ROLE_SERVER_ONLY   // drop PulseCoAPClient
#if defined(PULSECOAP_ROLE_CLIENT_ONLY) && defined(PULSECOAP_ROLE_SERVER_ONLY)
#error "PulseCoAP: define at most one of PULSECOAP_ROLE_CLIENT_ONLY / PULSECOAP_ROLE_SERVER_ONLY"
#endif

#if defined(PULSECOAP_ROLE_SERVER_ONLY)
#define PULSECOAP_ENABLE_CLIENT 0
#else
#define PULSECOAP_ENABLE_CLIENT 1
#endif

#if defined(PULSECOAP_ROLE_CLIENT_ONLY)
#define PULSECOAP_ENABLE_SERVER 0
#else
#define PULSECOAP_ENABLE_SERVER 1
#endif

// ---------------------------------------------------------------------------
// Optional modules (off by default — enable explicitly to pull them in)
// ---------------------------------------------------------------------------

// Block-wise transfer (RFC 7959, Block1/Block2). Off by default: most
// constrained-device payloads fit in one datagram, and this module pulls in
// extra reassembly state.
#ifndef PULSECOAP_ENABLE_BLOCKWISE
#define PULSECOAP_ENABLE_BLOCKWISE 0
#endif

// Block size exponent (SZX). Actual block size = 2^(SZX+4) bytes.
// SZX=4 → 256 B, SZX=3 → 128 B, SZX=2 → 64 B. Must be 0-6.
// Ensure block_size + ~20-byte message overhead ≤ PULSECOAP_MAX_MSG_SIZE.
#ifndef PULSECOAP_BLOCK_SZX
#define PULSECOAP_BLOCK_SZX 4
#endif

// Server: maximum simultaneous Block1 (client upload) reassembly sessions.
#ifndef PULSECOAP_MAX_BLOCK1_SESSIONS
#define PULSECOAP_MAX_BLOCK1_SESSIONS 2
#endif

// Server: byte capacity of each Block1 reassembly buffer (full incoming body).
#ifndef PULSECOAP_BLOCK1_MAX_BODY
#define PULSECOAP_BLOCK1_MAX_BODY 1024
#endif

// Client: byte capacity of the Block2 receive-side reassembly buffer
// (per pending request, only when PULSECOAP_ENABLE_BLOCKWISE is set).
#ifndef PULSECOAP_BLOCK2_MAX_BODY
#define PULSECOAP_BLOCK2_MAX_BODY 1024
#endif

// ---------------------------------------------------------------------------
// URI template parameters (wildcard path segments, e.g. /sensors/:id)
// ---------------------------------------------------------------------------

// Maximum number of template parameters per request (e.g., 2 for
// /devices/:dev/ch/:ch). Raise if you need more segments.
#ifndef PULSECOAP_MAX_PATH_PARAMS
#define PULSECOAP_MAX_PATH_PARAMS 4
#endif

// Maximum byte length (including NUL) of a single parameter name or value.
// "temperature" = 11 chars; "sensor-node-17" = 14 chars — 16 is tight but
// sufficient for typical embedded paths. Raise for longer segment strings.
#ifndef PULSECOAP_MAX_PATH_PARAM_LEN
#define PULSECOAP_MAX_PATH_PARAM_LEN 16
#endif

// ---------------------------------------------------------------------------
// DTLS transport (RFC 6347, CoAP over DTLS per RFC 7252 §9)
// ---------------------------------------------------------------------------
// Off by default: requires mbedTLS, pulls in ~500 B per session, and the
// mbedTLS handshake itself uses heap internally (ESP32 Arduino ships with
// CONFIG_MBEDTLS_DYNAMIC_BUFFER=y). PulseCoAP's own message / routing code
// stays zero-heap regardless; only the mbedTLS wrapper uses the heap.
// Enable by defining PULSECOAP_ENABLE_DTLS 1 before #include <PulseCoAP.h>.
#ifndef PULSECOAP_ENABLE_DTLS
#define PULSECOAP_ENABLE_DTLS 0
#endif

// Maximum simultaneous DTLS sessions. Each slot holds one mbedtls_ssl_context
// plus a small receive buffer. 4 is enough for a typical sensor hub that talks
// to one gateway + a handful of peers. Raise if you need more.
#ifndef PULSECOAP_DTLS_MAX_SESSIONS
#define PULSECOAP_DTLS_MAX_SESSIONS 4
#endif

// Maximum PSK entries in the static key store. Each entry is an identity
// string + secret key pair. 8 covers common multi-device deployments.
#ifndef PULSECOAP_DTLS_MAX_PSK_ENTRIES
#define PULSECOAP_DTLS_MAX_PSK_ENTRIES 8
#endif

// Maximum byte length (including NUL) of a PSK identity string.
// "sensor-node-17" = 14 chars — 32 is comfortable.
#ifndef PULSECOAP_DTLS_IDENTITY_MAX_LEN
#define PULSECOAP_DTLS_IDENTITY_MAX_LEN 32
#endif

// Maximum byte length of a PSK secret (raw bytes, not NUL-terminated).
// 16 bytes (128-bit key) is common; 32 bytes (256-bit) covers AES-256.
#ifndef PULSECOAP_DTLS_PSK_MAX_LEN
#define PULSECOAP_DTLS_PSK_MAX_LEN 32
#endif

// Maximum time (ms) allowed for a DTLS handshake to complete before the
// session is abandoned and marked FAILED. 10 s accommodates slow/lossy links.
#ifndef PULSECOAP_DTLS_HANDSHAKE_TIMEOUT_MS
#define PULSECOAP_DTLS_HANDSHAKE_TIMEOUT_MS 10000
#endif

// ---------------------------------------------------------------------------
// Multicast resource discovery (RFC 7252 §8)
// ---------------------------------------------------------------------------
// Client::discover() sends a NON GET to 224.0.1.187:5683 for
// /.well-known/core. Every PulseCoAP server on the LAN that has joined the
// multicast group responds unicast; each response fires DiscoverHandler.
// Slots are freed automatically after PULSECOAP_DISCOVER_TIMEOUT_MS.

// Maximum simultaneously active discover() operations. Two is enough for most
// devices — one active discovery with headroom for a second before the first
// expires.
#ifndef PULSECOAP_MAX_DISCOVERS
#define PULSECOAP_MAX_DISCOVERS 2
#endif

// How long (ms) a discover slot stays active, collecting responses. RFC 7252
// §8.2 says servers should randomise their reply within [0, ACK_TIMEOUT] to
// avoid response floods — 5 000 ms catches even the slowest responders.
#ifndef PULSECOAP_DISCOVER_TIMEOUT_MS
#define PULSECOAP_DISCOVER_TIMEOUT_MS 5000
#endif

// ---------------------------------------------------------------------------
// PulseTrace hook for retransmits / timeouts / dropped or duplicate
// messages. Off by default so PulseCoAP has zero Pulse-ecosystem
// dependencies unless you opt in.
#ifndef PULSECOAP_ENABLE_TRACE
#define PULSECOAP_ENABLE_TRACE 0
#endif
