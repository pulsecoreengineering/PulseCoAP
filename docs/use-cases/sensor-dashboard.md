# Sensor + LED Dashboard

Observable temperature resource, writable LED resource, and an HTTP gateway that bridges CoAP to a browser dashboard.

**Features covered:** observable resource + `notify()`, writable resource (PUT), Gateway integration.

---

## What this builds

```
┌─────────────────────────────────────┐
│         ESP32 CoAP Server           │
│                                     │
│  GET /sensors/temp  → read °C       │
│  PUT /actuators/led → on / off      │
│  OBS /sensors/temp  → push updates  │
└─────────────┬───────────────────────┘
              │ CoAP (UDP 5683)
              ▼
┌─────────────────────────────────────┐
│        Node.js Gateway              │
│                                     │
│  observes /sensors/temp             │
│  exposes HTTP /api/temp             │
│  exposes HTTP POST /api/led         │
└─────────────┬───────────────────────┘
              │ HTTP / WebSocket
              ▼
         Browser dashboard
```

---

## Server sketch (ESP32)

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";
const uint8_t LED_PIN = 2;  // built-in LED on most ESP32 dev boards

WiFiUDP                        udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server              server(transport);

// ── Temperature (observable) ─────────────────────────────────────────────
static float g_temp = 22.0f;

void handleTemp(const pulsecoap::Request& /*req*/,
                pulsecoap::Response& res, void*) {
    static char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", g_temp);
    res.code = pulsecoap::Code::Content;
    res.setPayload(buf);
}

// ── LED (writable) ──────────────────────────────────────────────────────
void handleLed(const pulsecoap::Request& req,
               pulsecoap::Response& res, void*) {
    if (req.method == pulsecoap::Code::Put) {
        bool on = (req.payloadLength() >= 2 &&
                   strncmp((const char*)req.payload(), "on", 2) == 0);
        digitalWrite(LED_PIN, on ? HIGH : LOW);
        res.code = pulsecoap::Code::Changed;
    } else {
        res.code = pulsecoap::Code::Content;
        res.setPayload(digitalRead(LED_PIN) ? "on" : "off");
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) delay(300);
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

    server.addResource("/sensors/temp",
                       pulsecoap::MethodGet,
                       handleTemp, nullptr, /*observable=*/true);

    server.addResource("/actuators/led",
                       pulsecoap::MethodGet | pulsecoap::MethodPut,
                       handleLed);

    server.begin(5683);
}

void loop() {
    uint32_t now = millis();
    server.poll(now);

    // Simulate temperature drifting every 3 seconds; push to observers
    static uint32_t lastUpdate = 0;
    if (now - lastUpdate >= 3000) {
        lastUpdate = now;
        g_temp += (random(-10, 10) * 0.1f);  // ± 1 °C random walk
        g_temp = constrain(g_temp, 18.0f, 30.0f);

        char buf[12];
        snprintf(buf, sizeof(buf), "%.1f", g_temp);
        server.notify("/sensors/temp", buf);
    }
}
```

---

## Key points

### Observable temperature

```cpp
server.addResource("/sensors/temp",
                   pulsecoap::MethodGet,
                   handleTemp, nullptr, /*observable=*/true);
```

Setting `observable = true` allows clients to subscribe. Every call to `notify()` pushes the new value to all current subscribers without them polling.

### Writable LED

```cpp
server.addResource("/actuators/led",
                   pulsecoap::MethodGet | pulsecoap::MethodPut,
                   handleLed);
```

A single handler covers both methods. Check `req.method` inside to branch:

```cpp
if (req.method == pulsecoap::Code::Put) { /* write */ }
else { /* read */ }
```

### Driving notify() from the main loop

```cpp
// Every 3 seconds, push the new reading to all subscribers:
server.notify("/sensors/temp", buf);
```

`notify()` does nothing if there are no observers — safe to call unconditionally. Use `server.observerCount()` to skip the reading if nothing is subscribed yet.

### CON notify for dead-peer detection

Send a Confirmable notification periodically. Any observer that never ACKs after `PULSECOAP_MAX_RETRANSMIT` retries is silently removed:

```cpp
static uint32_t lastCon = 0;
bool confirmable = (now - lastCon >= 60000);
if (confirmable) lastCon = now;

server.notify("/sensors/temp", buf, strlen(buf),
              pulsecoap::ContentFormat::TextPlain, confirmable);
```

---

## Gateway integration

A Node.js process (or any CoAP client library) can bridge the observable resource to HTTP:

```js
const coap = require('coap');
const http = require('http');

let latestTemp = '--';

// Subscribe to /sensors/temp via CoAP Observe
const req = coap.request({
    host: '192.168.1.42',
    method: 'GET',
    pathname: '/sensors/temp',
    observe: true,
});

req.on('response', (res) => {
    res.on('data', (chunk) => {
        latestTemp = chunk.toString();
        console.log('temp updated:', latestTemp);
    });
});
req.end();

// Expose the latest value over HTTP for the browser
http.createServer((req, res) => {
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify({ temp: latestTemp }));
}).listen(3000);
```

The browser polls `/` (or a WebSocket) and gets fresh values without ever touching UDP.

---

## Testing without a gateway

Use `coap-client` (libcoap) from a PC on the same LAN:

```sh
# Read temperature
coap-client -m get coap://192.168.1.42/sensors/temp

# Subscribe to temperature updates
coap-client -m get -s 60 coap://192.168.1.42/sensors/temp

# Turn LED on
coap-client -m put -e "on" coap://192.168.1.42/actuators/led

# Turn LED off
coap-client -m put -e "off" coap://192.168.1.42/actuators/led
```
