# PulseCoAP API Reference

**Version:** 1.1.0  
**Standard:** RFC 7252 (CoAP), RFC 7641 (Observe), RFC 7959 (Block-wise), RFC 6690 (CoRE Link Format), RFC 6347 (DTLS), RFC 7252 §9 (CoAPs), RFC 7252 §8 (Multicast Discovery)

---

## Contents

1. [Quick Start](#1-quick-start)
2. [Configuration Macros](#2-configuration-macros)
3. [Types and Enumerations](#3-types-and-enumerations)
4. [Transport Layer](#4-transport-layer)
   - [Endpoint](#endpoint)
   - [Transport interface](#transport-interface)
   - [ArduinoUdpTransport](#arduinoudptransport)
   - [LwIpUdpTransport](#lwipudptransport)
   - [PosixUdpTransport](#posixudptransport)
   - [DtlsTransport](#dtlstransport)
5. [TransactionPool](#5-transactionpool)
6. [Server](#6-server)
   - [MethodMask](#methodmask)
   - [DeferHandle](#deferhandle)
   - [Request](#request)
   - [Response](#response)
   - [ResourceHandler](#resourcehandler)
   - [Server class](#server-class)
7. [Client](#7-client)
   - [ClientResponse](#clientresponse)
   - [ResponseHandler](#responsehandler)
   - [TimeoutHandler](#timeouthandler)
   - [DiscoverHandler](#discoverhandler)
   - [Client class](#client-class)
8. [Code Enum Reference](#8-code-enum-reference)
9. [ContentFormat Enum Reference](#9-contentformat-enum-reference)
10. [Complete Macro Reference](#10-complete-macro-reference)

---

## 1. Quick Start

### Minimal server (Arduino / ESP32)

```cpp
#include <WiFiUDP.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

using namespace pulsecoap;

WiFiUDP               udp;
ArduinoUdpTransport   transport(udp);
Server                server(transport);

void tempHandler(const Request& req, Response& res, void*) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%.1f", readTempC());
    res.code = Code::Content;
    res.setPayload(buf);
}

void setup() {
    WiFi.begin("ssid", "pass");
    while (WiFi.status() != WL_CONNECTED) delay(100);

    server.addResource("/temp", MethodGet, tempHandler, nullptr, /*observable=*/true);
    server.begin(); // binds to CoAP default port 5683
}

void loop() {
    server.poll(millis());

    // Push to all observers every 5 s
    static uint32_t last = 0;
    if (millis() - last >= 5000) {
        last = millis();
        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", readTempC());
        server.notify("/temp", reinterpret_cast<const uint8_t*>(buf), strlen(buf));
    }
}
```

### Minimal client (Linux / macOS)

```cpp
#include <PulseCoAP.h>
#include <PulseCoAPTransportPosix.h>
#include <cstdio>

using namespace pulsecoap;

void onResponse(const ClientResponse& res, void* /*ctx*/) {
    printf("Response %d.%02d: %.*s\n",
           codeClass(static_cast<uint8_t>(res.code)),
           codeDetail(static_cast<uint8_t>(res.code)),
           static_cast<int>(res.payloadLength),
           res.payload);
}

int main() {
    PosixUdpTransport transport;
    TransactionPool   txPool;
    Client            client(transport, txPool);

    client.begin(); // ephemeral port

    Endpoint server;
    server.ip[0]=192; server.ip[1]=168; server.ip[2]=1; server.ip[3]=42;
    server.port = 5683;

    client.get(server, "/temp", onResponse);

    for (int i = 0; i < 50; ++i) {          // poll for ~500 ms
        client.poll(static_cast<uint32_t>(i * 10));
        usleep(10000);
    }
}
```

---

## 2. Configuration Macros

All macros are defined in `PulseCoAPConfig.h` and can be overridden by adding a `#define` before the first `#include <PulseCoAP.h>` or via a compiler flag (`-DPULSECOAP_MAX_MSG_SIZE=512`). PulseCoAP never allocates from the heap; every buffer is a fixed-size array sized by these constants.

### Message Sizing

| Macro | Default | Description |
|-------|---------|-------------|
| `PULSECOAP_MAX_MSG_SIZE` | `256` | Maximum byte length of any single CoAP message (header + token + options + payload). Raise if you need large payloads without block-wise transfer. |
| `PULSECOAP_MAX_OPTIONS` | `16` | Maximum number of options indexed per message. 16 covers Uri-Path, Content-Format, Observe, ETag, and Block1/Block2 in one message. |
| `PULSECOAP_MAX_TOKEN_LEN` | `8` | Maximum token length in bytes. RFC 7252 fixes the maximum at 8; this macro exists for documentation and `static_assert` use. |

### Reliability Layer

| Macro | Default | Description |
|-------|---------|-------------|
| `PULSECOAP_MAX_TRANSACTIONS` | `4` | Maximum in-flight Confirmable messages (client + server each have their own `TransactionPool`). Fixed pool, no heap. |
| `PULSECOAP_ACK_TIMEOUT_MS` | `2000` | RFC 7252 §4.8 `ACK_TIMEOUT`: base retransmission interval in milliseconds. The first actual timeout is uniformly randomised in `[ACK_TIMEOUT, ACK_TIMEOUT × ACK_RANDOM_FACTOR]`. |
| `PULSECOAP_ACK_RANDOM_FACTOR_NUM` | `3` | Numerator of the jitter factor fraction. With the denominator below, this gives RFC default 1.5. |
| `PULSECOAP_ACK_RANDOM_FACTOR_DEN` | `2` | Denominator of the jitter factor fraction. Integer-only arithmetic so no `libm` is pulled in. |
| `PULSECOAP_MAX_RETRANSMIT` | `4` | RFC 7252 §4.8 `MAX_RETRANSMIT`. A CON message is retransmitted up to this many times before it is declared timed out. |

### Server Sizing

| Macro | Default | Description |
|-------|---------|-------------|
| `PULSECOAP_MAX_RESOURCES` | `8` | Maximum number of resources (paths) the server can register, including the auto-registered `/.well-known/core`. |
| `PULSECOAP_MAX_OBSERVERS` | `8` | Maximum number of simultaneous Observe subscribers across all observable resources. |
| `PULSECOAP_MAX_URI_PATH_LEN` | `64` | Maximum byte length (including NUL) of a resource path stored internally. |
| `PULSECOAP_MAX_DEFERRED` | `2` | Maximum number of requests the server can hold in the deferred state simultaneously (i.e. handler called `res.deferred = true` but `respond()` has not been called yet). |
| `PULSECOAP_MAX_LINK_FORMAT_LEN` | `256` | Byte capacity of the CoRE Link Format body serialised by `GET /.well-known/core`. One entry costs roughly `len(path) + 10` bytes. |

### Role Selection

Define **at most one** of these to exclude the other half of the library from the build:

```cpp
#define PULSECOAP_ROLE_CLIENT_ONLY  // removes Server, Block1Session, observer tables
#define PULSECOAP_ROLE_SERVER_ONLY  // removes Client, Block2RecvState, pending-request table
```

Defining both is a compile error. Defining neither (the default) compiles both roles.

### Optional Modules

| Macro | Default | Description |
|-------|---------|-------------|
| `PULSECOAP_ENABLE_BLOCKWISE` | `0` | Set to `1` to enable block-wise transfer (RFC 7959). Adds Block1 server reassembly and Block2 server fragmentation, plus Block1 client upload and Block2 client reassembly. Off by default because most constrained devices fit their payloads in one datagram. |
| `PULSECOAP_BLOCK_SZX` | `4` | Block size exponent. Actual block size = 2^(SZX+4) bytes. SZX 4 → 256 B, 3 → 128 B, 2 → 64 B. Must satisfy: `2^(SZX+4) + ~20 B header overhead ≤ PULSECOAP_MAX_MSG_SIZE`. |
| `PULSECOAP_MAX_BLOCK1_SESSIONS` | `2` | (Server) Maximum simultaneous Block1 upload reassembly sessions. |
| `PULSECOAP_BLOCK1_MAX_BODY` | `1024` | (Server) Byte capacity of each Block1 reassembly buffer. |
| `PULSECOAP_BLOCK2_MAX_BODY` | `1024` | (Client) Byte capacity of the Block2 receive-side reassembly buffer per pending request. |

### URI Template Parameters

| Macro | Default | Description |
|-------|---------|-------------|
| `PULSECOAP_MAX_PATH_PARAMS` | `4` | Maximum number of `:param` captures per matched request. |
| `PULSECOAP_MAX_PATH_PARAM_LEN` | `16` | Maximum byte length (including NUL) of one captured parameter name or value. |

### DTLS (CoAP over DTLS, RFC 6347 / RFC 7252 §9)

Requires `PULSECOAP_ENABLE_DTLS=1` and the Arduino framework with mbedTLS. Include `PulseCoAPTransportDTLS.h` after enabling this macro.

| Macro | Default | Description |
|-------|---------|-------------|
| `PULSECOAP_ENABLE_DTLS` | `0` | Set to `1` to compile `DtlsTransport`. Requires Arduino + ESP32 mbedTLS. |
| `PULSECOAP_DTLS_MAX_SESSIONS` | `4` | Maximum simultaneous DTLS sessions. Each slot holds one `mbedtls_ssl_context` plus a receive scratch buffer. |
| `PULSECOAP_DTLS_MAX_PSK_ENTRIES` | `8` | Size of the static PSK key store (identity + secret pairs). |
| `PULSECOAP_DTLS_IDENTITY_MAX_LEN` | `32` | Maximum byte length (including NUL) of a PSK identity string. |
| `PULSECOAP_DTLS_PSK_MAX_LEN` | `32` | Maximum byte length of a PSK secret (raw bytes). |
| `PULSECOAP_DTLS_HANDSHAKE_TIMEOUT_MS` | `10000` | Maximum time (ms) to complete a DTLS handshake before the session is abandoned. |

### Tracing

| Macro | Default | Description |
|-------|---------|-------------|
| `PULSECOAP_ENABLE_TRACE` | `0` | Reserved for future PulseTrace integration. No hook calls exist yet; setting this to 1 has no effect in the current release. |

---

## 3. Types and Enumerations

Header: `src/PulseCoAPTypes.h` (pulled in automatically via `PulseCoAP.h`)

### MessageType

```cpp
enum class MessageType : uint8_t {
    Confirmable     = 0,  // CON — must be ACK'd; retransmitted if lost
    NonConfirmable  = 1,  // NON — fire-and-forget; no ACK
    Acknowledgement = 2,  // ACK — confirms receipt of a CON
    Reset           = 3   // RST — rejects or deregisters a CON/NON
};
```

Handlers generally don't read `MessageType` directly; the server and client deal with message typing transparently. It is exposed for code that inspects raw `Message` objects.

### Code

`Code` is the CoAP response/request code. Values are `(class.detail)` per RFC 7252 §3.

```cpp
enum class Code : uint8_t {
    // Empty (0.00) — used for empty ACK and RST
    Empty   = 0x00,

    // Methods (0.xx) — sent by clients in requests
    Get     = 0x01,   Post    = 0x02,
    Put     = 0x03,   Delete  = 0x04,

    // 2.xx Success — sent by servers in responses
    Created = 0x41,   Deleted  = 0x42,
    Valid   = 0x43,   Changed  = 0x44,
    Content = 0x45,   Continue = 0x5F,   // Block-wise 2.31

    // 4.xx Client Error
    BadRequest              = 0x80,   Unauthorized            = 0x81,
    BadOption               = 0x82,   Forbidden               = 0x83,
    NotFound                = 0x84,   MethodNotAllowed        = 0x85,
    NotAcceptable           = 0x86,   RequestEntityIncomplete = 0x88,
    PreconditionFailed      = 0x8C,   RequestEntityTooLarge   = 0x8D,
    UnsupportedContentFormat = 0x8F,

    // 5.xx Server Error
    InternalServerError  = 0xA0,   NotImplemented       = 0xA1,
    BadGateway           = 0xA2,   ServiceUnavailable   = 0xA3,
    GatewayTimeout       = 0xA4,   ProxyingNotSupported = 0xA5
};
```

See [Section 8](#8-code-enum-reference) for a complete table with dotted notation.

### ContentFormat

```cpp
enum class ContentFormat : uint16_t {
    TextPlain   = 0,    // text/plain; charset=utf-8
    LinkFormat  = 40,   // application/link-format (RFC 6690)
    Xml         = 41,   // application/xml
    OctetStream = 42,   // application/octet-stream
    Exi         = 47,   // application/exi
    Json        = 50,   // application/json
    Cbor        = 60    // application/cbor
};
```

### Helper Functions

```cpp
// Decompose a Code or raw byte into class (0-7) and detail (0-31).
uint8_t codeClass (uint8_t rawCode);   // e.g. 2 for Content (0x45)
uint8_t codeDetail(uint8_t rawCode);   // e.g. 5 for Content (0x45)

// Returns true if the code is a method (GET, PUT, POST, DELETE, ...).
bool isMethodCode(Code code);
```

---

## 4. Transport Layer

### Endpoint

Header: `src/PulseCoAPTransport.h`

An `Endpoint` identifies a UDP peer (address + port). It supports both IPv4 and IPv6 in a single struct. All existing code that uses `ep.ip[0]…ip[3]` continues to compile unchanged after the IPv6 addition.

```cpp
struct Endpoint {
    uint8_t  ip[4]  = {0,0,0,0};  // IPv4 address, MSB first; valid when isV6 == false
    uint8_t  v6[16] = {};          // IPv6 address, MSB first (RFC 4291); valid when isV6 == true
    bool     isV6   = false;       // false = IPv4, true = IPv6
    uint16_t port   = 0;

    bool operator==(const Endpoint& other) const;
    bool operator!=(const Endpoint& other) const;
};
```

`operator==` is family-aware: an IPv4 and an IPv6 endpoint with identical ports are **never** equal.

**Build an IPv4 endpoint:**
```cpp
Endpoint srv;
srv.ip[0]=192; srv.ip[1]=168; srv.ip[2]=1; srv.ip[3]=100;
srv.port = 5683;
// srv.isV6 == false by default
```

**Build an IPv6 endpoint:**
```cpp
Endpoint srv;
srv.isV6   = true;
srv.v6[15] = 1;    // ::1 (loopback)
srv.port   = 5683;
// All other v6 bytes default to 0
```

---

### Transport Interface

Header: `src/PulseCoAPTransport.h`

All platform adapters inherit from `Transport`. Implement this interface to port PulseCoAP to any UDP stack.

```cpp
class Transport {
public:
    virtual ~Transport() = default;

    // One-time initialisation. Binds to localPort on all interfaces.
    // Pass 0 to let the OS assign an ephemeral port.
    // Returns false if the socket cannot be opened or the port is in use.
    virtual bool begin(uint16_t localPort) = 0;

    // Sends one UDP datagram to `to`. Returns false only if the datagram
    // could not be queued (transport-level failure). Reliability is handled
    // at the CoAP layer by TransactionPool — the transport is fire-and-forget.
    virtual bool send(const Endpoint& to, const uint8_t* data, size_t length) = 0;

    // Non-blocking receive: if a datagram is waiting, copies up to `capacity`
    // bytes into `buffer`, fills `from` with the sender's address, and returns
    // the datagram length. Returns 0 if nothing is waiting. Must never block.
    virtual size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) = 0;
};
```

---

### ArduinoUdpTransport

Header: `src/PulseCoAPTransportArduinoUDP.h`

Wraps any Arduino `UDP` subclass (`WiFiUDP`, `EthernetUDP`, `AsyncUDP`, etc.). Header-only. Suitable for all Arduino/ESP32 targets.

```cpp
class ArduinoUdpTransport : public Transport {
public:
    // `udp` must outlive this adapter. Pass any UDP& subclass.
    explicit ArduinoUdpTransport(UDP& udp);

    bool   begin(uint16_t localPort) override;
    bool   send(const Endpoint& to, const uint8_t* data, size_t length) override;
    size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) override;
};
```

**Example:**
```cpp
#include <WiFiUDP.h>
#include <PulseCoAPTransportArduinoUDP.h>

WiFiUDP             udp;
ArduinoUdpTransport transport(udp);
Server              server(transport);

void setup() {
    // ... connect WiFi ...
    server.begin(5683);
}
void loop() {
    server.poll(millis());
}
```

**Note:** The `ArduinoUdpTransport` supports IPv4 only. ESP32 targets with IPv6-capable stacks can use `LwIpUdpTransport` instead.

---

### LwIpUdpTransport

Header: `src/PulseCoAPTransportLwIP.h`

Bare-metal lwIP 2.x raw UDP adapter for MCUs running lwIP directly (STM32+LwIP, RP2040 with lwIP, custom RTOS stacks). Header-only. Compiled only when `PULSECOAP_TRANSPORT_LWIP` is defined or `LWIP_RAW` is already defined by the lwIP headers.

```cpp
class LwIpUdpTransport : public Transport {
public:
    LwIpUdpTransport();   // initialises internal ring and PCB pointer
    ~LwIpUdpTransport();  // calls udp_remove() on the PCB

    bool   begin(uint16_t localPort) override;
    bool   send(const Endpoint& to, const uint8_t* data, size_t length) override;
    size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) override;
};
```

**Configuration knobs specific to this adapter:**

| Macro | Default | Description |
|-------|---------|-------------|
| `PULSECOAP_LWIP_RX_SLOTS` | `4` | Number of receive ring slots. If a burst of datagrams arrives faster than `poll()` drains the ring, the oldest slot is silently dropped (CoAP retransmission handles the loss). |

**IPv6:** When lwIP is compiled with `LWIP_IPV6=1`, `begin()` creates a dual-stack PCB via `udp_new_ip_type(IPADDR_TYPE_ANY)`. Both IPv4 and IPv6 datagrams are received and demultiplexed into `Endpoint` with the correct `isV6` flag.

**Threading model:** Assumes cooperative single-threaded execution (`NO_SYS=1`) or an RTOS where lwIP and the application run in the same task. If lwIP runs in a separate RTOS task, protect the ring buffer with a mutex.

**Usage pattern:**
```cpp
#include <PulseCoAPTransportLwIP.h>

LwIpUdpTransport transport;
Server           server(transport);

void app_main() {
    // lwIP stack must already be initialised.
    server.begin(5683);

    while (1) {
        sys_check_timeouts();     // drive lwIP timers
        netif_input();            // feed received packets to lwIP (board-specific)
        server.poll(sys_now());   // dispatch CoAP requests
    }
}
```

---

### PosixUdpTransport

Header: `src/PulseCoAPTransportPosix.h`

POSIX/BSD `SOCK_DGRAM` adapter for Linux, macOS, and POSIX-compliant RTOS targets (Zephyr with POSIX API, NuttX, RIOT). Header-only. Compiled only on `__unix__`, `__linux__`, `__APPLE__`, or `_POSIX_VERSION` platforms.

```cpp
class PosixUdpTransport : public Transport {
public:
    PosixUdpTransport();    // fd_ = -1
    ~PosixUdpTransport();   // closes fd_ if open

    bool   begin(uint16_t localPort) override;
    bool   send(const Endpoint& to, const uint8_t* data, size_t length) override;
    size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) override;

    // Returns the actual bound port. Call after begin(0) to discover the
    // ephemeral port the OS assigned.
    uint16_t localPort() const;
};
```

**IPv4 / IPv6 dual-stack:** `begin()` opens an `AF_INET6` socket with `IPV6_V6ONLY=0` so a single socket handles both families. If the kernel has no IPv6 (or if `-DNO_IPV6` is set), it falls back to `AF_INET`. `send()` maps IPv4 targets to `::ffff:a.b.c.d` on dual-stack sockets. `receive()` demaps incoming IPv4-mapped addresses so callers always see `isV6 == false` for IPv4 peers.

**Non-blocking:** The socket is put in `O_NONBLOCK` mode so `receive()` returns 0 immediately when no datagram is waiting.

**Example — host-side test tool:**
```cpp
#include <PulseCoAPTransportPosix.h>

PosixUdpTransport transport;
TransactionPool   txPool;
Client            client(transport, txPool);

void run() {
    client.begin(0);           // OS assigns ephemeral port
    printf("client port: %d\n", transport.localPort());

    Endpoint srv;
    srv.ip[0]=127; srv.ip[1]=0; srv.ip[2]=0; srv.ip[3]=1;
    srv.port = 5683;

    client.get(srv, "/hello", [](const ClientResponse& r, void*) {
        printf("%.*s\n", (int)r.payloadLength, r.payload);
    });

    for (int i = 0; i < 50; ++i) {
        client.poll(static_cast<uint32_t>(i));
        usleep(1000);
    }
}
```

**IPv6 example:**
```cpp
Endpoint srv6;
srv6.isV6   = true;
srv6.v6[15] = 1;   // ::1
srv6.port   = 5683;
client.get(srv6, "/hello", onResponse);
```

---

### DtlsTransport

Header: `src/PulseCoAPTransportDTLS.h`  
Requires: `PULSECOAP_ENABLE_DTLS=1`, Arduino framework, ESP32 mbedTLS (RFC 6347, RFC 7252 §9)

Wraps a caller-owned Arduino `UDP&` object and runs a fixed-size DTLS session table on top of it. Upper layers (`Server`, `Client`) use the same three-method `Transport` interface — no changes to application code beyond swapping the transport and calling `addPsk()` before `begin()`.

**Security model:** Pre-shared keys (PSK, RFC 4279). No x.509 certificates. Server-mode enables DTLS cookies for anti-amplification (RFC 6347 §4.2.1).

**Memory note:** PulseCoAP's own protocol code (codec, routing, retransmission) stays zero-heap. mbedTLS itself uses ~7–12 KB of heap per session during handshake, dropping to ~3–5 KB at idle.

**Arduino platform only** — the adapter is not compiled on POSIX or lwIP-only builds.

```cpp
class DtlsTransport : public Transport {
public:
    // server=true  — expect inbound handshakes (server role, DTLS cookie enabled).
    // server=false — initiate outbound handshakes (client role).
    explicit DtlsTransport(UDP& udp, bool server = false);
    ~DtlsTransport();

    // Add a PSK entry (raw key bytes). Call before begin().
    // Returns false if the store is full or an argument is invalid.
    bool addPsk(const char* identity, const uint8_t* key, size_t keyLen);

    // Convenience overload — passphrase is used as raw key bytes.
    bool addPsk(const char* identity, const char* passphrase);

    // One-time setup: initialise mbedTLS and bind the UDP socket.
    // Pass 5684 (CoAP-over-DTLS standard port) for servers.
    // For clients, a non-zero server PSK must be added before calling begin().
    bool   begin(uint16_t localPort) override;

    // Encrypt and send one CoAP message to `to`.
    // Returns false while a DTLS handshake is in progress — PulseCoAP's CON
    // retransmit engine retries after ACK_TIMEOUT_MS, which drives handshake
    // progress on subsequent receive() calls.
    bool   send(const Endpoint& to, const uint8_t* data, size_t length) override;

    // Non-blocking: drives in-progress handshakes and returns a decrypted
    // CoAP datagram when one is ready. Returns 0 while handshaking.
    size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from) override;
};
```

**Server example (ESP32):**
```cpp
#define PULSECOAP_ENABLE_DTLS 1
#include <WiFiUDP.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportDTLS.h>
using namespace pulsecoap;

WiFiUDP       udp;
DtlsTransport transport(udp, /*server=*/true);
Server        server(transport);

const uint8_t PSK_KEY[] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                            0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10};

void setup() {
    // ... connect WiFi ...
    transport.addPsk("sensor-node-01", PSK_KEY, sizeof(PSK_KEY));
    server.addResource("/temp", MethodGet, tempHandler, nullptr, true);
    server.begin(5684);   // CoAP-over-DTLS standard port
}
void loop() { server.poll(millis()); }
```

**Client example (ESP32):**
```cpp
#define PULSECOAP_ENABLE_DTLS 1
#include <WiFiUDP.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportDTLS.h>
using namespace pulsecoap;

WiFiUDP           udp;
DtlsTransport     transport(udp, /*server=*/false);
TransactionPool   txPool;
Client            client(transport, txPool);

const uint8_t PSK_KEY[] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                            0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,0x10};
Endpoint gw;   // fill ip[] and port before use

void setup() {
    // ... connect WiFi ...
    transport.addPsk("sensor-node-01", PSK_KEY, sizeof(PSK_KEY));
    client.begin(0);        // OS assigns ephemeral port
    gw.ip[0]=192; gw.ip[1]=168; gw.ip[2]=1; gw.ip[3]=1;
    gw.port = 5684;
    client.get(gw, "/temp", [](const ClientResponse& r, void*){
        Serial.write(r.payload, r.payloadLength); Serial.println();
    });
}
void loop() { client.poll(millis()); }
```

**Non-blocking handshake flow:**
1. First `send()` to a new peer allocates a session slot, starts the DTLS handshake, buffers the outgoing CoAP packet, and returns `false`.
2. PulseCoAP's CON retransmit engine retries `send()` after `ACK_TIMEOUT_MS` (2 s by default), and each `receive()` call drives any incoming handshake flights.
3. Once ESTABLISHED, the buffered packet is flushed on the next `send()` retry.
4. If the handshake does not complete within `PULSECOAP_DTLS_HANDSHAKE_TIMEOUT_MS` (default 10 s), the session slot is reclaimed automatically.

---

## 5. TransactionPool

Header: `src/PulseCoAPTransaction.h`

`TransactionPool` implements RFC 7252 §4.8 reliable message delivery for Confirmable messages. It maintains a fixed pool of in-flight message records, drives exponential backoff with jitter, and fires a timeout callback when `MAX_RETRANSMIT` retries are exhausted.

Both `Server` and `Client` each own their own embedded or externally-provided `TransactionPool`. You only interact with `TransactionPool` directly if you are writing a custom transport adapter or lower-level CoAP tooling; in normal use the server and client manage it internally.

```cpp
class TransactionPool {
public:
    // Register a new in-flight CON message. Returns the slot index on success,
    // -1 if the pool is full. `message` and `token` are copied internally.
    int start(const Endpoint& remote, uint16_t messageId,
              const uint8_t* token,   uint8_t tokenLen,
              const uint8_t* message, size_t  messageLen,
              uint32_t nowMs);

    // Mark a transaction complete when an ACK or piggybacked response arrives.
    // Matches by (remote, messageId). Returns true if a slot was freed.
    bool complete(const Endpoint& remote, uint16_t messageId);

    // Same as complete() but matches by (remote, token). Useful when the
    // incoming packet carries a token but no message ID (e.g. separate response).
    bool completeByToken(const Endpoint& remote,
                         const uint8_t* token, uint8_t tokenLen);

    // Recover the token for a given (remote, messageId). Used when a RST
    // arrives (RST carries messageId only, not the token). Returns false if
    // no matching slot is found.
    bool getToken(const Endpoint& remote, uint16_t messageId,
                  uint8_t* tokenOut, uint8_t& tokenLenOut) const;

    // Drive retransmission and timeout logic. Call from Server::poll() /
    // Client::poll() every loop iteration.
    //   resend   — called each time a retransmit is due with the raw message bytes
    //   onTimeout — called after MAX_RETRANSMIT failures; slot is freed before the call
    //   ctx      — forwarded to both callbacks
    void tick(uint32_t nowMs, ResendFn resend, TimeoutFn onTimeout, void* ctx);

    // Returns the number of currently in-flight transactions.
    uint8_t activeCount() const;

    // Computes the initial retransmit timeout for a new slot per RFC 7252 §4.8:
    // random in [ACK_TIMEOUT, ACK_TIMEOUT × ACK_RANDOM_FACTOR].
    // `seedForJitter` can be a message ID or any cheaply available pseudo-random value.
    static uint32_t computeInitialTimeoutMs(uint32_t seedForJitter);
};
```

**Callback signatures:**
```cpp
// Called by tick() to retransmit a message.
using ResendFn  = void (*)(void* ctx, const Endpoint& remote,
                            const uint8_t* message, size_t messageLen);

// Called by tick() when MAX_RETRANSMIT is exhausted (slot already freed).
using TimeoutFn = void (*)(void* ctx, const Endpoint& remote,
                            const uint8_t* token, uint8_t tokenLen);
```

**Example — custom integration:**
```cpp
// Rarely needed directly. Shown for completeness.
TransactionPool pool;

void myResend(void* ctx, const Endpoint& remote, const uint8_t* msg, size_t len) {
    static_cast<MyTransport*>(ctx)->send(remote, msg, len);
}
void myTimeout(void* ctx, const Endpoint& remote, const uint8_t* token, uint8_t tlen) {
    printf("CON timed out after %d retries\n", PULSECOAP_MAX_RETRANSMIT);
}

// In your loop:
pool.tick(nowMs, myResend, myTimeout, &myTransport);
```

---

## 6. Server

Header: `src/PulseCoAPServer.h`

### MethodMask

A bitmask that specifies which HTTP-like methods a resource handler accepts. Pass a bitwise OR of the constants to `addResource()`.

```cpp
enum MethodMask : uint8_t {
    MethodGet    = 1 << 0,   // 0x01
    MethodPost   = 1 << 1,   // 0x02
    MethodPut    = 1 << 2,   // 0x04
    MethodDelete = 1 << 3,   // 0x08
};
```

**Example:**
```cpp
// Read-only resource.
server.addResource("/status", MethodGet, handleStatus);

// Read + write.
server.addResource("/cfg", MethodGet | MethodPut, handleCfg);

// Write-only collection endpoint.
server.addResource("/log", MethodPost, handleLog);
```

A request for a method not in the bitmask receives an automatic `4.05 Method Not Allowed` response.

---

### DeferHandle

```cpp
using DeferHandle = uint8_t;
static constexpr DeferHandle kInvalidDeferHandle = 0xFF;
```

An opaque, lightweight handle the server assigns to deferred requests. Store the value of `req.deferHandle` in your handler (when `res.deferred = true`) and pass it back to `server.respond()` once the asynchronous work completes.

---

### Request

Passed by const reference to every `ResourceHandler` call.

```cpp
struct Request {
    Code           method;   // the CoAP method code (Get, Put, Post, Delete)
    const Message* message;  // full decoded message — inspect raw options here
    Endpoint       remote;   // sender's address and port
    DeferHandle    deferHandle; // store this; pass to server.respond() if deferred

    // Block-wise: when the final block of a Block1 transfer arrives,
    // block1Body points to the reassembled full payload. nullptr otherwise.
    const uint8_t* block1Body       = nullptr;
    size_t         block1BodyLength = 0;

    // Convenience accessor: returns block1Body when set, otherwise the
    // message's own payload. Use this in handlers instead of
    // message->payload() to be block-wise-transparent.
    const uint8_t* payload()       const;
    size_t         payloadLength() const;

    // For URI-template resources (e.g. "/sensors/:id"): returns the matched
    // segment value as a NUL-terminated string, or nullptr if the name is absent.
    // The pointer is valid for the lifetime of the Request (i.e. inside the handler).
    const char* pathParam(const char* name) const;
};
```

**Example — reading payload and a path param:**
```cpp
void handleSensorPut(const Request& req, Response& res, void* ctx) {
    const char* id = req.pathParam("id");   // "/sensors/:id"
    if (!id) { res.code = Code::InternalServerError; return; }

    // Use payload() for block-wise transparency.
    const uint8_t* body = req.payload();
    size_t         len  = req.payloadLength();

    if (!updateSensor(id, body, len)) {
        res.code = Code::BadRequest;
        return;
    }
    res.code = Code::Changed;
}
```

---

### Response

Populated by the `ResourceHandler` before returning to the server.

```cpp
struct Response {
    Code          code          = Code::Content;
    ContentFormat contentFormat = ContentFormat::TextPlain;
    const uint8_t* payload      = nullptr;
    size_t         payloadLength = 0;

    // When true: the server sends an empty ACK immediately (for CON requests)
    // to stop the client retransmitting, then waits for server.respond() with
    // the actual response. The req.deferHandle must be stored before returning.
    bool deferred = false;

    // Convenience setters — pointer only, no copy.
    void setPayload(const uint8_t* data, size_t length);
    void setPayload(const char* text);  // uses strlen()
};
```

**Setting a response:**
```cpp
void handleGet(const Request&, Response& res, void*) {
    static const char* kBody = "{\"v\":1}";
    res.code          = Code::Content;
    res.contentFormat = ContentFormat::Json;
    res.setPayload(kBody);   // pointer to static string — not copied
}
```

**Deferred response (handler does async I²C read, then responds):**
```cpp
static DeferHandle g_handle;

void handleSlowGet(const Request& req, Response& res, void*) {
    g_handle   = req.deferHandle;   // store before returning
    res.deferred = true;
    startI2CRead();                  // kick off async operation
    // Server sends empty ACK; client stops retransmitting.
}

void i2cDoneCallback(const uint8_t* data, size_t len) {
    // Called from your I2C ISR / RTOS task / polling check.
    server.respond(g_handle, Code::Content, data, len);
}
```

---

### ResourceHandler

```cpp
using ResourceHandler = void (*)(const Request& req, Response& res, void* userContext);
```

A plain C function pointer. `userContext` is whatever pointer you passed to `addResource()`. There is no `std::function` overhead; no heap allocation.

---

### Server Class

```cpp
class Server {
public:
    // `transport` must outlive the Server.
    explicit Server(Transport& transport);

    // Opens the socket and auto-registers /.well-known/core.
    // localPort=5683 is the IANA-assigned CoAP port.
    // Pass 0 for an ephemeral port (useful in tests).
    bool begin(uint16_t localPort = 5683);

    // Registers a resource. path must start with '/' and remain valid (not
    // freed or modified) for the lifetime of the Server — store it in a
    // string literal or a static buffer; it is NOT copied.
    //   path       — e.g. "/sensors/temp" or "/devices/:id/status"
    //   methods    — MethodMask bitmask
    //   handler    — called for each matching request
    //   userContext — forwarded to handler as-is; may be nullptr
    //   observable  — true to let clients subscribe via Observe (RFC 7641)
    //
    // Returns false if the resource table is full (PULSECOAP_MAX_RESOURCES).
    bool addResource(const char* path, uint8_t methods, ResourceHandler handler,
                     void* userContext = nullptr, bool observable = false);

    // Attaches a CoRE Link Format rt= attribute to a resource for discovery.
    // `rt` is stored by pointer; must remain valid for the Server's lifetime.
    // Returns false if `path` is not a registered resource.
    // Call after addResource(), before the first poll().
    bool setResourceType(const char* path, const char* rt);

    // Receive, decode, and dispatch one incoming datagram per call, and drive
    // retransmission of pending CON responses. Must be called from the
    // application loop at least as often as your fastest expected CoAP
    // round-trip (~50 ms is a comfortable maximum interval for typical LAN use).
    //   nowMs — current monotonic time in milliseconds (e.g. millis() on Arduino,
    //           sys_now() in lwIP, or a counter in bare-metal code)
    void poll(uint32_t nowMs);

    // Sends the deferred response for `handle`. The response is sent as CON
    // if the original request was Confirmable (the retransmission cycle is
    // completed by Client::poll() ACK-ing it), NON otherwise.
    //   handle        — value stored from req.deferHandle
    //   code          — response code (e.g. Code::Content, Code::Changed)
    //   payload/len   — response body; not copied, must remain valid until ACK'd
    //   contentFormat — defaults to TextPlain
    // Returns false if handle is invalid, already consumed, or the pool is full.
    bool respond(DeferHandle handle, Code code,
                 const uint8_t* payload, size_t payloadLength,
                 ContentFormat contentFormat = ContentFormat::TextPlain);

    // Convenience overload for NUL-terminated string payloads.
    bool respond(DeferHandle handle, Code code, const char* text,
                 ContentFormat contentFormat = ContentFormat::TextPlain);

    // Pushes `payload` to every observer currently subscribed to `path`.
    //   path        — must match an existing observable resource exactly
    //   confirmable — false (default): NON push, fire-and-forget
    //                 true: CON push, retransmitted until ACK'd; a peer that
    //                 never ACKs is silently removed after MAX_RETRANSMIT
    //                 retries (RFC 7641 §4.5 dead-peer detection).
    //
    // Returns false if the resource is not found or not observable; never
    // an error when there are zero active subscribers.
    bool notify(const char* path,
                const uint8_t* payload, size_t payloadLength,
                ContentFormat contentFormat = ContentFormat::TextPlain,
                bool confirmable = false);

    // Returns the number of active Observe subscribers across all resources.
    uint8_t observerCount() const;
};
```

---

#### Resource Discovery (`/.well-known/core`)

`Server::begin()` automatically registers `/.well-known/core` (RFC 6690). A `GET` on it returns all user-registered resource paths in CoRE Link Format (Content-Format 40).

The generated body looks like:

```
</.well-known/core>,</sensors/temp>;obs,</actuators/led>;rt="switch"
```

- Resources registered with `observable=true` get `;obs`.
- Resources with `setResourceType()` get `;rt="..."`.

No IP address hardcoding needed: a client that knows the server's address can always discover its resources via this endpoint.

---

#### URI Templates

Paths with `:param` segments match any single path segment at that position. Exact paths are tried first; templates are tried in registration order only when no exact match is found.

```cpp
server.addResource("/sensors/:id",        MethodGet,        handleSensor);
server.addResource("/devices/:dev/ch/:ch", MethodGet | MethodPut, handleChannel);
server.addResource("/sensors/temp",        MethodGet,        handleTemp);
// ^-- exact match takes priority over /sensors/:id for this path
```

Inside the handler, `req.pathParam("id")` returns the matched segment or `nullptr`:

```cpp
void handleSensor(const Request& req, Response& res, void* ctx) {
    const char* sensorId = req.pathParam("id");
    // e.g. for GET /sensors/42, sensorId == "42"
}
```

---

#### Complete Server Example

```cpp
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>
#include <WiFiUDP.h>

using namespace pulsecoap;

WiFiUDP             udp;
ArduinoUdpTransport transport(udp);
Server              server(transport);

// ---- Handlers ----

void handleGetTemp(const Request&, Response& res, void*) {
    static char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", readTempC());
    res.code          = Code::Content;
    res.contentFormat = ContentFormat::TextPlain;
    res.setPayload(buf);
}

void handleSetLED(const Request& req, Response& res, void*) {
    if (req.payloadLength() < 1) { res.code = Code::BadRequest; return; }
    bool on = req.payload()[0] == '1';
    digitalWrite(LED_PIN, on ? HIGH : LOW);
    res.code = Code::Changed;
}

void handleSensorById(const Request& req, Response& res, void*) {
    const char* id = req.pathParam("id");
    float value = getSensorValue(atoi(id));
    static char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", value);
    res.setPayload(buf);
}

// ---- Setup ----

void setup() {
    WiFi.begin(SSID, PASS);
    while (WiFi.status() != WL_CONNECTED) delay(100);

    server.addResource("/temp",        MethodGet,       handleGetTemp, nullptr, /*obs=*/true);
    server.addResource("/led",         MethodPut,       handleSetLED);
    server.addResource("/sensors/:id", MethodGet,       handleSensorById);
    server.setResourceType("/temp", "temperature");
    server.begin(); // port 5683
}

void loop() {
    uint32_t now = millis();
    server.poll(now);

    // Reliable periodic notify every 10 s; dead peers auto-pruned.
    static uint32_t last = 0;
    if (now - last >= 10000) {
        last = now;
        static char buf[12];
        snprintf(buf, sizeof(buf), "%.1f", readTempC());
        server.notify("/temp",
                      reinterpret_cast<const uint8_t*>(buf), strlen(buf),
                      ContentFormat::TextPlain,
                      /*confirmable=*/true);
    }
}
```

---

## 7. Client

Header: `src/PulseCoAPClient.h`

### ClientResponse

Passed by const reference to every `ResponseHandler` call.

```cpp
struct ClientResponse {
    Code           code;           // response code (e.g. Code::Content)
    const uint8_t* payload;        // response body; valid only during the callback
    size_t         payloadLength;
    ContentFormat  contentFormat;  // e.g. ContentFormat::Json
};
```

**Note:** `payload` points into an internal buffer that is immediately reused after the callback returns. Copy the data if you need it to outlive the callback.

---

### ResponseHandler

```cpp
using ResponseHandler = void (*)(const ClientResponse& res, void* userContext);
```

- For one-shot requests (`get`, `put`, `post`, `del`): called exactly once when the response arrives or the retransmission pool fires `TimeoutHandler`.
- For `observe()`: called once for the initial response and then once per notification from the server. It continues firing until `cancelObserve()` is called or the server sends a RST.

---

### TimeoutHandler

```cpp
using TimeoutHandler = void (*)(void* userContext);
```

Optional. Installed via `client.setTimeoutHandler(fn)`. Called when a CON request exhausts `PULSECOAP_MAX_RETRANSMIT` retries without receiving an ACK or response. The `userContext` passed is the one that was given to the triggering `get()`/`put()`/etc. call.

---

### DiscoverHandler

```cpp
using DiscoverHandler = void (*)(const Endpoint& server,
                                  const uint8_t* linkFormat, size_t length,
                                  void* userContext);
```

Fires once per responding server during a `discover()` collection window.

| Parameter | Description |
|-----------|-------------|
| `server` | Unicast endpoint (IP + port) of the responding server |
| `linkFormat` | Raw CoRE Link Format body (RFC 6690, Content-Format 40) from the server's `/.well-known/core` response; valid only during the callback |
| `length` | Byte length of `linkFormat` |
| `userContext` | The pointer that was passed to `discover()` |

**Note:** `linkFormat` points into an internal buffer that is immediately reused after the callback returns. Copy the data if you need it to outlive the call.

---

### Client Class

```cpp
class Client {
public:
    // `transport` and `transactions` must outlive the Client.
    Client(Transport& transport, TransactionPool& transactions);

    // Opens the socket. localPort=0 lets the OS assign an ephemeral port.
    bool begin(uint16_t localPort = 0);

    // ---- One-shot requests ----
    // All return false if:
    //   - the pending-request table is full (PULSECOAP_MAX_TRANSACTIONS)
    //   - the message could not be encoded or sent
    // `confirmable` defaults to true (CON — reliable); pass false for NON.

    bool get(const Endpoint& server, const char* path,
             ResponseHandler onResponse, void* userContext = nullptr,
             bool confirmable = true);

    bool put(const Endpoint& server, const char* path,
             const uint8_t* payload, size_t payloadLength,
             ContentFormat contentFormat,
             ResponseHandler onResponse, void* userContext = nullptr,
             bool confirmable = true);

    bool post(const Endpoint& server, const char* path,
              const uint8_t* payload, size_t payloadLength,
              ContentFormat contentFormat,
              ResponseHandler onResponse, void* userContext = nullptr,
              bool confirmable = true);

    bool del(const Endpoint& server, const char* path,
             ResponseHandler onResponse, void* userContext = nullptr,
             bool confirmable = true);

    // ---- Observe (RFC 7641) ----

    // Sends a Confirmable GET with Observe=0 to register for push notifications.
    // `onResponse` fires for the initial response and every subsequent notification.
    // Multiple resources on the same server can be observed simultaneously —
    // each call occupies its own pending slot identified by (server, path).
    bool observe(const Endpoint& server, const char* path,
                 ResponseHandler onResponse, void* userContext = nullptr);

    // Sends a GET with Observe=1 to deregister the specific (server, path)
    // subscription (RFC 7641 §3.6). Returns false if no matching subscription
    // is active. Two simultaneous observes on the same server at different paths
    // are cancelled independently.
    bool cancelObserve(const Endpoint& server, const char* path);

    // ---- Multicast resource discovery (RFC 7252 §8) ----

    // Sends a NON GET for /.well-known/core to 224.0.1.187:port (the IANA
    // CoAP all-nodes IPv4 multicast address). Every server on the LAN that
    // has joined the multicast group responds unicast; onDiscover fires once
    // per responder. The slot stays active for PULSECOAP_DISCOVER_TIMEOUT_MS ms,
    // then frees automatically. Returns false if no discover slot is free or
    // the send fails.
    bool discover(DiscoverHandler onDiscover, void* userContext = nullptr,
                  uint16_t port = 5683);

    // Overload for a custom multicast destination (e.g. [FF02::FD]:5683 for
    // IPv6 or an alternate port in tests).
    bool discover(const Endpoint& multicastEp, DiscoverHandler onDiscover,
                  void* userContext = nullptr);

    // Returns an Endpoint for the IANA IPv4 CoAP all-nodes multicast address
    // 224.0.1.187. Callers that need IPv6 can build [FF02::FD] themselves.
    static Endpoint allNodesEndpoint(uint16_t port = 5683);

    // ---- Lifetime callbacks ----

    // Installs an optional handler for CON retransmission timeout. The handler
    // receives the same userContext that was passed to the original request call.
    void setTimeoutHandler(TimeoutHandler onTimeout);

    // ---- Main loop ----

    // Receives and dispatches one waiting datagram per call, and drives
    // retransmission of in-flight CON requests. Call from the application loop
    // with a monotonic millisecond counter.
    void poll(uint32_t nowMs);
};
```

---

#### Example — GET

```cpp
void onTemp(const ClientResponse& res, void* ctx) {
    if (res.code == Code::Content) {
        char buf[32];
        size_t n = res.payloadLength < sizeof(buf)-1 ? res.payloadLength : sizeof(buf)-1;
        memcpy(buf, res.payload, n); buf[n] = '\0';
        printf("Temperature: %s°C\n", buf);
    }
}

// --- in main/setup ---
Endpoint srv; srv.ip[0]=192; srv.ip[1]=168; srv.ip[2]=1; srv.ip[3]=10; srv.port=5683;
client.get(srv, "/temp", onTemp);
```

---

#### Example — PUT with JSON payload

```cpp
const char* kBody = "{\"led\":1}";
client.put(srv, "/actuators/led",
           reinterpret_cast<const uint8_t*>(kBody), strlen(kBody),
           ContentFormat::Json,
           [](const ClientResponse& r, void*) {
               printf("PUT result: %d\n", static_cast<int>(r.code));
           });
```

---

#### Example — Observe + Cancel

```cpp
struct SensorCtx { std::string lastVal; };
SensorCtx sctx;

void onSensor(const ClientResponse& res, void* ctx) {
    auto* c = static_cast<SensorCtx*>(ctx);
    c->lastVal.assign(reinterpret_cast<const char*>(res.payload), res.payloadLength);
    printf("Sensor update: %s\n", c->lastVal.c_str());
}

// Register:
client.observe(srv, "/sensors/temp", onSensor, &sctx);

// Later, to stop receiving notifications:
client.cancelObserve(srv, "/sensors/temp");
```

---

#### Example — Multiple simultaneous Observes on the same server

```cpp
client.observe(srv, "/sensors/temp", onTemp, &tempCtx);
client.observe(srv, "/sensors/hum",  onHum,  &humCtx);
// Each subscription has an independent slot and callback.
// Cancelling one leaves the other untouched:
client.cancelObserve(srv, "/sensors/temp");
// /sensors/hum is still active.
```

---

#### Example — Timeout handling

```cpp
client.setTimeoutHandler([](void* ctx) {
    printf("Request timed out after %d retries — server unreachable?\n",
           PULSECOAP_MAX_RETRANSMIT);
});
client.get(srv, "/slow-resource", onResponse);
```

---

#### Example — Block-wise large PUT (when `PULSECOAP_ENABLE_BLOCKWISE=1`)

Block-wise transfer is automatic. The client transparently fragments large payloads into `PULSECOAP_BLOCK_SZX`-sized blocks and reassembles large server responses. No API change is required.

```cpp
// This payload is 2 KB — larger than PULSECOAP_MAX_MSG_SIZE.
// With PULSECOAP_ENABLE_BLOCKWISE=1 the client sends it as Block1 chunks.
std::vector<uint8_t> bigPayload(2048, 0xAB);
client.put(srv, "/ota/firmware",
           bigPayload.data(), bigPayload.size(),
           ContentFormat::OctetStream,
           [](const ClientResponse& r, void*) {
               printf("OTA upload done: %d\n", static_cast<int>(r.code));
           });
```

---

#### Example — Multicast resource discovery

```cpp
// Called once per responding server during the collection window.
void onDiscover(const Endpoint& server, const uint8_t* lf, size_t len, void* ctx) {
    char buf[256];
    size_t n = len < sizeof(buf)-1 ? len : sizeof(buf)-1;
    memcpy(buf, lf, n); buf[n] = '\0';
    printf("Server %d.%d.%d.%d:%d resources: %s\n",
           server.ip[0], server.ip[1], server.ip[2], server.ip[3],
           server.port, buf);
}

// --- in setup ---
// On ESP32 / Arduino the multicast group is joined at the network layer by
// the UDP stack automatically for link-local groups; call discover() and
// responses arrive via poll() in the normal main loop.
client.discover(onDiscover, nullptr);      // sends to 224.0.1.187:5683

// Custom destination — e.g. an alternate port used in integration tests:
Endpoint mcast = Client::allNodesEndpoint(9999);
client.discover(mcast, onDiscover, nullptr);
```

On POSIX/Linux hosts (unit tests, gateways) the server also needs to join the group:

```cpp
// Server side
PosixUdpTransport serverTr;
Server srv(serverTr);
srv.begin(5683);
serverTr.joinMulticastGroup("224.0.1.187");          // join on INADDR_ANY
// srv.addResource(...)
// ...poll loop...

// Client side (test — route through loopback)
PosixUdpTransport clientTr;
Client client(clientTr, txPool);
client.begin(0);
clientTr.setMulticastOutboundInterface("127.0.0.1"); // loopback only
client.discover(onDiscover, nullptr);
```

---

## 8. Code Enum Reference

| Enum name | Dotted | Wire byte | Meaning |
|-----------|--------|-----------|---------|
| `Empty` | 0.00 | 0x00 | Empty ACK or RST |
| `Get` | 0.01 | 0x01 | GET request |
| `Post` | 0.02 | 0x02 | POST request |
| `Put` | 0.03 | 0x03 | PUT request |
| `Delete` | 0.04 | 0x04 | DELETE request |
| `Created` | 2.01 | 0x41 | Resource created (response to POST/PUT) |
| `Deleted` | 2.02 | 0x42 | Resource deleted |
| `Valid` | 2.03 | 0x43 | Resource not modified (ETag match) |
| `Changed` | 2.04 | 0x44 | Resource updated |
| `Content` | 2.05 | 0x45 | Success with payload (response to GET/Observe) |
| `Continue` | 2.31 | 0x5F | Block1 intermediate ACK — send next block |
| `BadRequest` | 4.00 | 0x80 | Malformed request |
| `Unauthorized` | 4.01 | 0x81 | Client not authorised |
| `BadOption` | 4.02 | 0x82 | Unrecognised critical option |
| `Forbidden` | 4.03 | 0x83 | Access denied |
| `NotFound` | 4.04 | 0x84 | Resource does not exist |
| `MethodNotAllowed` | 4.05 | 0x85 | Method not in resource's MethodMask |
| `NotAcceptable` | 4.06 | 0x86 | Requested content format not available |
| `RequestEntityIncomplete` | 4.08 | 0x88 | Block1 gap — missing earlier block |
| `PreconditionFailed` | 4.12 | 0x8C | ETag or If-Match check failed |
| `RequestEntityTooLarge` | 4.13 | 0x8D | Payload exceeds server capacity |
| `UnsupportedContentFormat` | 4.15 | 0x8F | Server cannot process this content format |
| `InternalServerError` | 5.00 | 0xA0 | Unspecified server fault |
| `NotImplemented` | 5.01 | 0xA1 | Method not supported by server |
| `BadGateway` | 5.02 | 0xA2 | Upstream proxy returned error |
| `ServiceUnavailable` | 5.03 | 0xA3 | Server temporarily overloaded |
| `GatewayTimeout` | 5.04 | 0xA4 | Upstream proxy timed out |
| `ProxyingNotSupported` | 5.05 | 0xA5 | Server does not proxy |

---

## 9. ContentFormat Enum Reference

| Enum name | Value | MIME type |
|-----------|-------|-----------|
| `TextPlain` | 0 | `text/plain; charset=utf-8` |
| `LinkFormat` | 40 | `application/link-format` (CoRE, RFC 6690) |
| `Xml` | 41 | `application/xml` |
| `OctetStream` | 42 | `application/octet-stream` |
| `Exi` | 47 | `application/exi` |
| `Json` | 50 | `application/json` |
| `Cbor` | 60 | `application/cbor` |

---

## 10. Complete Macro Reference

| Macro | Default | Section |
|-------|---------|---------|
| `PULSECOAP_MAX_MSG_SIZE` | 256 | Message sizing |
| `PULSECOAP_MAX_OPTIONS` | 16 | Message sizing |
| `PULSECOAP_MAX_TOKEN_LEN` | 8 | Message sizing |
| `PULSECOAP_MAX_TRANSACTIONS` | 4 | Reliability |
| `PULSECOAP_ACK_TIMEOUT_MS` | 2000 | Reliability |
| `PULSECOAP_ACK_RANDOM_FACTOR_NUM` | 3 | Reliability |
| `PULSECOAP_ACK_RANDOM_FACTOR_DEN` | 2 | Reliability |
| `PULSECOAP_MAX_RETRANSMIT` | 4 | Reliability |
| `PULSECOAP_MAX_RESOURCES` | 8 | Server |
| `PULSECOAP_MAX_OBSERVERS` | 8 | Server |
| `PULSECOAP_MAX_URI_PATH_LEN` | 64 | Server |
| `PULSECOAP_MAX_DEFERRED` | 2 | Server |
| `PULSECOAP_MAX_LINK_FORMAT_LEN` | 256 | Server |
| `PULSECOAP_ROLE_CLIENT_ONLY` | *(not set)* | Role selection |
| `PULSECOAP_ROLE_SERVER_ONLY` | *(not set)* | Role selection |
| `PULSECOAP_ENABLE_BLOCKWISE` | 0 | Block-wise |
| `PULSECOAP_BLOCK_SZX` | 4 | Block-wise |
| `PULSECOAP_MAX_BLOCK1_SESSIONS` | 2 | Block-wise |
| `PULSECOAP_BLOCK1_MAX_BODY` | 1024 | Block-wise |
| `PULSECOAP_BLOCK2_MAX_BODY` | 1024 | Block-wise |
| `PULSECOAP_MAX_PATH_PARAMS` | 4 | URI templates |
| `PULSECOAP_MAX_PATH_PARAM_LEN` | 16 | URI templates |
| `PULSECOAP_ENABLE_TRACE` | 0 | Tracing (reserved) |
| `PULSECOAP_LWIP_RX_SLOTS` | 4 | LwIP adapter |
| `NO_IPV6` | *(not set)* | POSIX adapter |

---

## 11. Board & Platform Examples

This section shows complete, copy-paste starter sketches for every supported hardware family. Each example is self-contained — only include what you need for your target.

---

### 11.1 ESP32 — Arduino framework (WiFiUDP)

The most common embedded target. `ArduinoUdpTransport` wraps `WiFiUDP` directly.

```cpp
// platform: ESP32, Arduino framework
// lib: PulseCoAP
#include <WiFi.h>
#include <WiFiUDP.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

using namespace pulsecoap;

WiFiUDP             udp;
ArduinoUdpTransport transport(udp);
Server              server(transport);

void handleTemp(const Request&, Response& res, void*) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", 25.3f); // replace with real sensor
    res.code = Code::Content;
    res.setPayload(buf);
}

void setup() {
    WiFi.begin("YourSSID", "YourPass");
    while (WiFi.status() != WL_CONNECTED) delay(200);
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

    server.addResource("/temp", MethodGet, handleTemp, nullptr, /*observable=*/true);
    server.begin(); // port 5683
}

void loop() {
    server.poll(millis());
}
```

**platformio.ini:**
```ini
[env:esp32dev]
platform  = espressif32
board     = esp32dev
framework = arduino
lib_deps  = PulseCoAP
```

---

### 11.2 ESP8266 — Arduino framework (WiFiUDP)

Identical API to ESP32; just swap the WiFi header.

```cpp
// platform: ESP8266, Arduino framework
#include <ESP8266WiFi.h>
#include <WiFiUDP.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

using namespace pulsecoap;

WiFiUDP             udp;
ArduinoUdpTransport transport(udp);
Server              server(transport);

void handleHum(const Request&, Response& res, void*) {
    res.code = Code::Content;
    res.setPayload("60.1");
}

void setup() {
    WiFi.begin("YourSSID", "YourPass");
    while (WiFi.status() != WL_CONNECTED) delay(200);

    server.addResource("/hum", MethodGet, handleHum);
    server.begin();
}

void loop() { server.poll(millis()); }
```

**platformio.ini:**
```ini
[env:esp12e]
platform  = espressif8266
board     = esp12e
framework = arduino
lib_deps  = PulseCoAP
```

---

### 11.3 Arduino Uno / Mega / Nano — W5100/W5500 Ethernet shield (EthernetUDP)

AVR boards with a hardware Ethernet shield. `ArduinoUdpTransport` wraps `EthernetUDP` the same way it wraps `WiFiUDP` — no code change in PulseCoAP.

```cpp
// platform: Arduino Uno / Mega (AVR), W5100/W5500 Ethernet shield
// RAM is tight on Uno (2 KB) — shrink the config below!
#define PULSECOAP_MAX_MSG_SIZE      128  // smaller messages
#define PULSECOAP_MAX_RESOURCES       4
#define PULSECOAP_MAX_OBSERVERS       4
#define PULSECOAP_MAX_TRANSACTIONS    2

#include <Ethernet.h>
#include <EthernetUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

using namespace pulsecoap;

static byte mac[] = {0xDE,0xAD,0xBE,0xEF,0xFE,0xED};

EthernetUDP         udp;
ArduinoUdpTransport transport(udp);
Server              server(transport);

void handlePing(const Request&, Response& res, void*) {
    res.code = Code::Content;
    res.setPayload("pong");
}

void setup() {
    Ethernet.begin(mac);          // DHCP
    delay(1000);

    server.addResource("/ping", MethodGet, handlePing);
    server.begin();
}

void loop() {
    Ethernet.maintain();          // renew DHCP lease when needed
    server.poll(millis());
}
```

**Memory tip:** On an Uno (32 KB flash, 2 KB RAM) always define `PULSECOAP_MAX_MSG_SIZE`, `PULSECOAP_MAX_RESOURCES`, `PULSECOAP_MAX_OBSERVERS`, and `PULSECOAP_MAX_TRANSACTIONS` to the minimum your application needs. Every byte counts.

**platformio.ini:**
```ini
[env:uno]
platform  = atmelavr
board     = uno
framework = arduino
lib_deps  = Ethernet
            PulseCoAP
```

---

### 11.4 Raspberry Pi Pico W — Arduino framework (WiFiUDP)

The Pico W runs the Arduino framework via the `arduino-pico` core. The WiFi stack exposes a `WiFiUDP` class, so `ArduinoUdpTransport` applies directly.

```cpp
// platform: Raspberry Pi Pico W, arduino-pico core
#include <WiFi.h>
#include <WiFiUDP.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

using namespace pulsecoap;

WiFiUDP             udp;
ArduinoUdpTransport transport(udp);
Server              server(transport);
TransactionPool     txPool;
Client              client(transport, txPool);

// Example: Pico W acts as server AND client on the same socket.
// (Use two separate transport + socket instances for full duplex.)

void handleLed(const Request& req, Response& res, void*) {
    if (req.payloadLength() >= 1)
        digitalWrite(LED_BUILTIN, req.payload()[0] == '1' ? HIGH : LOW);
    res.code = Code::Changed;
}

void setup() {
    WiFi.begin("YourSSID", "YourPass");
    while (WiFi.status() != WL_CONNECTED) delay(200);

    server.addResource("/led", MethodPut, handleLed);
    server.begin();
}

void loop() { server.poll(millis()); }
```

**platformio.ini:**
```ini
[env:rpipicow]
platform  = https://github.com/maxgerhardt/platform-raspberrypi.git
board     = rpipicow
framework = arduino
board_build.core = earlephilhower
lib_deps  = PulseCoAP
```

---

### 11.5 STM32 — bare-metal lwIP (LwIpUdpTransport)

Typical STM32F4/F7/H7 projects using CubeMX-generated lwIP with NO_SYS=1. The key loop pattern is: drive the netif input → call `sys_check_timeouts()` → call `poll()`.

```cpp
// platform: STM32F4xx / F7xx / H7xx, bare-metal lwIP (NO_SYS=1)
// Build with: -DPULSECOAP_TRANSPORT_LWIP (or ensure lwip/udp.h is already included)
#define PULSECOAP_TRANSPORT_LWIP
#include "PulseCoAP.h"
#include "PulseCoAPTransportLwIP.h"
// lwIP headers must be on the include path; CubeMX puts them in Middlewares/lwip/src/include

using namespace pulsecoap;

static LwIpUdpTransport transport;
static Server           server(transport);

static void handleStatus(const Request&, Response& res, void*) {
    res.code = Code::Content;
    res.setPayload("ok");
}

void app_init() {
    // lwIP and netif must be initialised before this call.
    server.addResource("/status", MethodGet, handleStatus);
    server.begin(5683);
}

void app_loop() {
    // 1. Feed received Ethernet frames to lwIP.
    ethernetif_input(&gnetif);          // board-specific netif poll

    // 2. Drive lwIP timers.
    sys_check_timeouts();

    // 3. Dispatch CoAP.
    server.poll(HAL_GetTick());         // HAL_GetTick() returns ms since reset
}
```

**lwIP lwipopts.h minimum settings:**
```c
#define NO_SYS                  1
#define LWIP_UDP                1
#define UDP_TTL                 255
// For IPv6 support add:
// #define LWIP_IPV6            1
```

---

### 11.6 ESP32 — ESP-IDF framework, bare-metal lwIP (LwIpUdpTransport)

When using ESP-IDF directly (not the Arduino layer), the lwIP raw API is available via `lwip/udp.h`. `LwIpUdpTransport` plugs in without any changes.

```cpp
// platform: ESP32, ESP-IDF framework (not Arduino)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sys.h"

#define LWIP_RAW 1      // ensures LwIpUdpTransport compiles
#include "PulseCoAP.h"
#include "PulseCoAPTransportLwIP.h"

using namespace pulsecoap;

static LwIpUdpTransport transport;
static Server           server(transport);

static void handleVersion(const Request&, Response& res, void*) {
    res.code = Code::Content;
    res.setPayload("PulseCoAP/0.9.0");
}

static void coap_task(void*) {
    server.addResource("/ver", MethodGet, handleVersion);
    server.begin(5683);

    while (1) {
        server.poll(pdTICKS_TO_MS(xTaskGetTickCount()));
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

extern "C" void app_main() {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    // ... WiFi init and connect ...

    xTaskCreate(coap_task, "coap", 4096, nullptr, 5, nullptr);
}
```

---

### 11.7 Zephyr RTOS — POSIX API (PosixUdpTransport)

Zephyr's POSIX compatibility layer (`CONFIG_POSIX_API=y`) exposes BSD sockets, so `PosixUdpTransport` works without modification. The `_POSIX_VERSION` macro that gates the adapter header is defined by Zephyr's libc.

```cpp
// platform: Zephyr RTOS (any board with CONFIG_POSIX_API=y)
// prj.conf: CONFIG_NETWORKING=y CONFIG_NET_IPV4=y CONFIG_POSIX_API=y
//           CONFIG_NET_UDP=y CONFIG_NET_SOCKETS=y
#include <zephyr/kernel.h>
#include "PulseCoAP.h"
#include "PulseCoAPTransportPosix.h"

using namespace pulsecoap;

static PosixUdpTransport transport;
static Server            server(transport);

static void handleInfo(const Request&, Response& res, void*) {
    res.code = Code::Content;
    res.setPayload("zephyr-node");
}

int main() {
    k_sleep(K_SECONDS(2));   // wait for network to come up

    server.addResource("/info", MethodGet, handleInfo);
    server.begin(5683);

    while (1) {
        server.poll(static_cast<uint32_t>(k_uptime_get()));
        k_msleep(5);
    }
}
```

**prj.conf minimum:**
```
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_UDP=y
CONFIG_NET_SOCKETS=y
CONFIG_POSIX_API=y
CONFIG_MAIN_STACK_SIZE=4096
```

---

### 11.8 NuttX RTOS (PosixUdpTransport)

NuttX provides a full POSIX socket layer. PulseCoAP uses `PosixUdpTransport` and the application structure mirrors a standard Linux program.

```cpp
// platform: NuttX (any supported board with network support)
// Compile: add PulseCoAP sources to your NuttX app's Makefile
#include <nuttx/config.h>
#include <sys/socket.h>   // triggers _POSIX_VERSION
#include <unistd.h>
#include "PulseCoAP.h"
#include "PulseCoAPTransportPosix.h"

using namespace pulsecoap;

extern "C" int pulsecoap_main(int argc, char* argv[]) {
    PosixUdpTransport transport;
    TransactionPool   txPool;
    Server            server(transport);
    Client            client(transport, txPool);

    server.addResource("/ping", MethodGet, [](const Request&, Response& res, void*) {
        res.code = Code::Content;
        res.setPayload("pong");
    }, nullptr);

    server.begin(5683);

    while (1) {
        server.poll(static_cast<uint32_t>(clock() / (CLOCKS_PER_SEC / 1000)));
        usleep(5000);
    }
    return 0;
}
```

---

### 11.9 Linux / macOS — host tooling and gateways (PosixUdpTransport)

The POSIX adapter is the standard choice for any Linux-based CoAP gateway, border router, or host-side command-line tool.

```cpp
// platform: Linux, macOS — g++ -std=c++11
// Build: g++ -std=c++11 -Isrc tool.cpp src/PulseCoAP*.cpp -o coap-tool
#include <cstdio>
#include <unistd.h>
#include "PulseCoAP.h"
#include "PulseCoAPTransportPosix.h"

using namespace pulsecoap;

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <host-ip> <path>\n", argv[0]);
        return 1;
    }

    PosixUdpTransport transport;
    TransactionPool   txPool;
    Client            client(transport, txPool);

    client.setTimeoutHandler([](void*) { fprintf(stderr, "timeout\n"); });
    client.begin(0);

    // Parse a.b.c.d from argv[1]
    Endpoint srv;
    unsigned a,b,c,d;
    sscanf(argv[1], "%u.%u.%u.%u", &a, &b, &c, &d);
    srv.ip[0]=(uint8_t)a; srv.ip[1]=(uint8_t)b;
    srv.ip[2]=(uint8_t)c; srv.ip[3]=(uint8_t)d;
    srv.port = 5683;

    bool done = false;
    client.get(srv, argv[2], [&done](const ClientResponse& res, void*) {
        printf("%d.%02d  %.*s\n",
               codeClass (static_cast<uint8_t>(res.code)),
               codeDetail(static_cast<uint8_t>(res.code)),
               static_cast<int>(res.payloadLength), res.payload);
        done = true;
    }, nullptr);

    for (int i = 0; i < 100 && !done; ++i) {
        client.poll(static_cast<uint32_t>(i * 10));
        usleep(10000);
    }
    return done ? 0 : 1;
}
```

**IPv6 usage (same code, different Endpoint):**
```cpp
Endpoint srv6;
srv6.isV6   = true;
// fill srv6.v6[16] with the target address
srv6.v6[15] = 1;   // ::1 for loopback
srv6.port   = 5683;
client.get(srv6, "/resource", onResponse);
```

---

### 11.10 Adapter–Board Quick Reference

| Board / Target | Adapter | Transport header |
|----------------|---------|-----------------|
| ESP32 (Arduino) | `ArduinoUdpTransport(WiFiUDP)` | `PulseCoAPTransportArduinoUDP.h` |
| ESP8266 (Arduino) | `ArduinoUdpTransport(WiFiUDP)` | `PulseCoAPTransportArduinoUDP.h` |
| Arduino Uno/Mega + W5x00 | `ArduinoUdpTransport(EthernetUDP)` | `PulseCoAPTransportArduinoUDP.h` |
| Raspberry Pi Pico W (Arduino) | `ArduinoUdpTransport(WiFiUDP)` | `PulseCoAPTransportArduinoUDP.h` |
| STM32 + bare-metal lwIP | `LwIpUdpTransport` | `PulseCoAPTransportLwIP.h` |
| ESP32 (ESP-IDF, bare lwIP) | `LwIpUdpTransport` | `PulseCoAPTransportLwIP.h` |
| RP2040 + bare-metal lwIP | `LwIpUdpTransport` | `PulseCoAPTransportLwIP.h` |
| Zephyr RTOS (POSIX API) | `PosixUdpTransport` | `PulseCoAPTransportPosix.h` |
| NuttX RTOS | `PosixUdpTransport` | `PulseCoAPTransportPosix.h` |
| RIOT OS (POSIX sockets) | `PosixUdpTransport` | `PulseCoAPTransportPosix.h` |
| Linux / macOS (gateway, tool) | `PosixUdpTransport` | `PulseCoAPTransportPosix.h` |
