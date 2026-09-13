# PulseCoAP

**No-heap CoAP for embedded systems.**  
RFC 7252 compliant. Runs on ESP32, Arduino, STM32, RP2040, and any POSIX host.

---

## Why CoAP?

HTTP is heavy — a single request costs hundreds of bytes of headers plus a full TCP handshake. MQTT needs a broker in the middle. CoAP (RFC 7252) is what you reach for when you want REST semantics, sub-100-byte messages, and direct device-to-device communication over UDP.

The protocol maps cleanly to embedded firmware: a temperature sensor is a resource (`/sensors/temp`), a relay is a resource (`/actuators/relay`). GET it, PUT it, subscribe to it with Observe and get a push every time it changes — no broker, no polling, no wasted wake cycles on a battery device.

```cpp
#include <PulseCoAP.h>

WiFiUDP udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server server(transport);

void handleTemp(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[8];
    dtostrf(readTempC(), 0, 1, buf);
    res.setPayload(buf);
}

void setup() {
    // ... WiFi connect ...
    server.addResource("/sensors/temp", pulsecoap::MethodGet, handleTemp,
                        nullptr, /*observable=*/true);
    server.begin(5683);
}

void loop() {
    server.poll(millis());

    // Push a reading to every Observe subscriber every 5 seconds.
    if (millis() - lastNotify >= 5000) {
        char buf[8]; dtostrf(readTempC(), 0, 1, buf);
        server.notify("/sensors/temp", (uint8_t*)buf, strlen(buf));
        lastNotify = millis();
    }
}
```

## Features

| Feature | Detail |
|---|---|
| **Server** | Resource registration with exact or URI-template paths (`/sensors/:id`); GET, PUT, POST, DELETE, FETCH, PATCH |
| **Client** | GET, PUT, POST, DELETE with confirmable retransmission; callback-per-response |
| **Observe** (RFC 7641) | Server `notify()` pushes updates to all subscribers; client `observe()` / `cancelObserve()` |
| **Separate responses** | Handler sets `res.deferred = true`, saves `req.deferHandle`, calls `server.respond()` later |
| **Resource discovery** | Auto-registered `/.well-known/core` (RFC 6690); client-side `discover()` with multicast |
| **Block-wise transfer** | RFC 7959 Block1/Block2 for payloads that don't fit a single datagram (opt-in) |
| **DTLS** | RFC 6347 / CoAPs (RFC 7252 §9) over mbedTLS, PSK mode (opt-in) |
| **Zero heap** | Every buffer is a fixed-size array; no `malloc`, no `new` anywhere in the library |

---

## Memory footprint

Defaults (`PULSECOAP_MAX_MSG_SIZE 256`, `PULSECOAP_MAX_RESOURCES 8`, `PULSECOAP_MAX_OBSERVERS 8`, `PULSECOAP_MAX_TRANSACTIONS 4`):

| Role | Platform | Flash (approx.) | RAM (approx.) |
|---|---|---|---|
| Server only | ESP32 / ARM Cortex-M | ~5.5 KB | ~3.0 KB |
| Client only | ESP32 / ARM Cortex-M | ~4.0 KB | ~2.5 KB |
| Server + Client | ESP32 / ARM Cortex-M | ~8.0 KB | ~5.0 KB |
| Server only | AVR (ATmega328P) | ~6.5 KB | ~1.8 KB |

Raise `PULSECOAP_MAX_MSG_SIZE` for bigger payloads. Reduce `PULSECOAP_MAX_RESOURCES` and `PULSECOAP_MAX_OBSERVERS` to shrink RAM on tight targets. See [Configuration](guide/configuration.md) for the full list.

---

## Limitations

- **No dynamic resources**: `addResource()` is called once in `setup()` and the table is fixed for the program's lifetime. This is intentional — it keeps the footprint static.
- **Single-threaded poll model**: `server.poll()` / `client.poll()` must be called from the same task that registered the resources. On ESP32 with FreeRTOS, keep everything on Core 0 or add your own mutex.
- **DTLS requires mbedTLS**: the PSK handshake itself uses mbedTLS heap internally (ESP32 Arduino ships with `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`). PulseCoAP's own message/routing code stays zero-heap regardless.
- **Block-wise transfer is opt-in**: off by default. Enable with `#define PULSECOAP_ENABLE_BLOCKWISE 1` before `#include <PulseCoAP.h>`.
- **IPv6**: `Endpoint` carries a full 16-byte `v6[]` field and the POSIX transport handles dual-stack. The Arduino transport is currently IPv4 only (WiFiUDP limitation).

---

## Get started

- [Concepts](guide/concepts.md) — what CoAP is and how PulseCoAP models it
- [Quick Start](guide/quickstart.md) — a working server in 30 lines, then a client, then Observe
- [Configuration](guide/configuration.md) — every `PULSECOAP_*` macro documented
- [Transports & Platforms](guide/transports.md) — WiFiUDP, lwIP, POSIX, DTLS, custom transport

## Use cases

Real-world patterns, fully worked:

- [Sensor + LED Dashboard](use-cases/sensor-dashboard.md) — observable temperature + writable LED, wired to the PulseCoAP Gateway
- [Observe — Live Push](use-cases/observe.md) — server notifies multiple clients simultaneously; client cancels independently
- [Deferred Response](use-cases/deferred.md) — handler ACKs immediately, replies later after slow work
- [URI Templates](use-cases/uri-templates.md) — one handler covers `/sensors/0`, `/sensors/1`, `/sensors/2` …
- [Resource Discovery](use-cases/discovery.md) — `/.well-known/core` and LAN multicast discovery
