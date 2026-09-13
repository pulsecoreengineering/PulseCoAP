# Quick Start

## Installation

1. Download or clone the repository.
2. Copy (or symlink) the `src/` folder into your Arduino `libraries/` directory as `PulseCoAP`.
3. Restart the Arduino IDE.

```
~/Arduino/libraries/PulseCoAP/
  PulseCoAP.h
  PulseCoAPServer.h
  PulseCoAPClient.h
  PulseCoAPConfig.h
  ...
```

Alternatively, install from the Arduino Library Manager: search for **PulseCoAP**.

---

## Step 1 — A simple server

Expose a temperature reading as a CoAP resource. Any CoAP client (including `coap-client` from libcoap) can GET it.

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>

const char* kSsid = "YOUR_WIFI_SSID";
const char* kPass = "YOUR_WIFI_PASSWORD";

WiFiUDP udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server server(transport);

void handleTemp(const pulsecoap::Request& /*req*/, pulsecoap::Response& res, void* /*ctx*/) {
    static char buf[8];
    dtostrf(readTempC(), 0, 1, buf);   // replace with your real sensor
    res.code = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload(buf);
}

void setup() {
    Serial.begin(115200);
    WiFi.begin(kSsid, kPass);
    while (WiFi.status() != WL_CONNECTED) delay(250);
    Serial.println(WiFi.localIP());

    server.addResource("/sensors/temp", pulsecoap::MethodGet, handleTemp);
    server.begin(5683);   // standard CoAP port
}

void loop() {
    server.poll(millis());
}
```

Test from a PC:

```sh
coap-client -m get coap://<device-ip>/sensors/temp
# → 24.5
```

Or use the browser dashboard from [PulseCoAPGateway](https://github.com/pulsecoreengineering/PulseCoAPGateway).

---

## Step 2 — A simple client

Poll the server every 5 seconds from a second device (or a POSIX host). Responses arrive in a callback — the main loop is never blocked.

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>

const char* kSsid = "YOUR_WIFI_SSID";
const char* kPass = "YOUR_WIFI_PASSWORD";

pulsecoap::Endpoint kServer = {{192, 168, 1, 42}, 5683};

WiFiUDP udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::TransactionPool transactions;
pulsecoap::Client client(transport, transactions);

uint32_t lastReq = 0;

void onTemp(const pulsecoap::ClientResponse& res, void* /*ctx*/) {
    Serial.print("Temp: ");
    Serial.write(res.payload, res.payloadLength);
    Serial.println(" °C");
}

void setup() {
    Serial.begin(115200);
    WiFi.begin(kSsid, kPass);
    while (WiFi.status() != WL_CONNECTED) delay(250);
    client.begin();
}

void loop() {
    client.poll(millis());

    if (millis() - lastReq >= 5000) {
        lastReq = millis();
        client.get(kServer, "/sensors/temp", onTemp);
    }
}
```

Key points:
- The `TransactionPool` tracks the in-flight CON GET and retransmits it if the ACK is lost.
- `onTemp` fires once when the response arrives. There is no blocking wait.
- Adjust `kServer` to match the ESP32 running Step 1.

---

## Step 3 — Add Observe (live push)

Instead of polling, subscribe to the resource and have the server push every update automatically.

**Server side** — mark the resource observable and call `notify()` when the value changes:

```cpp
// In setup():
server.addResource("/sensors/temp", pulsecoap::MethodGet, handleTemp,
                    nullptr, /*observable=*/true);

// In loop():
static uint32_t lastNotify = 0;
if (millis() - lastNotify >= 5000) {
    lastNotify = millis();
    char buf[8];
    dtostrf(readTempC(), 0, 1, buf);
    server.notify("/sensors/temp",
                  reinterpret_cast<const uint8_t*>(buf), strlen(buf));
}
```

**Client side** — replace the polling loop with a single `observe()` call:

```cpp
void onTempUpdate(const pulsecoap::ClientResponse& res, void* /*ctx*/) {
    Serial.print("Update: ");
    Serial.write(res.payload, res.payloadLength);
    Serial.println();
}

void setup() {
    // ... WiFi connect, client.begin() ...
    client.observe(kServer, "/sensors/temp", onTempUpdate);
    // onTempUpdate fires with the initial value, then for every notify().
}

void loop() {
    client.poll(millis());
}
```

To cancel the subscription:

```cpp
client.cancelObserve(kServer, "/sensors/temp");
```

---

## What's next

- [Configuration](configuration.md) — tune buffer sizes, disable unused roles, enable block-wise or DTLS
- [Transports & Platforms](transports.md) — lwIP, POSIX, DTLS, custom transport
- [API Reference — Server](../api/server.md) / [Client](../api/client.md)
- [Use Cases](../use-cases/overview.md) — deferred responses, URI templates, resource discovery
