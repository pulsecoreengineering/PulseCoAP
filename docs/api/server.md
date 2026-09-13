# Server API

```cpp
#include <PulseCoAP.h>
using namespace pulsecoap;
```

## Callback types

```cpp
using ResourceHandler = void (*)(const Request& req, Response& res, void* userContext);
```

Called when a request matches a registered resource. Set fields on `res` to build the reply. `userContext` is whatever pointer you passed to `addResource()`.

---

## Server class

### Constructor

```cpp
explicit Server(Transport& transport);
```

Pass a concrete transport (e.g. `ArduinoUdpTransport`, `PosixUdpTransport`). The transport must outlive the `Server`.

---

### begin()

```cpp
bool begin(uint16_t localPort = 5683);
```

Binds the transport to `localPort` and auto-registers `/.well-known/core` (RFC 6690). Returns `false` if the port cannot be bound.

Call once in `setup()`, after all `addResource()` calls.

---

### addResource()

```cpp
bool addResource(const char* path,
                 uint8_t methods,
                 ResourceHandler handler,
                 void* userContext = nullptr,
                 bool observable   = false);
```

Registers a resource. Returns `false` when the resource table is full (`PULSECOAP_MAX_RESOURCES`).

**Parameters:**

| Parameter | Description |
|---|---|
| `path` | URI path starting with `/`. Must remain valid for the life of the `Server` — pass a string literal or a static buffer; it is not copied. Supports URI templates: a `:name` segment matches any single path segment (e.g. `/sensors/:id`). |
| `methods` | Bitmask of accepted methods: `MethodGet`, `MethodPost`, `MethodPut`, `MethodDelete` (combine with `|`). Requests with a method not in the mask receive 4.05 Method Not Allowed. |
| `handler` | Called synchronously inside `poll()` when a matching request arrives. |
| `userContext` | Passed through to `handler` unchanged. May be `nullptr`. |
| `observable` | Set `true` to allow clients to register Observe subscriptions. Required before `notify()` on this path will do anything. |

**Rules:**
- Add all resources **before** calling `begin()`.
- Exact paths take priority over URI templates when both could match.
- `path` is stored by pointer, not copied. Passing a stack-allocated buffer is a bug.

---

### setResourceType()

```cpp
bool setResourceType(const char* path, const char* rt);
```

Attaches a CoRE Link Format `rt=` (resource-type) attribute to a previously registered resource. Stored by pointer — the caller keeps `rt` alive.

Returns `false` if `path` is not found. Call after `addResource()`, before the first `poll()`.

The `rt=` value appears in the `/.well-known/core` response:

```
</sensors/temp>;obs;rt="temperature"
```

---

### poll()

```cpp
void poll(uint32_t nowMs);
```

The main scheduler tick. Call every `loop()` iteration.

On each tick:
1. Calls `transport.receive()` — processes any waiting request, dispatches it to the matching handler, sends the piggybacked response (or empty ACK for deferred).
2. Drives retransmission of any in-flight separate-response CON messages.
3. Checks for RST messages that signal a dead Observe observer (removes the subscriber).

Pass `millis()` on Arduino or an equivalent monotonic millisecond counter. The counter may roll over — PulseCoAP uses subtraction for all time comparisons.

---

### notify()

```cpp
bool notify(const char* path,
            const uint8_t* payload, size_t payloadLength,
            ContentFormat contentFormat = ContentFormat::TextPlain,
            bool confirmable = false);
```

Pushes `payload` to every client currently observing `path`.

Returns `false` if the resource is not registered or not marked `observable`. Returns `true` (and does nothing) if the resource has zero observers — this is not an error.

**`confirmable`:**
- `false` (default): NON notification — fire-and-forget, no retransmission.
- `true`: CON notification — retransmitted per the transaction pool. If a peer never ACKs after `PULSECOAP_MAX_RETRANSMIT` retries, it is silently removed from the observer list. Send CON notifications periodically to detect and clean up stale registrations (RFC 7641 §4.5).

Convenience overload for plain text:

```cpp
bool notify(const char* path, const char* text,
            ContentFormat contentFormat = ContentFormat::TextPlain,
            bool confirmable = false);
```

---

### respond()

```cpp
bool respond(DeferHandle handle, Code code,
             const uint8_t* payload, size_t payloadLength,
             ContentFormat contentFormat = ContentFormat::TextPlain);

// Convenience overload for plain text:
bool respond(DeferHandle handle, Code code, const char* text,
             ContentFormat contentFormat = ContentFormat::TextPlain);
```

Sends the deferred response for a handler that set `res.deferred = true`. `handle` comes from `req.deferHandle` inside the handler.

Returns `false` if `handle` is invalid or has already been consumed.

The response type (CON or NON) matches the original request — a CON request gets a CON separate response that is retransmitted until ACK'd.

---

### observerCount()

```cpp
uint8_t observerCount() const;
```

Returns the total number of active Observe subscriptions across all resources. Useful for diagnostics and for deciding whether to send CON or NON notifications.

---

## Request

Passed to a `ResourceHandler` by const reference. Do not store a pointer to it — it is only valid for the duration of the handler call.

```cpp
struct Request {
    Code method;              // MethodGet, MethodPut, MethodPost, MethodDelete
    const Message* message;   // Full decoded request — read extra options here
    Endpoint remote;          // Who sent the request
    DeferHandle deferHandle;  // Store and pass to server.respond() if deferred
    // ...
};
```

### `req.pathParam(name)`

```cpp
const char* pathParam(const char* name) const;
```

Returns the matched value for a URI template parameter, or `nullptr` if `name` is not in the template.

```cpp
// Resource registered as "/sensors/:id", request path "/sensors/42":
const char* id = req.pathParam("id");  // "42"
```

### `req.payload()` / `req.payloadLength()`

```cpp
const uint8_t* payload()       const;
size_t         payloadLength() const;
```

Returns the effective request payload. When block-wise transfer is enabled and this is the final block of a Block1 upload, these return the fully reassembled body. For plain requests, they return `message->payload()` / `message->payloadLength()`.

---

## Response

Filled by the `ResourceHandler`. The server reads it after the handler returns and sends the reply.

```cpp
struct Response {
    Code          code          = Code::Content;
    ContentFormat contentFormat = ContentFormat::TextPlain;
    const uint8_t* payload      = nullptr;
    size_t         payloadLength = 0;
    bool           deferred      = false;
};
```

### `res.setPayload()`

```cpp
void setPayload(const uint8_t* data, size_t length);
void setPayload(const char* text);     // convenience: sets length via strlen()
```

### `res.deferred`

Set `true` to send a deferred (separate) response. The server immediately sends an empty ACK to stop client retransmission. Save `req.deferHandle` and call `server.respond(handle, ...)` later.

```cpp
void handleSlowSensor(const Request& req, Response& res, void*) {
    res.deferred = true;
    pending.handle    = req.deferHandle;
    pending.startedMs = millis();
}

// Later in loop():
if (pending.active && millis() - pending.startedMs >= 2000) {
    server.respond(pending.handle, Code::Content, "22.5");
    pending.active = false;
}
```

---

## MethodMask

```cpp
enum MethodMask : uint8_t {
    MethodGet    = 1 << 0,
    MethodPost   = 1 << 1,
    MethodPut    = 1 << 2,
    MethodDelete = 1 << 3,
};
```

Combine with `|` to accept multiple methods:

```cpp
server.addResource("/sensors/led",
                   pulsecoap::MethodGet | pulsecoap::MethodPut,
                   handleLed);
```

---

## DeferHandle

```cpp
using DeferHandle = uint8_t;
static constexpr DeferHandle kInvalidDeferHandle = 0xFF;
```

Opaque handle identifying a deferred request slot. Store `req.deferHandle` inside the handler and pass it to `server.respond()` when the work is complete. Handles are valid only until `server.respond()` consumes them. Check against `kInvalidDeferHandle` to detect an uninitialized handle.
