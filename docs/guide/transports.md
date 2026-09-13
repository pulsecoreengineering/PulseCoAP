# Transports & Platforms

PulseCoAP never calls a socket API directly. All network I/O goes through the `Transport` abstract class:

```cpp
class Transport {
public:
    virtual bool   begin(uint16_t localPort) = 0;
    virtual bool   send(const Endpoint& to, const uint8_t* data, size_t length) = 0;
    virtual size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) = 0;
};
```

This seam lets the same `Server` and `Client` code run on any platform, in any test environment, without any changes.

---

## ArduinoUdpTransport

**File:** `PulseCoAPTransportArduinoUDP.h`  
**Platforms:** ESP32, ESP8266, Arduino Uno/Mega + Ethernet shield, Arduino Nano 33, RP2040 (WiFi), any Arduino with a `UDP`-derived class

Only compiled under the Arduino framework (`#ifdef ARDUINO`). Wraps a caller-owned `UDP&` instance — PulseCoAP never constructs the underlying UDP object, so you pick the stack.

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>

WiFiUDP udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server server(transport);
// or
pulsecoap::TransactionPool txPool;
pulsecoap::Client client(transport, txPool);
```

To use `EthernetUDP` instead:

```cpp
#include <Ethernet.h>
EthernetUDP udp;
pulsecoap::ArduinoUdpTransport transport(udp);
```

**Note:** The Arduino `UDP` base class is currently IPv4 only. IPv6 requires the POSIX or lwIP transport.

---

## PosixUdpTransport

**File:** `PulseCoAPTransportPosix.h`  
**Platforms:** Linux, macOS, Raspberry Pi, POSIX-compliant RTOS (Zephyr + POSIX API, NuttX, RIOT)

Opens a non-blocking dual-stack UDP socket (`AF_INET6` with `IPV6_V6ONLY=0`) that handles both IPv4 and IPv6 in one socket. Falls back to `AF_INET` on kernels without IPv6.

```cpp
#include "PulseCoAPTransportPosix.h"
using namespace pulsecoap;

PosixUdpTransport transport;
Server server(transport);

// In setup:
if (!server.begin(5683)) {
    fprintf(stderr, "failed to bind port 5683\n");
    return 1;
}

// In loop:
while (true) {
    server.poll(nowMs());
    usleep(1000);
}
```

**Multicast** (for `Client::discover()`): call `joinMulticastGroup()` after `begin()`:

```cpp
transport.joinMulticastGroup("224.0.1.187");  // IANA CoAP all-nodes address
```

**Build** (from the PulseCoAP repo root, no extra flags needed):

```sh
g++ -std=c++11 -Isrc \
    your_app.cpp \
    src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp \
    src/PulseCoAPServer.cpp  src/PulseCoAPClient.cpp \
    -o my_app
```

---

## LwIpUdpTransport

**File:** `PulseCoAPTransportLwIP.h`  
**Platforms:** Bare-metal lwIP 2.x targets (STM32 + CubeMX, ESP-IDF native, RP2040 with SDK lwIP, custom RTOS)

Auto-detected when `LWIP_RAW` is defined in the lwIP include path; or opt in explicitly with `-DPULSECOAP_TRANSPORT_LWIP=1`.

```cpp
#include "PulseCoAPTransportLwIP.h"
using namespace pulsecoap;

LwIpUdpTransport transport;
Server server(transport);
```

**Threading model:** assumes cooperative single-threaded operation (`NO_SYS=1`) or an RTOS where lwIP and the application share one task. Call `sys_check_timeouts()` before `server.poll()` / `client.poll()`.

Incoming datagrams are enqueued in a fixed ring of `PULSECOAP_LWIP_RX_SLOTS` slots (default 4). If a burst fills the ring before `poll()` drains it, the oldest datagram is silently dropped — CoAP's retransmission layer recovers.

```cpp
// Raise the ring size if your loop can run slow:
#define PULSECOAP_LWIP_RX_SLOTS 8
#include "PulseCoAPTransportLwIP.h"
```

**IPv6:** when lwIP is built with `LWIP_IPV6=1`, `LwIpUdpTransport` creates a dual-stack PCB and demaps incoming IPv4-mapped addresses — same semantics as the POSIX adapter.

---

## DtlsTransport

**File:** `PulseCoAPTransportDTLS.h`  
**Platforms:** ESP32 Arduino (ships mbedTLS), any platform with mbedTLS 2.x/3.x  
**Requires:** `#define PULSECOAP_ENABLE_DTLS 1` before `#include <PulseCoAP.h>`

Wraps mbedTLS DTLS 1.2 (RFC 6347). PulseCoAP's own CoAP code stays zero-heap; the mbedTLS handshake itself allocates internally (ESP32 Arduino uses `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`).

Only PSK (pre-shared key) is supported. Certificates (PKI) are not in scope for the current version.

```cpp
#define PULSECOAP_ENABLE_DTLS 1
#include <PulseCoAP.h>

// ── Server side ──────────────────────────────────────────────────────────────
WiFiUDP udp;
pulsecoap::DtlsTransport transport(udp);
pulsecoap::Server server(transport);

// Register a PSK: identity (ASCII) + secret (raw bytes)
const uint8_t psk[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                        0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
transport.addPsk("sensor-node-01", psk, sizeof(psk));

void setup() {
    server.begin(5684);  // standard CoAPs port
}

// ── Client side ──────────────────────────────────────────────────────────────
WiFiUDP clientUdp;
pulsecoap::DtlsTransport clientTransport(clientUdp);
clientTransport.setClientPsk("sensor-node-01", psk, sizeof(psk));

pulsecoap::TransactionPool txPool;
pulsecoap::Client client(clientTransport, txPool);
```

> **Handshake timing:** the first request to a new server triggers a DTLS handshake. The request's response arrives only after the handshake completes — typically a few hundred milliseconds on a LAN. Subsequent requests to the same server reuse the session.

See [Configuration](configuration.md) for DTLS-related macros (`PULSECOAP_DTLS_MAX_SESSIONS`, `PULSECOAP_DTLS_HANDSHAKE_TIMEOUT_MS`, etc.).

---

## Writing a custom transport

Implement the three-method `Transport` interface:

```cpp
#include "PulseCoAPTransport.h"

class MyTransport : public pulsecoap::Transport {
public:
    bool begin(uint16_t localPort) override {
        // Bind your UDP socket to localPort.
        return true;
    }

    bool send(const pulsecoap::Endpoint& to, const uint8_t* data, size_t length) override {
        // Send length bytes to to.ip / to.port.
        // Return false only on a hard send failure; CoAP handles loss.
        return true;
    }

    size_t receive(uint8_t* buffer, size_t capacity, pulsecoap::Endpoint& from) override {
        // Non-blocking: if a datagram is waiting, copy up to `capacity` bytes
        // into buffer, fill `from`, return the datagram size.
        // Return 0 if nothing is waiting.
        return 0;
    }
};
```

Rules:
- `receive()` must be **non-blocking**. Poll-based: return 0 immediately when the queue is empty.
- `send()` is fire-and-forget at the transport level. CoAP's `TransactionPool` handles loss through retransmission.
- `begin()` is called once. `receive()` and `send()` may be called multiple times per `poll()` tick.
- Thread safety is the caller's responsibility. If multiple tasks share a transport, protect it with a mutex.

---

## Platform summary

| Platform | Transport | IPv6 | Multicast | DTLS |
|---|---|---|---|---|
| Arduino / ESP32 WiFiUDP | `ArduinoUdpTransport` | No | WiFiUDP-dependent | Yes (DtlsTransport) |
| Arduino + EthernetUDP | `ArduinoUdpTransport` | No | No | No |
| Linux / macOS / Raspberry Pi | `PosixUdpTransport` | Yes | Yes | No |
| Bare-metal lwIP | `LwIpUdpTransport` | Yes (LWIP_IPV6=1) | No | No |
| ESP-IDF native | `LwIpUdpTransport` | Yes | No | No |
| Zephyr / NuttX (POSIX API) | `PosixUdpTransport` | Yes | Yes | No |
| Custom / test double | Implement `Transport` | Depends | Depends | Depends |
