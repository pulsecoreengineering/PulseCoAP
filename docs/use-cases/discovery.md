# Resource Discovery

A client asks the server what resources it exposes before sending any data requests. Works both unicast (known server IP) and multicast (find all CoAP servers on the LAN).

**Features covered:** `/.well-known/core` auto-registration, `setResourceType()`, `client.get()` unicast discovery, `client.discover()` multicast, server + client loopback.

---

## What this builds

```
Client                               Server
  │                                    │
  │── GET /.well-known/core ──────────>│
  │<── 2.05 Content (CoRE Link) ───────│  </temp>;obs;rt="temperature",
  │                                    │  </humidity>;obs;rt="humidity",
  │                                    │  </uptime>
  │── GET /temp ──────────────────────>│
  │<── 2.05 Content (24.3) ────────────│
  │── GET /humidity ──────────────────>│
  │<── 2.05 Content (61) ──────────────│
```

---

## Full sketch (unicast)

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";

WiFiUDP                        serverUdp;
pulsecoap::ArduinoUdpTransport serverTransport(serverUdp);
pulsecoap::Server              server(serverTransport);

WiFiUDP                        clientUdp;
pulsecoap::ArduinoUdpTransport clientTransport(clientUdp);
pulsecoap::TransactionPool     txPool;
pulsecoap::Client              client(clientTransport, txPool);

pulsecoap::Endpoint loopbackEp;

// ── Server handlers ───────────────────────────────────────────────────────
void handleTemp(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    res.code = pulsecoap::Code::Content;
    res.setPayload("24.3");
}

void handleHumidity(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    res.code = pulsecoap::Code::Content;
    res.setPayload("61");
}

void handleUptime(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000UL);
    res.code = pulsecoap::Code::Content;
    res.setPayload(buf);
}

// ── Client callback: fires when /.well-known/core arrives ─────────────────
void onDiscovery(const pulsecoap::ClientResponse& res, void*) {
    Serial.println("\n/.well-known/core response:");
    Serial.write(res.payload, res.payloadLength);
    Serial.println();

    // After discovering, fetch each known resource
    client.get(loopbackEp, "/temp",     [](const pulsecoap::ClientResponse& r, void*) {
        char buf[r.payloadLength + 1];
        memcpy(buf, r.payload, r.payloadLength); buf[r.payloadLength] = '\0';
        Serial.printf("[/temp]     %s\n", buf);
    }, nullptr);

    client.get(loopbackEp, "/humidity", [](const pulsecoap::ClientResponse& r, void*) {
        char buf[r.payloadLength + 1];
        memcpy(buf, r.payload, r.payloadLength); buf[r.payloadLength] = '\0';
        Serial.printf("[/humidity] %s\n", buf);
    }, nullptr);
}

void setup() {
    Serial.begin(115200);

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) delay(300);
    auto ip = WiFi.localIP();
    Serial.printf("IP: %s\n", ip.toString().c_str());

    // Register resources; /.well-known/core is auto-registered by begin()
    server.addResource("/temp",     pulsecoap::MethodGet, handleTemp,     nullptr, true);
    server.addResource("/humidity", pulsecoap::MethodGet, handleHumidity, nullptr, true);
    server.addResource("/uptime",   pulsecoap::MethodGet, handleUptime,   nullptr, false);

    // Annotate resources with a CoRE resource type
    server.setResourceType("/temp",     "temperature");
    server.setResourceType("/humidity", "humidity");
    // /uptime intentionally has no rt=

    server.begin(5683);

    loopbackEp.ip[0]=ip[0]; loopbackEp.ip[1]=ip[1];
    loopbackEp.ip[2]=ip[2]; loopbackEp.ip[3]=ip[3];
    loopbackEp.port = 5683;
    clientUdp.begin(5684);
    client.begin();
    delay(200);

    // Discover: GET /.well-known/core
    client.get(loopbackEp, "/.well-known/core", onDiscovery);
}

void loop() {
    uint32_t now = millis();
    server.poll(now);
    client.poll(now);
}
```

**Expected Serial output:**

```
/.well-known/core response:
</temp>;obs;rt="temperature",</humidity>;obs;rt="humidity",</uptime>

[/temp]     24.3
[/humidity] 61
```

---

## `/.well-known/core` auto-registration

`server.begin()` automatically registers `/.well-known/core` using the RFC 6690 CoRE Link Format. You do not add it manually. The response lists every resource registered via `addResource()`:

```
</temp>;obs;rt="temperature",</humidity>;obs;rt="humidity",</uptime>
```

- **`obs`** — present when `observable = true` was passed to `addResource()`.
- **`rt="..."`** — present after `server.setResourceType(path, rt)`.

### Adding resource types

```cpp
server.setResourceType("/temp", "temperature");
```

Call after `addResource()`, before the first `poll()`. The pointer is stored by reference — pass a string literal or a static buffer. The `rt=` value appears in the discovery response as `rt="temperature"`.

---

## Multicast discovery (LAN scan)

To find all PulseCoAP servers on the local network without knowing their IPs, use `client.discover()`:

```cpp
void onFound(const pulsecoap::Endpoint& ep,
             const uint8_t* linkFormat, size_t length, void*) {
    Serial.printf("Server at %d.%d.%d.%d: %.*s\n",
        ep.ip[0], ep.ip[1], ep.ip[2], ep.ip[3],
        (int)length, (char*)linkFormat);
}

// In setup() or loop():
client.discover(onFound);
```

`discover()` sends a NON GET for `/.well-known/core` to the IANA all-CoAP-nodes multicast address `224.0.1.187:5683`. Every server on the LAN that has joined the group responds unicast; `onFound` fires once per responder.

The slot stays open for `PULSECOAP_DISCOVER_TIMEOUT_MS` (default 5 000 ms), then frees automatically.

**POSIX / Linux:** join the multicast group after `begin()`:

```cpp
transport.joinMulticastGroup("224.0.1.187");
```

**ESP32 with WiFiUDP:** the WiFi stack joins automatically.

### Custom multicast endpoint (IPv6)

```cpp
pulsecoap::Endpoint ff02;
ff02.isV6 = true;
ff02.port = 5683;
// FF02::FD — CoAP all-nodes site-local
const uint8_t ff02fd[16] = {0xFF,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0xFD};
memcpy(ff02.v6, ff02fd, 16);

client.discover(ff02, onFound);
```

---

## Testing from a PC

```sh
# Query a specific server's resource list
coap-client -m get coap://192.168.1.42/.well-known/core

# Multicast scan of the LAN (requires libcoap with multicast support)
coap-client -m get coap://224.0.1.187/.well-known/core
```

---

## Configuration

| Macro | Default | Description |
|---|---|---|
| `PULSECOAP_MAX_LINK_FORMAT_LEN` | 256 | Maximum byte length of the `/.well-known/core` response |
| `PULSECOAP_MAX_DISCOVERS` | 2 | Concurrent `discover()` slots |
| `PULSECOAP_DISCOVER_TIMEOUT_MS` | 5000 | How long a `discover()` slot stays open |

See [Configuration](/guide/configuration.md) for details.
