# FAQ

---

### Why CoAP instead of MQTT or HTTP?

CoAP is purpose-built for constrained devices and lossy networks:

- **UDP-based** — no persistent TCP connection, no three-way handshake, no flow control overhead. One request fits in one UDP datagram (typically 60–120 bytes).
- **Request/response + pub/sub in one protocol** — GET, PUT, POST, DELETE for REST semantics; Observe for push. MQTT requires a separate broker; HTTP has no native push.
- **Built-in reliability** — CON/ACK retransmission is in the protocol, not in application code.
- **Designed for embedded** — payloads fit in SRAM; the stack runs in a tight `loop()`.

Use MQTT when you already run a broker and need fan-out to many subscribers. Use HTTP when you talk to web services that only speak HTTP. Use CoAP when the device itself is the server and you want minimal overhead.

---

### Does PulseCoAP allocate heap memory?

No. PulseCoAP uses zero dynamic allocation (`malloc`/`new`). All buffers — message, option list, observer table, transaction pool, deferred slots — live in stack or `static` storage, sized at compile time by the `PULSECOAP_*` macros.

This is intentional: heap fragmentation on a microcontroller can produce hard-to-reproduce crashes hours into a run. Compile-time sizing makes memory usage predictable.

---

### How many resources can I register?

The default is `PULSECOAP_MAX_RESOURCES = 8`. Raise it if you need more:

```cpp
#define PULSECOAP_MAX_RESOURCES 16
#include <PulseCoAP.h>
```

Each resource entry is a small struct (pointer to path, pointer to handler, bitmask). The cost is ~20 bytes per slot on a 32-bit platform.

---

### How many observers can one resource have?

`PULSECOAP_MAX_OBSERVERS` (default 8) across **all** resources combined. If you need 4 clients observing `/temp` and 4 observing `/humidity`, set it to 8 or higher.

---

### My CON request is timing out. What should I check?

1. **Firewall / NAT** — UDP 5683 may be blocked or NAT-translated.
2. **Port mismatch** — server and client must use the same port.
3. **`poll()` not called** — `server.poll()` and `client.poll()` must be called every `loop()` iteration. If `loop()` is blocked for more than `PULSECOAP_ACK_TIMEOUT_MS` (default 2 000 ms), retransmissions are delayed.
4. **Too few transaction slots** — if `PULSECOAP_MAX_TRANSACTIONS` is exhausted, `client.get()` returns `false`. Check the return value.
5. **WiFi disconnected** — check `WiFi.status()` before sending.

---

### Can I run server and client on the same device?

Yes. Use two separate UDP objects on different ports:

```cpp
WiFiUDP serverUdp;   // binds to 5683
WiFiUDP clientUdp;   // binds to 5684 (ephemeral)

pulsecoap::ArduinoUdpTransport serverTransport(serverUdp);
pulsecoap::ArduinoUdpTransport clientTransport(clientUdp);

pulsecoap::Server server(serverTransport);
pulsecoap::TransactionPool txPool;
pulsecoap::Client client(clientTransport, txPool);
```

Set `loopbackEp` to the device's own WiFi IP (not `127.0.0.1` — the Arduino UDP stack typically does not loopback on localhost). See the [Observe](/use-cases/observe.md) and [URI Templates](/use-cases/uri-templates.md) examples.

---

### What happens when `PULSECOAP_MAX_TRANSACTIONS` is full?

`client.get()`, `client.put()`, `client.post()`, `client.del()`, and `client.observe()` return `false` and do not send. The pending transaction slots free as CON messages are ACK'd or time out. Design around this by not sending more than `PULSECOAP_MAX_TRANSACTIONS` simultaneous CON requests.

---

### Can I send a JSON payload?

Yes. Set `contentFormat` and build the payload string:

```cpp
res.contentFormat = pulsecoap::ContentFormat::Json;
res.setPayload("{\"temp\":24.5,\"unit\":\"C\"}");
```

For PUT/POST with JSON, read `req.payload()` and parse it manually (or with a JSON library like ArduinoJson).

---

### How do I send CBOR?

```cpp
res.contentFormat = pulsecoap::ContentFormat::Cbor;
// Build the CBOR bytes in a buffer, then:
res.setPayload(cborBuf, cborLen);
```

PulseCoAP is encoding-agnostic — it sends whatever bytes you put in the payload.

---

### My observer keeps receiving old values after I reconnect. Why?

The server tracks observers by `(IP, port, token)`. If the client restarts with a new ephemeral port, the old subscription becomes stale. The stale entry is cleaned up automatically when:

- The server sends a CON notification and the client never ACKs (removed after `PULSECOAP_MAX_RETRANSMIT` retries).
- The client sends a RST in response to a notification it does not recognize.

Send periodic CON notifications (rather than always NON) to drive this cleanup:

```cpp
static uint32_t lastCon = 0;
bool confirmable = (millis() - lastCon >= 60000);
if (confirmable) lastCon = millis();
server.notify("/temp", buf, len, ContentFormat::TextPlain, confirmable);
```

---

### Is block-wise transfer (RFC 7959) supported?

Yes, as an opt-in module:

```cpp
#define PULSECOAP_ENABLE_BLOCKWISE 1
#include <PulseCoAP.h>
```

Block-wise enables payloads larger than one datagram (`PULSECOAP_MAX_MSG_SIZE`). The `Request::payload()` / `payloadLength()` accessors return the fully reassembled body on the final block, so handlers do not need to change.

See [Configuration](/guide/configuration.md) for block-wise tuning macros.

---

### Is DTLS (encrypted CoAP) supported?

Yes, as an opt-in module using mbedTLS PSK:

```cpp
#define PULSECOAP_ENABLE_DTLS 1
#include <PulseCoAP.h>
```

See [Transports & Platforms](/guide/transports.md#dtlstransport) for setup. Only PSK mode is supported; certificate-based PKI is not in scope.

---

### How do I port PulseCoAP to a new platform?

Implement the three-method `Transport` interface:

```cpp
class MyTransport : public pulsecoap::Transport {
public:
    bool   begin(uint16_t localPort) override;
    bool   send(const pulsecoap::Endpoint& to,
                const uint8_t* data, size_t length) override;
    size_t receive(uint8_t* buffer, size_t capacity,
                   pulsecoap::Endpoint& from) override;
};
```

`receive()` must be non-blocking (return 0 immediately when the queue is empty). See [Transports & Platforms](/guide/transports.md#writing-a-custom-transport) for details.

---

### What C++ standard does PulseCoAP require?

C++11 or later. All three toolchains it targets (GCC for AVR/ARM/RISC-V, Xtensa GCC for ESP32/ESP8266, Clang for macOS/Linux) support C++11 with `-std=c++11`.

---

### Can I use PulseCoAP without Arduino?

Yes. The core library (`PulseCoAPMessage`, `PulseCoAPTransaction`, `PulseCoAPServer`, `PulseCoAPClient`) has no Arduino dependencies. Use `PosixUdpTransport` on Linux/macOS:

```sh
g++ -std=c++11 -Isrc \
    your_app.cpp \
    src/PulseCoAPMessage.cpp src/PulseCoAPTransaction.cpp \
    src/PulseCoAPServer.cpp  src/PulseCoAPClient.cpp \
    -o my_app
```

See [Transports & Platforms](/guide/transports.md#posixudptransport).
