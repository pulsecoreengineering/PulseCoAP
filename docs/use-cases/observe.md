# Observe — Live Push

Subscribe to multiple resources simultaneously. Each subscription has its own callback and can be cancelled independently (RFC 7641).

**Features covered:** `client.observe()`, `cancelObserve()`, multiple simultaneous observations, server + client on one device (loopback).

---

## What this builds

Three observable resources — `/temp`, `/humidity`, `/uptime` — served from a single ESP32. A client on the same board subscribes to all three at once and receives independent push updates every few seconds. After 30 seconds, the temperature subscription is cancelled; humidity and uptime keep streaming.

---

## Full sketch

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";

// ── CoAP stack ────────────────────────────────────────────────────────────
WiFiUDP                        serverUdp;
pulsecoap::ArduinoUdpTransport serverTransport(serverUdp);
pulsecoap::Server              server(serverTransport);

WiFiUDP                        clientUdp;
pulsecoap::ArduinoUdpTransport clientTransport(clientUdp);
pulsecoap::TransactionPool     txPool;
pulsecoap::Client              client(clientTransport, txPool);

pulsecoap::Endpoint loopbackEp;
bool tempCancelled = false;

// ── Server handlers ───────────────────────────────────────────────────────
static float g_temp = 24.0f;
static int   g_hum  = 61;

void handleTemp(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", g_temp);
    res.code = pulsecoap::Code::Content;
    res.setPayload(buf);
}

void handleHumidity(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d", g_hum);
    res.code = pulsecoap::Code::Content;
    res.setPayload(buf);
}

void handleUptime(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000UL);
    res.code = pulsecoap::Code::Content;
    res.setPayload(buf);
}

// ── Client observe callbacks — one per resource ───────────────────────────
void onTemp(const pulsecoap::ClientResponse& res, void*) {
    char buf[res.payloadLength + 1];
    memcpy(buf, res.payload, res.payloadLength);
    buf[res.payloadLength] = '\0';
    Serial.printf("[temp]     %s °C\n", buf);
}

void onHumidity(const pulsecoap::ClientResponse& res, void*) {
    char buf[res.payloadLength + 1];
    memcpy(buf, res.payload, res.payloadLength);
    buf[res.payloadLength] = '\0';
    Serial.printf("[humidity] %s %%\n", buf);
}

void onUptime(const pulsecoap::ClientResponse& res, void*) {
    char buf[res.payloadLength + 1];
    memcpy(buf, res.payload, res.payloadLength);
    buf[res.payloadLength] = '\0';
    Serial.printf("[uptime]   %s s\n", buf);
}

void setup() {
    Serial.begin(115200);
    delay(300);

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    auto ip = WiFi.localIP();
    Serial.printf("\nIP: %s\n", ip.toString().c_str());

    // Server: register three observable resources
    server.addResource("/temp",     pulsecoap::MethodGet, handleTemp,     nullptr, true);
    server.addResource("/humidity", pulsecoap::MethodGet, handleHumidity, nullptr, true);
    server.addResource("/uptime",   pulsecoap::MethodGet, handleUptime,   nullptr, true);
    server.begin(5683);

    // Client: loopback to the same device
    loopbackEp.ip[0]=ip[0]; loopbackEp.ip[1]=ip[1];
    loopbackEp.ip[2]=ip[2]; loopbackEp.ip[3]=ip[3];
    loopbackEp.port = 5683;
    clientUdp.begin(5684);
    client.begin();
    delay(200);

    // Subscribe to all three independently
    client.observe(loopbackEp, "/temp",     onTemp,     nullptr);
    client.observe(loopbackEp, "/humidity", onHumidity, nullptr);
    client.observe(loopbackEp, "/uptime",   onUptime,   nullptr);

    Serial.println("Subscribed. /temp cancels after 30 s.\n");
}

void loop() {
    uint32_t now = millis();
    server.poll(now);
    client.poll(now);

    // Push updates every 3 seconds
    static uint32_t lastNotify = 0;
    if (now - lastNotify >= 3000) {
        lastNotify = now;

        char t[12], h[8], u[16];
        snprintf(t, sizeof(t), "%.1f", g_temp);
        snprintf(h, sizeof(h), "%d",   g_hum);
        snprintf(u, sizeof(u), "%lu",  now / 1000UL);

        server.notify("/temp",     (const uint8_t*)t, strlen(t));
        server.notify("/humidity", (const uint8_t*)h, strlen(h));
        server.notify("/uptime",   (const uint8_t*)u, strlen(u));
    }

    // Cancel /temp after 30 seconds
    if (!tempCancelled && now >= 30000) {
        tempCancelled = true;
        client.cancelObserve(loopbackEp, "/temp");
        Serial.println("\n[client] /temp cancelled. Only /humidity and /uptime remain.\n");
    }
}
```

---

## Key points

### Registering the subscription

```cpp
client.observe(loopbackEp, "/temp",     onTemp,     nullptr);
client.observe(loopbackEp, "/humidity", onHumidity, nullptr);
client.observe(loopbackEp, "/uptime",   onUptime,   nullptr);
```

Each call sends a CON GET with `Observe: 0`. The registration is retransmitted automatically if no response arrives. Once the server ACKs, the callback fires for the initial value and again for every subsequent `notify()`.

Multiple resources on the same server each occupy their own pending slot and are tracked independently by `(server, path)`.

### The notification callback

`onResponse` fires:
1. Once with the initial value (the server's response to the subscribe request).
2. Once per `server.notify()` push.

The `ClientResponse` is only valid during the callback — copy `res.payload` if you need it later.

### Cancelling one subscription

```cpp
client.cancelObserve(loopbackEp, "/temp");
```

Sends a GET with `Observe: 1` to deregister just `/temp`. The other two subscriptions continue unaffected. Returns `false` if no matching subscription is found.

### Server side: notifying all subscribers

```cpp
server.notify("/temp", (const uint8_t*)t, strlen(t));
```

`notify()` pushes to every current subscriber of that path. It is safe to call even when there are zero observers — it silently does nothing in that case.

---

## Splitting across two boards

The sketch runs server and client on one ESP32 via loopback. To split:

1. Copy the **server block** (everything from `WiFiUDP serverUdp` through `server.begin()`) to a separate sketch and flash it onto Board A.
2. In the **client sketch** on Board B, replace `loopbackEp` with Board A's actual IP:

```cpp
pulsecoap::Endpoint serverEp = {{192, 168, 1, 42}, 5683};
client.observe(serverEp, "/temp", onTemp, nullptr);
```

3. The client no longer needs `server.*` calls.

---

## Observation lifecycle

```
Client                                Server
  │                                     │
  │── CON GET /temp (Observe: 0) ──────>│
  │<── ACK + Content (initial value) ───│  ← onTemp() fires (initial)
  │                                     │
  │   (server calls notify() later)     │
  │<── NON 2.05 Content ────────────────│  ← onTemp() fires
  │<── NON 2.05 Content ────────────────│  ← onTemp() fires
  │                                     │
  │── CON GET /temp (Observe: 1) ──────>│  cancelObserve()
  │<── ACK + Content ───────────────────│  (subscription removed)
```
