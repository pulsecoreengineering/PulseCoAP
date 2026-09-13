# URI Templates

One handler serves `/sensors/0`, `/sensors/1`, `/sensors/2`, and any other ID — without registering each path individually.

**Features covered:** URI template (`:param`), `req.pathParam()`, exact path priority, server + client loopback.

---

## The problem without templates

```cpp
// Without templates you register every path explicitly:
server.addResource("/sensors/0", MethodGet, handleSensor0);
server.addResource("/sensors/1", MethodGet, handleSensor1);
server.addResource("/sensors/2", MethodGet, handleSensor2);
// ... and so on for every ID, pin, channel, or device
```

This uses one resource slot per path and forces you to duplicate handler code.

---

## The solution: URI template

```cpp
// One registration covers all IDs:
server.addResource("/sensors/:id", pulsecoap::MethodGet, handleSensor);
```

Inside the handler, `req.pathParam("id")` returns the matched segment as a C-string:

```cpp
// Request: GET /sensors/42
const char* id = req.pathParam("id");  // "42"
```

---

## Full sketch

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";

WiFiUDP                        udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server              server(transport);

WiFiUDP                        clientUdp;
pulsecoap::ArduinoUdpTransport clientTransport(clientUdp);
pulsecoap::TransactionPool     txPool;
pulsecoap::Client              client(clientTransport, txPool);

pulsecoap::Endpoint loopbackEp;

// ── Virtual sensor bank ───────────────────────────────────────────────────
struct Sensor { const char* name; float value; };
static const Sensor kSensors[] = {
    { "temperature", 24.3f },
    { "humidity",    61.0f },
    { "pressure",  1013.2f },
};
static const uint8_t kSensorCount = sizeof(kSensors) / sizeof(kSensors[0]);

// ── One handler covers ALL /sensors/:id paths ─────────────────────────────
void handleSensor(const pulsecoap::Request& req,
                  pulsecoap::Response& res, void*) {
    const char* idStr = req.pathParam("id");
    if (!idStr) {
        res.code = pulsecoap::Code::BadRequest;
        return;
    }

    int id = atoi(idStr);
    if (id < 0 || id >= (int)kSensorCount) {
        res.code = pulsecoap::Code::NotFound;
        res.setPayload("sensor id out of range");
        return;
    }

    static char buf[32];
    snprintf(buf, sizeof(buf), "%s=%.1f",
             kSensors[id].name, kSensors[id].value);
    res.code          = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload(buf);
}

// ── Client callback ───────────────────────────────────────────────────────
void onSensor(const pulsecoap::ClientResponse& res, void* ctx) {
    int id = (int)(intptr_t)ctx;
    if (res.code == pulsecoap::Code::Content) {
        char buf[res.payloadLength + 1];
        memcpy(buf, res.payload, res.payloadLength);
        buf[res.payloadLength] = '\0';
        Serial.printf("[sensor %d] %s\n", id, buf);
    } else {
        Serial.printf("[sensor %d] error %d.%02d\n",
                      id,
                      (uint8_t)res.code >> 5,
                      (uint8_t)res.code & 0x1F);
    }
}

void setup() {
    Serial.begin(115200);

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) delay(300);
    auto ip = WiFi.localIP();
    Serial.printf("IP: %s\n", ip.toString().c_str());

    server.addResource("/sensors/:id", pulsecoap::MethodGet, handleSensor);
    server.begin(5683);

    loopbackEp.ip[0]=ip[0]; loopbackEp.ip[1]=ip[1];
    loopbackEp.ip[2]=ip[2]; loopbackEp.ip[3]=ip[3];
    loopbackEp.port = 5683;
    clientUdp.begin(5684);
    client.begin();
    delay(200);

    // Query each sensor by ID
    for (int i = 0; i < (int)kSensorCount; ++i) {
        char path[20];
        snprintf(path, sizeof(path), "/sensors/%d", i);
        client.get(loopbackEp, path, onSensor, (void*)(intptr_t)i);
        delay(50);
    }

    // Invalid ID — expect 4.04 Not Found
    client.get(loopbackEp, "/sensors/99", onSensor, (void*)(intptr_t)99);
}

void loop() {
    uint32_t now = millis();
    server.poll(now);
    client.poll(now);
}
```

---

## Key points

### Template syntax

A `:name` segment in a path matches any single path segment:

| Registered path | Incoming request | `pathParam("id")` |
|---|---|---|
| `/sensors/:id` | `/sensors/0` | `"0"` |
| `/sensors/:id` | `/sensors/temp` | `"temp"` |
| `/sensors/:id` | `/sensors/42` | `"42"` |
| `/sensors/:id` | `/sensors/` | no match (empty) |
| `/sensors/:id` | `/sensors/a/b` | no match (two segments) |

### Exact paths take priority

If both `/sensors/special` and `/sensors/:id` are registered, a request for `/sensors/special` matches the exact path — the template is never invoked:

```cpp
server.addResource("/sensors/special", MethodGet, handleSpecial);
server.addResource("/sensors/:id",     MethodGet, handleSensor);

// GET /sensors/special → handleSpecial()
// GET /sensors/0       → handleSensor(), pathParam("id") == "0"
```

### Validating the matched value

`pathParam()` returns a pointer into PulseCoAP's internal buffer — valid only during the handler call. Parse it immediately:

```cpp
const char* idStr = req.pathParam("id");
if (!idStr) { res.code = Code::BadRequest; return; }

int id = atoi(idStr);   // or strtol, or a custom parser
if (id < 0 || id >= kMax) {
    res.code = Code::NotFound;
    return;
}
```

### Using `userContext` instead of a global lookup

For hardware-mapped channels, pass context through `addResource()`:

```cpp
struct Channel { uint8_t pin; const char* name; };
Channel ch0 = {A0, "temperature"};
Channel ch1 = {A1, "pressure"};

server.addResource("/ch/0", MethodGet, handleChannel, &ch0);
server.addResource("/ch/1", MethodGet, handleChannel, &ch1);

void handleChannel(const Request& /*req*/, Response& res, void* ctx) {
    Channel* ch = static_cast<Channel*>(ctx);
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d", analogRead(ch->pin));
    res.setPayload(buf);
}
```

For a small fixed set this avoids the template parsing entirely.

---

## Testing from a PC

```sh
coap-client -m get coap://192.168.1.42/sensors/0
# temperature=24.3

coap-client -m get coap://192.168.1.42/sensors/1
# humidity=61.0

coap-client -m get coap://192.168.1.42/sensors/99
# 4.04 Not Found
```

---

## Configuration

| Macro | Default | Description |
|---|---|---|
| `PULSECOAP_MAX_PATH_PARAMS` | 4 | Maximum `:param` segments per path |
| `PULSECOAP_MAX_PATH_PARAM_LEN` | 16 | Maximum length of each matched value |

See [Configuration](/guide/configuration.md) for details.
