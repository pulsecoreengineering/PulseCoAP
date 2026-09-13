# PulseCoAP

CoAP (RFC 7252) client and server for embedded devices. No dynamic allocation — not "mostly static" with a few hidden mallocs, genuinely none. Every buffer is a fixed array sized at compile time, so you know exactly how much RAM you're spending before the first byte is sent.

Works on anything from an Arduino Uno (2 KB RAM) to an ESP32 to a Raspberry Pi. Same API everywhere; only the transport header changes.

```sh
# No device required — hit a real CoAP server from your laptop:
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

### Server (ESP32)

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
    server.begin();
}

void loop() {
    server.poll(millis());
}
```

### Client (ESP32)

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

Endpoint device;

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

### Observe (push notifications)

Instead of polling, the server pushes updates to whoever subscribed:

```cpp
// Client side — fires on the initial value and every notify() after that
client.observe(device, "/temp", onTemp, &ctx);

// Server side — one call pushes to all subscribers
server.notify("/temp",
              reinterpret_cast<const uint8_t*>(buf), strlen(buf),
              ContentFormat::TextPlain,
              /*confirmable=*/true); // CON mode drops dead subscribers automatically
```

### POSIX (Linux / macOS / Raspberry Pi)

```cpp
#include "PulseCoAP.h"
#include "PulseCoAPTransportPosix.h"
using namespace pulsecoap;

PosixUdpTransport transport;
TransactionPool   txPool;
Client            client(transport, txPool);

client.begin();
client.get(ep, "/sensors/temp", onResponse);
while (true) { client.poll(nowMs()); usleep(1000); }
```

See [`examples/`](examples) for complete working projects for each board family.

## What's included

**Full RFC 7252 codec** — header, 0–8 byte token, delta+length option encoding (both extended-byte cases), payload marker. Strict rejection of malformed input; no silent truncation.

**Reliable delivery** — the `TransactionPool` retransmits Confirmable messages using the RFC 7252 §4.8 backoff-plus-jitter schedule and calls a timeout handler after `MAX_RETRANSMIT` failures. Jitter is real (not faked), so multiple devices don't all retry in sync.

**Observe (RFC 7641)** — register a resource as observable and call `server.notify()` to push to all subscribers. CON-mode notifications prune dead peers after repeated failures; RST frames deregister immediately.

**Block-wise transfer (RFC 7959)** — opt in with `PULSECOAP_ENABLE_BLOCKWISE=1`. Large GET responses are fragmented automatically; large PUT/POST uploads are reassembled before the handler is called. All into fixed buffers, no heap.

**URI templates** — `addResource("/sensors/:id", ...)` matches any single path segment. `req.pathParam("id")` gives you the value. Exact paths still take priority and have no extra overhead.

**Multiple simultaneous Observe subscriptions** — the client tracks each `(server, path)` pair independently. `cancelObserve(server, path)` cancels one without touching the others on the same server.

**Resource discovery** (`/.well-known/core`, RFC 6690) — auto-registered at `begin()`. Returns CoRE Link Format with `;obs` on observable resources and optional `;rt="..."` if you set it.

**Deferred responses** (RFC 7252 §5.2.2) — for handlers that need to wait on I2C, a slow sensor, etc. Set `res.deferred = true`, save the handle, send the response later with `server.respond()`. An empty ACK goes out immediately so the client stops retransmitting.

**Multicast resource discovery** (RFC 7252 §8) — `client.discover()` sends a NON GET to `224.0.1.187:5683`. Every PulseCoAP server on the LAN responds unicast and fires your `DiscoverHandler`. Slots expire automatically after `PULSECOAP_DISCOVER_TIMEOUT_MS`.

**DTLS 1.2 / CoAPs** (RFC 6347) — pre-shared key auth, non-blocking handshake, server-side DTLS cookies. Built on the ESP32 Arduino core's mbedTLS; no extra libraries or sdkconfig changes. Enable with `PULSECOAP_ENABLE_DTLS=1`. (Note: mbedTLS session state itself uses the heap — that's mbedTLS's constraint, not ours.)

**IPv6** — `Endpoint` has both `ip[4]` and `v6[16]` with an `isV6` flag. Existing code that only uses `ep.ip[]` still compiles and runs fine.

## Configuration

All buffer sizes and timing constants are in [`src/PulseCoAPConfig.h`](src/PulseCoAPConfig.h). Override them with a `#define` before your first `#include`:

```cpp
#define PULSECOAP_MAX_MSG_SIZE       512  // default 256
#define PULSECOAP_MAX_RESOURCES       16  // default 8
#define PULSECOAP_ENABLE_BLOCKWISE     1  // default off
#define PULSECOAP_ROLE_CLIENT_ONLY       // drop server code entirely
#include <PulseCoAP.h>
```

On an Uno or Mega, shrink the defaults or you'll run out of RAM fast:

```cpp
#define PULSECOAP_MAX_MSG_SIZE      128
#define PULSECOAP_MAX_RESOURCES       4
#define PULSECOAP_MAX_OBSERVERS       4
#define PULSECOAP_MAX_TRANSACTIONS    2
#include <PulseCoAP.h>
```

Every constant is documented in [`src/PulseCoAPConfig.h`](src/PulseCoAPConfig.h) with its default value and what it controls. [`API.md`](API.md) has the full list.

## Testing

No Arduino toolchain needed — all suites compile and run on a host `g++` or `clang++`:

```sh
PULSECOAP_SRC=src bash test/run_tests.sh
```

392 checks across 8 suites. DTLS is covered by the Arduino examples (mbedTLS isn't available on the host):

| Suite | Checks | What it covers |
|-------|--------|----------------|
| `test_message_codec` | 46 | RFC 7252 encode/decode, malformed-input rejection |
| `test_transaction_pool` | 28 | Backoff, jitter, wraparound-safe timing |
| `test_client_server_integration` | 125 | GET, PUT, Observe, deferred responses, RST handling |
| `test_blockwise` | 29 | Block1 upload, Block2 fragmentation + reassembly |
| `test_path_templates` | 38 | URI template matching, exact-path priority |
| `test_multi_observe` | 35 | Multiple simultaneous observes, independent cancel |
| `test_posix_transport` | 50* | Real OS loopback sockets, GET, Observe, IPv6 |
| `test_multicast_discover` | 41† | Slot management, expiry, handler dispatch, loopback |

\*IPv6 tests skip gracefully if the host kernel has no IPv6.  
†Multicast loopback test skips if `IP_ADD_MEMBERSHIP` is unavailable.

## Gateway

If you need to expose CoAP devices to a web app or dashboard, take a look at
[PulseCoAPGateway](https://github.com/pulsecoreengineering/PulseCoAPGateway).
It's a separate POSIX-only library that proxies HTTP/1.1 to CoAP and streams
device notifications as Server-Sent Events. PulseCoAP is its only dependency.

## API Reference

[`API.md`](API.md) covers every class, method, enum, struct, and config macro with examples for each board family.

## License

MIT — see [LICENSE](LICENSE).
