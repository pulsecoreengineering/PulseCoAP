# Changelog

All notable changes to PulseCoAP are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

---

## [1.0.0] — 2025-01-15

Initial public release.

### Added

**Core protocol**
- CoAP RFC 7252 — Confirmable, Non-confirmable, Acknowledgement, Reset message types
- CoAP Observe RFC 7641 — server-side `notify()`, client-side `observe()` / `cancelObserve()`
- Deferred (separate) responses RFC 7252 §5.2.2 — `res.deferred`, `req.deferHandle`, `server.respond()`
- URI templates — `:name` segments in resource paths; `req.pathParam()` to extract matched values
- Resource discovery RFC 6690 — `/.well-known/core` auto-registered at `server.begin()`; `server.setResourceType()`
- Multicast discovery — `client.discover()` to `224.0.1.187:5683`; `client.allNodesEndpoint()`

**Server API**
- `Server::addResource()` — path, method bitmask, handler, optional user context, observable flag
- `Server::notify()` — NON and CON (dead-peer detection) variants
- `Server::respond()` — deferred response delivery
- `Server::observerCount()` — diagnostic observer count
- `MethodMask` — `MethodGet`, `MethodPost`, `MethodPut`, `MethodDelete`
- `Request::pathParam()`, `Request::payload()`, `Request::payloadLength()`
- `Response::setPayload()`, `Response::deferred`

**Client API**
- `Client::get()`, `put()`, `post()`, `del()` — CON and NON variants
- `Client::observe()`, `cancelObserve()`
- `Client::discover()` — unicast and multicast overloads
- `Client::setTimeoutHandler()` — global timeout callback
- `TransactionPool` — shared in-flight CON tracker with retransmission

**Types**
- `Code` enum — all RFC 7252 method codes (Get/Post/Put/Delete) and response codes (2.xx–5.xx)
- `ContentFormat` — TextPlain, LinkFormat, Xml, OctetStream, Exi, Json, Cbor
- `MessageType` — Confirmable, NonConfirmable, Acknowledgement, Reset
- `Endpoint` — IPv4 and IPv6 (RFC 4291) address + port; `==` / `!=` comparison
- `OptionNumber` — IfMatch, UriHost, ETag, Observe, UriPort, UriPath, ContentFormat, MaxAge, UriQuery, Block1, Block2

**Transports**
- `ArduinoUdpTransport` — wraps any `UDP&`-derived Arduino class (WiFiUDP, EthernetUDP)
- `PosixUdpTransport` — dual-stack `AF_INET6` socket; `joinMulticastGroup()`; `setMulticastOutboundInterface()`; `localPort()`
- `LwIpUdpTransport` — bare-metal lwIP 2.x, dual-stack when `LWIP_IPV6=1`, configurable RX ring (`PULSECOAP_LWIP_RX_SLOTS`)
- `DtlsTransport` — mbedTLS DTLS 1.2 PSK; `addPsk()` (server); `setClientPsk()` (client); requires `PULSECOAP_ENABLE_DTLS 1`

**Optional modules**
- Block-wise transfer RFC 7959 — Block1 (upload) and Block2 (download); requires `PULSECOAP_ENABLE_BLOCKWISE 1`
- DTLS 1.2 PSK — mbedTLS 2.x/3.x; requires `PULSECOAP_ENABLE_DTLS 1`

**Configuration macros**
- Message sizing: `PULSECOAP_MAX_MSG_SIZE`, `PULSECOAP_MAX_OPTIONS`, `PULSECOAP_MAX_TOKEN_LEN`
- Reliability: `PULSECOAP_MAX_TRANSACTIONS`, `PULSECOAP_ACK_TIMEOUT_MS`, `PULSECOAP_ACK_RANDOM_FACTOR_NUM/DEN`, `PULSECOAP_MAX_RETRANSMIT`
- Server sizing: `PULSECOAP_MAX_RESOURCES`, `PULSECOAP_MAX_OBSERVERS`, `PULSECOAP_MAX_URI_PATH_LEN`, `PULSECOAP_MAX_DEFERRED`, `PULSECOAP_MAX_LINK_FORMAT_LEN`
- URI templates: `PULSECOAP_MAX_PATH_PARAMS`, `PULSECOAP_MAX_PATH_PARAM_LEN`
- Discovery: `PULSECOAP_MAX_DISCOVERS`, `PULSECOAP_DISCOVER_TIMEOUT_MS`
- Block-wise: `PULSECOAP_BLOCK_SZX`, `PULSECOAP_MAX_BLOCK1_SESSIONS`, `PULSECOAP_BLOCK1_MAX_BODY`, `PULSECOAP_BLOCK2_MAX_BODY`
- DTLS: `PULSECOAP_DTLS_MAX_SESSIONS`, `PULSECOAP_DTLS_HANDSHAKE_TIMEOUT_MS`, `PULSECOAP_DTLS_SESSION_TIMEOUT_MS`

**Examples**
- `BasicServer` — GET `/sensors/temp`, observable resource
- `BasicClient` — periodic GET with timeout handler
- `ObserveClient` — subscribe and receive push notifications
- `DeferredResponse` — slow-sensor handler with deferred reply
- `URITemplates` — parameterized paths with loopback
- `ResourceDiscovery` — `/.well-known/core` + resource types
- `MultiObserve` — three simultaneous subscriptions, independent cancellation

**Documentation**
- Docsify-powered docs site (`docs/`)
- Getting Started: Concepts, Quick Start, Configuration, Transports & Platforms
- API Reference: Server, Client, Types & Constants
- Use Cases: Sensor Dashboard, Observe, Deferred Response, URI Templates, Resource Discovery
- Reference: FAQ, Changelog

---

[Unreleased]: https://github.com/pulsecoreengineering/PulseCoAP/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/pulsecoreengineering/PulseCoAP/releases/tag/v1.0.0
