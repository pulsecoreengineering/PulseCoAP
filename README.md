# PulseCoAP

**No heap. Ever.** Full CoAP (RFC 7252) client and server for constrained IoT devices — works on 32 KB RAM MCUs with zero dynamic allocation.

Every buffer — messages, options, transactions, resources, observers, block-wise reassembly — is a fixed-size array controlled by a `PULSECOAP_MAX_*` compile-time constant. No `malloc`, no `new`, no surprises.

```sh
# No device needed — try it against a real CoAP server right now:
git clone https://github.com/pulsecoreengineering/PulseCoAP.git && cd PulseCoAP
g++ -std=c++11 -Isrc examples/PosixClient/PosixClient.cpp src/PulseCoAP*.cpp -o coap_client
./coap_client coap.me /hello
#   code:    2.05
#   payload: world
```

## Boards

| Transport | Targets |
|-----------|---------|
| `PulseCoAPTransportArduinoUDP.h` | ESP32, ESP8266, Arduino Uno/Mega/Nano, Raspberry Pi Pico W |
| `PulseCoAPTransportLwIP.h` | STM32, ESP-IDF, RP2040, any bare-metal lwIP (NO_SYS=1) |
| `PulseCoAPTransportPosix.h` | Linux, macOS, Raspberry Pi, Zephyr, NuttX, RIOT |
| `PulseCoAPTransportDTLS.h` | ESP32 + Arduino with mbedTLS (DTLS 1.2 / CoAPs) |

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
    server.begin(); // binds to CoAP default port 5683
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

Endpoint device; // fill ip[4] and port before use

void onResponse(const ClientResponse& res, void*) {
    Serial.write(res.payload, res.payloadLength);
    Serial.println();
}

void setup() {
    WiFi.begin("SSID", "pass");
    while (WiFi.status() != WL_CONNECTED) delay(200);

    device.ip[0]=192; device.ip[1]=168; device.ip[2]=1; device.ip[3]=42;
    device.port = 5683;

    client.begin();
    client.get(device, "/temp", onResponse);
}

void loop() {
    client.poll(millis());
}
```

### Observe — push notifications

```cpp
// Client: subscribe to /temp; callback fires on the initial value and every notify().
client.observe(device, "/temp", onTemp, &ctx);

// Server: push to all subscribers (CON mode prunes dead peers automatically).
server.notify("/temp",
              reinterpret_cast<const uint8_t*>(buf), strlen(buf),
              ContentFormat::TextPlain,
              /*confirmable=*/true);
```

### POSIX host (Linux / macOS / Raspberry Pi)

```cpp
#include "PulseCoAP.h"
#include "PulseCoAPTransportPosix.h"
using namespace pulsecoap;

PosixUdpTransport transport;
TransactionPool   txPool;
Client            client(transport, txPool);

// Same API as Arduino — only the transport changes.
client.begin();
client.get(ep, "/sensors/temp", onResponse);
while (true) { client.poll(nowMs()); usleep(1000); }
```

See [`examples/`](examples) for complete board-by-board examples (ESP8266, Uno+Ethernet, Pico W, STM32+lwIP, ESP-IDF, Zephyr, NuttX, Linux/macOS).

## Features

- **No heap, ever.** Fixed-size buffers sized at compile time by `PULSECOAP_MAX_*` constants. Works on AVR with 2 KB RAM.

- **Full RFC 7252 codec.** Header, 0–8 byte token, delta+length option encoding (both extended-byte cases), payload marker, strict malformed-input rejection.

- **Reliable delivery.** `TransactionPool` retransmits Confirmable messages with the RFC 7252 §4.8 exponential-backoff-plus-jitter schedule; fires a timeout callback after `MAX_RETRANSMIT` failures.

- **Observe (RFC 7641).** `server.notify()` pushes updates to every subscriber. CON notifications prune dead peers automatically; RST deregisters immediately.

- **Block-wise transfer (RFC 7959).** Opt in with `PULSECOAP_ENABLE_BLOCKWISE=1`. Server auto-fragments large GET responses; client reassembles. Client auto-fragments large PUT/POST; server reassembles into a fixed buffer before calling the handler.

- **URI-template paths.** `addResource("/sensors/:id", ...)` matches any single segment; `req.pathParam("id")` returns its value. Exact paths take priority with zero overhead.

- **Multiple simultaneous Observe subscriptions.** Each `(server, path)` pair tracked independently; `cancelObserve(server, path)` cancels one without affecting others.

- **Resource discovery** (`/.well-known/core`, RFC 6690). Auto-registered at `begin()`; returns CoRE Link Format with `;obs` and optional `;rt="..."` per resource.

- **Deferred (non-piggybacked) responses** (RFC 7252 §5.2.2). Set `res.deferred = true`, store `req.deferHandle`, then call `server.respond(handle, ...)` later. Empty ACK sent immediately to stop client retransmission.

- **Multicast resource discovery** (RFC 7252 §8). `client.discover()` sends a NON GET to `224.0.1.187:5683`; every PulseCoAP server on the LAN responds unicast. Slots expire after `PULSECOAP_DISCOVER_TIMEOUT_MS` (default 5 s).

- **DTLS 1.2 / CoAPs** (RFC 6347). PSK authentication, non-blocking handshake, fixed session pool, DTLS cookies for anti-amplification. Built on ESP32 core's mbedTLS — no extra libraries. Enable with `PULSECOAP_ENABLE_DTLS=1`.

- **IPv6.** `Endpoint` carries both `ip[4]` and `v6[16]` with an `isV6` flag. Existing `ep.ip[]` code compiles unchanged.

- **Both roles, compile-time selectable.** `PULSECOAP_ROLE_CLIENT_ONLY` / `PULSECOAP_ROLE_SERVER_ONLY` strips the half you don't need.

- **Standalone.** Zero dependencies on the rest of the Pulse ecosystem by default.

## Configuration

Every size limit and timing constant lives in [`src/PulseCoAPConfig.h`](src/PulseCoAPConfig.h) and can be overridden before `#include <PulseCoAP.h>`:

```cpp
#define PULSECOAP_MAX_MSG_SIZE       512  // larger payloads (default 256)
#define PULSECOAP_MAX_RESOURCES       16  // more registered paths (default 8)
#define PULSECOAP_ENABLE_BLOCKWISE     1  // block-wise transfer (default off)
#define PULSECOAP_ROLE_CLIENT_ONLY       // drop server code to save flash
#define PULSECOAP_ENABLE_DTLS          1  // DTLS/CoAPs — ESP32/Arduino only
#include <PulseCoAP.h>
#include <PulseCoAPTransportDTLS.h>      // after PulseCoAP.h when DTLS is on
```

**Arduino Uno / Mega (AVR — tight RAM):**

```cpp
#define PULSECOAP_MAX_MSG_SIZE      128
#define PULSECOAP_MAX_RESOURCES       4
#define PULSECOAP_MAX_OBSERVERS       4
#define PULSECOAP_MAX_TRANSACTIONS    2
#include <PulseCoAP.h>
```

See [`API.md`](API.md) for the full list of every `PULSECOAP_MAX_*` constant, its default, and the memory it controls.

## Testing

No Arduino toolchain needed — all suites build and run on a host `g++`/`clang++`:

```sh
PULSECOAP_SRC=src bash test/run_tests.sh
```

**392 checks across 8 suites** (DTLS is tested via Arduino examples — mbedTLS is not available on the host):

| Suite | Checks | What it covers |
|-------|--------|----------------|
| `test_message_codec` | 46 | Full RFC 7252 encode/decode, malformed-input rejection |
| `test_transaction_pool` | 28 | Exponential backoff, jitter, wraparound-safe timing |
| `test_client_server_integration` | 125 | GET, PUT, Observe, deferred responses, RST handling |
| `test_blockwise` | 29 | Block1 upload, Block2 fragmentation + reassembly |
| `test_path_templates` | 38 | URI template matching, exact-path priority |
| `test_multi_observe` | 35 | Multiple simultaneous observes, independent cancel |
| `test_posix_transport` | 50* | Real OS loopback sockets, GET, Observe, IPv6 |
| `test_multicast_discover` | 41† | Slot management, expiry, handler dispatch, loopback integration |

\*IPv6 socket tests skip gracefully when the host kernel has no IPv6.  
†Loopback-multicast integration test skips gracefully when `IP_ADD_MEMBERSHIP` is unavailable.

## HTTP ↔ CoAP Gateway

Need to bridge CoAP devices to a web application or dashboard? See
**[PulseCoAPGateway](https://github.com/pulsecoreengineering/PulseCoAPGateway)** —
a standalone POSIX-only library (Linux, macOS, Raspberry Pi) that proxies
HTTP/1.1 to CoAP and streams device notifications as Server-Sent Events.
PulseCoAP is its only dependency.

## API Reference

See [`API.md`](API.md) for the complete reference covering every class, method, enum, struct, and configuration macro, with worked examples for every board family.

## License

MIT — see [LICENSE](LICENSE).
