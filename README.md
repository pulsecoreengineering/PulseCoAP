# PulseCoAP

A no-heap CoAP (RFC 7252) client and server for constrained IoT devices —
part of the [Pulse embedded library ecosystem](https://logicfrenzy.com)
(PulseHSM, PulseVars, PulseTrace, ...).

CoAP is a lightweight request/response protocol that runs over UDP, fits in
tiny embedded devices, and — via [Observe (RFC 7641)](https://www.rfc-editor.org/rfc/rfc7641) —
lets a device push state changes to subscribers instead of being polled.
PulseCoAP adds zero dynamic allocation, a consistent C++ API, and transport
adapters that compile directly against `WiFiUDP`, bare-metal lwIP, or a POSIX
socket, with no changes to the protocol layer.

## Features

- **No heap, ever.** Every buffer — messages, options, transactions, resources,
  observers, block-wise reassembly — is a fixed-size array sized by a
  `PULSECOAP_MAX_*` compile-time constant in
  [`src/PulseCoAPConfig.h`](src/PulseCoAPConfig.h).

- **Full RFC 7252 message codec.** Header, 0–8 byte token, delta+length option
  encoding (including both extended-byte cases), payload marker, and strict
  rejection of all malformed inputs.

- **Reliable delivery.** `TransactionPool` retransmits Confirmable messages
  with the RFC 7252 §4.8 exponential-backoff-plus-jitter schedule and fires a
  timeout callback after `PULSECOAP_MAX_RETRANSMIT` retries.

- **Both roles, compile-time selectable.** Build both client and server, or
  define `PULSECOAP_ROLE_CLIENT_ONLY` / `PULSECOAP_ROLE_SERVER_ONLY` to strip
  the half you don't need.

- **Observe (RFC 7641).** Register a resource as observable;
  `server.notify(path, payload, length)` pushes the update to every subscriber.
  CON-mode notifications prune dead peers automatically after
  `MAX_RETRANSMIT` failures. RST frames deregister observers immediately.

- **Resource discovery** (`/.well-known/core`, RFC 6690). Auto-registered at
  `begin()`; returns CoRE Link Format with `;obs` and optional `;rt="..."` per
  resource.

- **Separate (non-piggybacked) responses** (RFC 7252 §5.2.2). Handlers that
  need async work set `res.deferred = true`, store `req.deferHandle`, and call
  `server.respond(handle, code, payload, length)` later — the server sends an
  empty ACK immediately to stop client retransmission.

- **Block-wise transfer** (Block1/Block2, RFC 7959). Opt in with
  `PULSECOAP_ENABLE_BLOCKWISE=1`. Server auto-fragments large GET responses;
  client reassembles them. Client auto-fragments large PUT/POST uploads;
  server reassembles into a fixed buffer before calling the handler.

- **URI-template resource paths.** `addResource("/sensors/:id", ...)` matches
  any single segment; `req.pathParam("id")` returns its value. Exact paths have
  priority and zero overhead.

- **Multiple simultaneous Observe subscriptions.** The client tracks each
  `(server, path)` pair independently; `cancelObserve(server, path)` cancels
  one without affecting others on the same server.

- **Multicast resource discovery** (RFC 7252 §8). `client.discover()` sends a
  Non-Confirmable GET to `224.0.1.187:5683` (IANA CoAP all-nodes address) for
  `/.well-known/core`. Every server on the LAN responds unicast; a
  `DiscoverHandler` fires once per responder. Slots expire automatically after
  `PULSECOAP_DISCOVER_TIMEOUT_MS` (default 5 s). `PosixUdpTransport` gains
  `joinMulticastGroup()` and `setMulticastOutboundInterface()` helpers for
  Linux/macOS gateway and test use.

- **Four transport adapters — all header-only:**
  - `PulseCoAPTransportArduinoUDP.h` — wraps any Arduino `UDP&` subclass
    (`WiFiUDP`, `EthernetUDP`, ...). Works on ESP32, ESP8266, Arduino
    Uno/Mega/Nano, Raspberry Pi Pico W.
  - `PulseCoAPTransportLwIP.h` — bare-metal lwIP 2.x raw UDP API for STM32,
    ESP-IDF, RP2040, and any NO_SYS=1 lwIP target. Ring-buffered receive;
    dual-stack IPv6 when `LWIP_IPV6=1`.
  - `PulseCoAPTransportPosix.h` — POSIX `SOCK_DGRAM` for Linux, macOS,
    Zephyr, NuttX, RIOT. Dual-stack IPv6 with automatic IPv4-mapped demapping.
  - `PulseCoAPTransportDTLS.h` — DTLS 1.2 (RFC 6347) CoAPs adapter for
    Arduino/ESP32, built on the ESP32 core's built-in mbedTLS. Pre-shared key
    (PSK) authentication, non-blocking handshake, fixed session pool, and
    server-side DTLS cookies for anti-amplification. Enable with
    `PULSECOAP_ENABLE_DTLS=1`; no custom sdkconfig or extra libraries needed.

- **IPv6.** `Endpoint` carries both `ip[4]` and `v6[16]` with an `isV6` flag.
  Existing code that uses `ep.ip[]` compiles and runs unchanged.

- **Standalone.** Zero dependencies on the rest of the Pulse ecosystem by
  default — `PULSECOAP_ENABLE_TRACE` is reserved for a future PulseTrace
  integration hook.

## Quick Start

### Server (ESP32 + WiFiUDP)

```cpp
#include <WiFi.h>
#include <WiFiUDP.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

using namespace pulsecoap;

WiFiUDP             udp;
ArduinoUdpTransport transport(udp);
Server              server(transport);

void handleTemp(const Request&, Response& res, void*) {
    static char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", readTempC());
    res.code = Code::Content;
    res.setPayload(buf);
}

void setup() {
    WiFi.begin("SSID", "pass");
    while (WiFi.status() != WL_CONNECTED) delay(200);

    server.addResource("/temp", MethodGet, handleTemp, nullptr, /*observable=*/true);
    server.begin(); // CoAP default port 5683
}

void loop() {
    server.poll(millis());
}
```

### Client (ESP32 + WiFiUDP)

```cpp
#include <WiFi.h>
#include <WiFiUDP.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

using namespace pulsecoap;

WiFiUDP             udp;
ArduinoUdpTransport transport(udp);
TransactionPool     txPool;
Client              client(transport, txPool);

Endpoint server;  // fill ip[4] and port before use

void onResponse(const ClientResponse& res, void*) {
    Serial.write(res.payload, res.payloadLength);
    Serial.println();
}

void setup() {
    WiFi.begin("SSID", "pass");
    while (WiFi.status() != WL_CONNECTED) delay(200);

    server.ip[0]=192; server.ip[1]=168; server.ip[2]=1; server.ip[3]=42;
    server.port = 5683;

    client.begin();
    client.get(server, "/temp", onResponse);
}

void loop() {
    client.poll(millis());
}
```

### Observe — push notifications

```cpp
// Subscribe to /temp; onTemp fires for the initial value and every notify().
client.observe(server, "/temp", onTemp, &ctx);

// Server side — push to all subscribers, prune dead peers with CON:
server.notify("/temp",
              reinterpret_cast<const uint8_t*>(buf), strlen(buf),
              ContentFormat::TextPlain,
              /*confirmable=*/true);
```

See [`examples/`](examples) and [`API.md`](API.md) for complete board-by-board
examples (ESP8266, Uno+Ethernet, Pico W, STM32+lwIP, ESP-IDF, Zephyr, NuttX,
Linux/macOS).

## Configuration

Every size limit and timing constant lives in
[`src/PulseCoAPConfig.h`](src/PulseCoAPConfig.h) and can be overridden with a
`#define` before `#include <PulseCoAP.h>` (or as a build flag):

```cpp
#define PULSECOAP_MAX_MSG_SIZE       512  // larger payloads (default 256)
#define PULSECOAP_MAX_RESOURCES       16  // more registered paths (default 8)
#define PULSECOAP_ENABLE_BLOCKWISE     1  // block-wise transfer (default off)
#define PULSECOAP_ROLE_CLIENT_ONLY       // drop server code to save flash
#define PULSECOAP_ENABLE_DTLS          1  // DTLS/CoAPs via mbedTLS (Arduino/ESP32 only)
#include <PulseCoAP.h>
#include <PulseCoAPTransportDTLS.h>      // include after PulseCoAP.h when DTLS is enabled
```

For AVR (Uno/Mega) with limited RAM, always reduce the defaults:

```cpp
#define PULSECOAP_MAX_MSG_SIZE      128
#define PULSECOAP_MAX_RESOURCES       4
#define PULSECOAP_MAX_OBSERVERS       4
#define PULSECOAP_MAX_TRANSACTIONS    2
```

## Testing

No Arduino toolchain needed for the core logic — all suites build and run on
a host `g++`/`clang++`:

```sh
cd test && bash run_tests.sh
```

**375 checks across 8 suites** (as of v1.2.0; DTLS is tested via Arduino examples — mbedTLS is not available on the host):

| Suite | Checks | What it covers |
|-------|--------|----------------|
| `test_message_codec` | 46 | Full RFC 7252 encode/decode, malformed-input rejection |
| `test_transaction_pool` | 28 | Exponential backoff, jitter, wraparound-safe timing |
| `test_client_server_integration` | 125 | GET, PUT, Observe, deferred responses, RST handling |
| `test_blockwise` | 29 | Block1 upload, Block2 fragmentation + reassembly |
| `test_path_templates` | 38 | URI template matching, exact-path priority |
| `test_multi_observe` | 35 | Multiple simultaneous observes, independent cancel |
| `test_posix_transport` | 33* | Real OS loopback sockets, GET, Observe, IPv6 |
| `test_multicast_discover` | 41† | Discover slot management, expiry, handler dispatch, loopback integration |

*IPv6 socket tests skip gracefully when the host kernel has no IPv6.  
†Loopback-multicast integration test skips gracefully when `IP_ADD_MEMBERSHIP` is unavailable.

## API Reference

See [`API.md`](API.md) for the complete reference covering every class,
method, enum, struct, and configuration macro, with worked examples for every
board family.

## License

MIT — see [LICENSE](LICENSE).
