# PulseCoAP Roadmap

## Done (v0.1.0)

- **Message codec** (`PulseCoAPMessage`): full RFC 7252 §3 encode/decode —
  header, 0-8 byte token, delta+length option encoding including both
  extended-byte cases (13 and 269 thresholds), payload marker handling,
  malformed-input rejection (bad version, bad TKL, reserved nibble 15,
  truncated token/option/payload, empty payload marker). 46 host-side
  checks in `test/test_message_codec.cpp`.
- **Reliability layer** (`TransactionPool`): fixed pool of in-flight
  Confirmable messages, RFC 7252 §4.8 exponential backoff with jitter,
  give-up-after-`PULSECOAP_MAX_RETRANSMIT` timeout handling, wraparound-safe
  timing. 28 checks in `test/test_transaction_pool.cpp`.
- **Server role**: resource registration with a method bitmask, exact-path
  dispatch, piggybacked responses, Observe (RFC 7641) register/notify/
  deregister.
- **Client role**: GET/PUT/POST/DELETE (confirmable or non-confirmable),
  Observe registration with a repeating notification callback,
  `cancelObserve()`, automatic retry via `TransactionPool`.
- **Transport abstraction** + an ESP32/Arduino `WiFiUDP` adapter
  (`PulseCoAPTransportArduinoUDP.h`).
- **Compile-time role selection** (`PULSECOAP_ROLE_CLIENT_ONLY` /
  `PULSECOAP_ROLE_SERVER_ONLY`) and all sizing/timing knobs in
  `PulseCoAPConfig.h`.
- **End-to-end integration test** (`test/test_client_server_integration.cpp`):
  a real `Server` and `Client` exchanging real wire-format messages over an
  in-process loopback `Transport` double — GET, PUT, and Observe
  register+notify. 24 checks.

## Done (v0.2.1)

- **RST message handling** — both roles now handle incoming RST frames
  correctly. Server: a RST matching an observer's last-sent message ID
  immediately deactivates that observer slot (RFC 7641 §4.2). Client: a
  RST for an in-flight CON request immediately completes the transaction
  and fires `TimeoutHandler` without waiting for `MAX_RETRANSMIT` retries.
  `TransactionPool` gained a `getToken()` helper so the client can recover
  the pending-request token from a RST (which carries only a message ID).
  `LoopbackTransport` gained `injectPacket()` / `peek()` for RST test
  injection. 18 new integration checks.

## Done (v0.2.0)

- **`.well-known/core` resource discovery** (CoRE Link Format, RFC 6690) —
  `Server::begin()` auto-registers `/.well-known/core`. A `GET` on it returns
  all user-registered resource paths in CoRE Link Format (Content-Format 40),
  with `;obs` for observable resources and `;rt="…"` when set via
  `setResourceType()`. No IP hardcoding needed once a client can discover the
  server's resources. 27 new integration checks.

## Done (v0.3.0)

- **Separate (non-piggybacked) responses** (RFC 7252 §5.2.2) —
  `Server::addResource()` handlers can now set `res.deferred = true` and
  store `req.deferHandle`; the server immediately sends an empty ACK to a
  CON request (stopping retransmission) and later calls
  `server.respond(handle, code, payload, length)` to send the real answer.
  The separate response is CON when the original request was Confirmable
  (registered in the server's embedded `TransactionPool` for automatic
  retransmission until ACK'd) and NON otherwise. `Client::poll()` now ACKs
  incoming CON responses from the server, completing the retransmission
  cycle. 35 new integration checks; 178 checks across all suites.

## Done (v0.4.0)

- **CON-based (reliable) Observe notifications** (RFC 7641 §4.5) —
  `notify()` gains a `bool confirmable = false` parameter. When true, the
  notification is sent as a Confirmable message, registered in the server's
  `TransactionPool`, and retransmitted with RFC 7252 §4.8 backoff until
  ACK'd. If `MAX_RETRANSMIT` retries are exhausted with no ACK, the observer
  slot is silently freed — dead-peer detection without any application code.
  `Client::poll()` automatically ACKs incoming CON responses from the server
  (already added in v0.3.0). Recommended usage: send CON every N pushes to
  prune stale registrations. 21 new integration checks; 199 checks total.

## Done (v0.5.0)

- **Block-wise transfer** (Block1/Block2, RFC 7959) — gated on
  `PULSECOAP_ENABLE_BLOCKWISE=1`. Server auto-fragments large GET
  responses (Block2): handler returns the full payload; the server slices
  it and responds to each follow-up GET with the appropriate block.
  Client reassembles all blocks and fires `onResponse` once with the
  complete payload. Client auto-fragments large PUT/POST uploads (Block1):
  sends blocks sequentially, advances on each 2.31 Continue, fires
  `onResponse` when the server returns the final 2.04/2.01. Server
  reassembles Block1 uploads into a fixed buffer and hands the full body
  to the resource handler via `req.payload()`/`req.payloadLength()`.
  New config knobs: `PULSECOAP_BLOCK_SZX` (block size exponent, default
  4 = 256 B), `PULSECOAP_MAX_BLOCK1_SESSIONS`, `PULSECOAP_BLOCK1_MAX_BODY`,
  `PULSECOAP_BLOCK2_MAX_BODY`. 29 new checks in
  `test/test_blockwise.cpp`; **228 checks total across all suites**.

## Done (v0.6.0)

- **URI-template resource paths** — `addResource()` now accepts `:param`
  segments (e.g. `/sensors/:id`, `/devices/:dev/ch/:ch`). Exact paths keep
  their zero-overhead `strcmp` fast path (Phase 1); template paths are tried
  in registration order only when no exact match is found (Phase 2), so
  existing code is unaffected. Inside handlers, `req.pathParam("id")` returns
  the matched segment value as a NUL-terminated string (bounded by
  `PULSECOAP_MAX_PATH_PARAM_LEN`), or `nullptr` if the name is absent.
  Exact-before-template priority is enforced regardless of registration order.
  New config knobs: `PULSECOAP_MAX_PATH_PARAMS` (default 4),
  `PULSECOAP_MAX_PATH_PARAM_LEN` (default 16 bytes including NUL). 38 new
  checks in `test/test_path_templates.cpp`; **266 checks total**.

## Done (v0.7.0)

- **Multiple simultaneous observes per server / `cancelObserve()` by path**
  — `PendingRequest` now stores the resource path so `cancelObserve(server,
  path)` can distinguish two simultaneous subscriptions on the same server.
  `observe("/sensors/temp")` and `observe("/sensors/hum")` on the same server
  occupy separate pending slots and independent callbacks; cancelling one by
  path leaves the other untouched. `cancelObserve()` still returns `false`
  when no matching (server, path) subscription is active. 35 new checks in
  `test/test_multi_observe.cpp`; **301 checks total**.

## Done (v0.8.0)

- **Additional transport adapters** — two new header-only adapters alongside
  the existing `PulseCoAPTransportArduinoUDP.h`:
  - **`PulseCoAPTransportLwIP.h`** — bare-metal lwIP 2.x raw UDP API
    (`udp_new` / `udp_bind` / `udp_sendto` / receive callback). A
    `PULSECOAP_LWIP_RX_SLOTS`-slot ring buffer decouples the lwIP callback
    from `poll()` without any RTOS or heap allocation. Gated on
    `PULSECOAP_TRANSPORT_LWIP` or the presence of `LWIP_RAW` in the
    build — never compiled into Arduino or POSIX builds.
  - **`PulseCoAPTransportPosix.h`** — POSIX/BSD `SOCK_DGRAM` adapter for
    Linux, macOS, and POSIX-compliant RTOS targets (Zephyr, NuttX, RIOT).
    Non-blocking (`O_NONBLOCK`), `SO_REUSEADDR`, and a `localPort()` helper
    for ephemeral-port tests. Gated on `__unix__` / `__APPLE__` /
    `_POSIX_VERSION`. Used by the new `test/test_posix_transport.cpp` suite.
  - (AVR boards using Arduino's `EthernetUDP` or `WiFiUDP` are already
    covered by the existing `ArduinoUdpTransport`, which wraps any `UDP&`
    subclass.)
  26 new checks in `test/test_posix_transport.cpp` (raw loopback, GET/response,
  Observe over real OS sockets); **327 checks total across all suites**.

## Done (v0.9.0)

- **IPv6 addressing** — `Endpoint` (in `PulseCoAPTransport.h`) now carries
  both an `ip[4]` IPv4 field (unchanged — all existing code compiles without
  modification) and a `v6[16]` IPv6 field plus an `isV6` flag.
  `operator==` compares by family, so an IPv4 and an IPv6 endpoint with the
  same port are never equal. Transport adapters:
  - **`PosixUdpTransport`** opens a dual-stack `AF_INET6` socket
    (`IPV6_V6ONLY=0`); falls back to `AF_INET` if the kernel has no IPv6.
    `send()` maps IPv4 targets to `::ffff:a.b.c.d`; `receive()` demaps
    incoming IPv4-mapped addresses so callers always see plain IPv4 for IPv4
    peers and plain IPv6 for IPv6 peers.
  - **`LwIpUdpTransport`**: when built with `LWIP_IPV6=1`, creates a
    dual-stack PCB via `udp_new_ip_type(IPADDR_TYPE_ANY)`, handles both
    families in the send path (`IP_SET_TYPE_VAL` + `ip_2_ip6()`) and in
    the receive callback (`IP_IS_V6()`). Falls back to IPv4-only when
    `LWIP_IPV6` is not defined.
  - **`ArduinoUdpTransport`**: unchanged — most Arduino stacks expose only
    IPv4 UDP. IPv6-capable Arduino targets (ESP32 with lwIP6) can use
    `LwIpUdpTransport` directly.
  7 new checks in `test/test_posix_transport.cpp` (Endpoint struct semantics,
  raw IPv6 loopback, GET over IPv6 — last two skip gracefully when the host
  kernel has no IPv6); **334 checks total across all suites**.

## Not yet implemented

Roughly in the order they'd likely get picked up:
- **`PulseTrace` integration** — `PULSECOAP_ENABLE_TRACE` is reserved but
  no hook calls exist yet. (Deferred — not the next priority.)
- **DTLS / CoAPs** — deliberately deferred past v1: pulls in mbedTLS,
  a much heavier dependency than anything else in the no-heap Pulse
  ecosystem, and most constrained deployments run CoAP unencrypted on a
  trusted LAN/gateway.
- **Arduino Library Manager / PlatformIO registry publication** —
  `library.properties` / `library.json` still have `FILL_IN_ORG`
  placeholders for the eventual GitHub repo URL.
