# Use Cases

These examples cover real-world embedded patterns. Each one is self-contained and designed to be adapted directly to a project.

## Which example to read first?

| If you want to see… | Read |
|---|---|
| Observable resource + writable resource + Gateway integration | [Sensor + LED Dashboard](sensor-dashboard.md) |
| Multiple simultaneous subscriptions, independent cancellation | [Observe — Live Push](observe.md) |
| Handler ACKs immediately, replies later after slow work | [Deferred Response](deferred.md) |
| One handler covering `/sensors/0`, `/sensors/1`, … | [URI Templates](uri-templates.md) |
| Client discovering servers + resources on the LAN | [Resource Discovery](discovery.md) |

## PulseCoAP features by example

| Feature | Sensor Dashboard | Observe | Deferred | URI Templates | Discovery |
|---|:---:|:---:|:---:|:---:|:---:|
| Observable resource + `notify()` | ✓ | ✓ | — | — | ✓ |
| Writable resource (PUT) | ✓ | — | — | — | — |
| `res.deferred` + `server.respond()` | — | — | ✓ | — | — |
| URI template (`:param`) | — | — | — | ✓ | — |
| `req.pathParam()` | — | — | — | ✓ | — |
| `client.observe()` + `cancelObserve()` | — | ✓ | — | — | — |
| Multiple simultaneous observations | — | ✓ | — | — | — |
| `client.discover()` multicast | — | — | — | — | ✓ |
| `/.well-known/core` + `setResourceType()` | — | — | — | — | ✓ |
| `client.setTimeoutHandler()` | — | — | ✓ | — | — |
| Server + Client on one device (loopback) | — | ✓ | — | ✓ | ✓ |
| Gateway integration | ✓ | — | — | — | — |

## General patterns

### Observable resource

Mark a resource observable at registration. Call `notify()` any time the value changes.

```cpp
server.addResource("/sensors/temp", MethodGet, handleTemp,
                    nullptr, /*observable=*/true);

// Whenever the value changes:
server.notify("/sensors/temp",
              reinterpret_cast<const uint8_t*>(buf), strlen(buf));
```

### Writable resource

Accept both GET and PUT on the same path. Read `req.payload()` in the handler.

```cpp
server.addResource("/actuators/led",
                   MethodGet | MethodPut,
                   handleLed);

void handleLed(const Request& req, Response& res, void*) {
    if (req.method == Code::Put) {
        bool on = (strncmp((char*)req.payload(), "on", 2) == 0);
        digitalWrite(LED_PIN, on ? HIGH : LOW);
        res.code = Code::Changed;
    } else {
        res.setPayload(digitalRead(LED_PIN) ? "on" : "off");
        res.code = Code::Content;
    }
}
```

### CON notification for dead-peer detection

Send a CON notification periodically. If a subscriber never ACKs after `PULSECOAP_MAX_RETRANSMIT` retries, it is automatically removed from the observer list.

```cpp
// Every 60 seconds send a CON notify to clean up stale subscriptions:
static uint32_t lastCon = 0;
bool confirmable = (millis() - lastCon >= 60000);
if (confirmable) lastCon = millis();
server.notify("/sensors/temp", buf, len,
              ContentFormat::TextPlain, confirmable);
```

### Pass context through to a handler

Use `userContext` to avoid globals when multiple instances of the same handler serve different resources.

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
