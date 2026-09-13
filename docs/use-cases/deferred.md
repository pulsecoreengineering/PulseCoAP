# Deferred Response

A handler ACKs a CON request immediately and sends the real response later, after slow work completes (RFC 7252 §5.2.2).

**Features covered:** `res.deferred`, `req.deferHandle`, `server.respond()`, `client.setTimeoutHandler()`.

---

## The problem

Some handlers cannot respond immediately:

- I2C sensor that takes 50–200 ms to settle
- Flash read or database lookup
- Waiting for a lock or a radio to become idle

If the handler blocks, the CoAP server stalls: no other requests are processed and the `loop()` watchdog can fire. Worse, the client retransmits the CON packet repeatedly because it never saw an ACK.

---

## The solution: deferred response

```
Client                           Server
  │                                │
  │── CON GET /sensors/slow ──────>│
  │<── empty ACK ──────────────────│  handler sets res.deferred = true
  │                                │  (slow work starts in background)
  │                                │  (server.poll() keeps running)
  │                                │
  │   (~2 seconds later)           │
  │<── CON 2.05 Content ───────────│  server.respond() called
  │── ACK ────────────────────────>│
```

The client gets an empty ACK right away — it stops retransmitting. The real payload arrives in a separate CON message when the work is done.

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

// ── Deferred request state ────────────────────────────────────────────────
struct PendingWork {
    bool          active     = false;
    pulsecoap::DeferHandle handle    = pulsecoap::kInvalidDeferHandle;
    uint32_t      startedMs  = 0;
    static constexpr uint32_t kWorkDurationMs = 2000;
};
static PendingWork pending;

// ── Handler: defers immediately ───────────────────────────────────────────
void handleSlowSensor(const pulsecoap::Request& req,
                      pulsecoap::Response& res, void*) {
    if (pending.active) {
        // Already busy — reject with 5.03 Service Unavailable
        res.code = pulsecoap::Code::ServiceUnavailable;
        return;
    }

    res.deferred = true;              // sends empty ACK to stop retransmit
    pending.active    = true;
    pending.handle    = req.deferHandle;   // save for later
    pending.startedMs = millis();
}

void setup() {
    Serial.begin(115200);

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) delay(300);
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());

    server.addResource("/sensors/slow", pulsecoap::MethodGet, handleSlowSensor);
    server.begin(5683);
    Serial.println("Test: coap-client -m get coap://<IP>/sensors/slow");
}

void loop() {
    uint32_t now = millis();
    server.poll(now);

    // Check if the slow work has finished
    if (pending.active &&
        (now - pending.startedMs >= PendingWork::kWorkDurationMs)) {

        // Do the actual slow work (here: fake a sensor read)
        float value = 22.5f + (float)(now % 500) / 100.0f;
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", value);

        server.respond(pending.handle,
                       pulsecoap::Code::Content,
                       buf,
                       pulsecoap::ContentFormat::TextPlain);

        pending.active = false;
        pending.handle = pulsecoap::kInvalidDeferHandle;
    }
}
```

---

## Key points

### Setting up the defer

Inside the handler, set `res.deferred = true` and save `req.deferHandle`:

```cpp
void handleSlowSensor(const pulsecoap::Request& req,
                      pulsecoap::Response& res, void*) {
    res.deferred = true;
    pending.handle    = req.deferHandle;
    pending.startedMs = millis();
    pending.active    = true;
}
```

The server reads `res.deferred` after the handler returns and sends an empty ACK to the client immediately. The handler itself must return quickly — do not block here.

### Sending the deferred response

When the work is done, call `server.respond()`:

```cpp
server.respond(pending.handle,
               pulsecoap::Code::Content,
               buf,
               pulsecoap::ContentFormat::TextPlain);
```

- **`handle`** — the `DeferHandle` saved from `req.deferHandle`. Pass it once; `respond()` invalidates it.
- **`code`** — any valid response code (`Code::Content`, `Code::Changed`, `Code::InternalServerError`, etc.).
- **Payload** — C-string overload or `(uint8_t*, size_t)`.
- **`contentFormat`** — defaults to `ContentFormat::TextPlain`.

`respond()` returns `false` if the handle is invalid or already consumed.

### Guard against concurrent requests

A second request while the first is still pending should be rejected immediately:

```cpp
if (pending.active) {
    res.code = pulsecoap::Code::ServiceUnavailable;
    return;
}
```

For more than one concurrent deferred request, keep an array of `PendingWork` structs up to `PULSECOAP_MAX_DEFERRED` (default 4).

### Multiple simultaneous deferred requests

```cpp
#define PULSECOAP_MAX_DEFERRED 4
```

The server allocates `PULSECOAP_MAX_DEFERRED` deferred slots. Each `DeferHandle` occupies one slot until `server.respond()` consumes it. Track handles in an array:

```cpp
struct PendingWork {
    bool active = false;
    pulsecoap::DeferHandle handle = pulsecoap::kInvalidDeferHandle;
    uint32_t startedMs = 0;
};
static PendingWork queue[4];
```

---

## Testing

```sh
# From a PC on the same LAN (libcoap):
coap-client -m get coap://192.168.1.42/sensors/slow
# Output appears ~2 seconds after the command — that is the deferred response.
```

The client waits silently after the initial ACK. The response arrives as a separate CON message retransmitted until the client ACKs it.

---

## DeferHandle validity

```cpp
using DeferHandle = uint8_t;
static constexpr DeferHandle kInvalidDeferHandle = 0xFF;
```

Check against `kInvalidDeferHandle` before calling `respond()` if you are unsure whether a slot is active:

```cpp
if (pending.handle != pulsecoap::kInvalidDeferHandle) {
    server.respond(pending.handle, pulsecoap::Code::Content, "done");
    pending.handle = pulsecoap::kInvalidDeferHandle;
}
```
