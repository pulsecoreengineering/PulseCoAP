# Client API

```cpp
#include <PulseCoAP.h>
using namespace pulsecoap;
```

## Callback types

```cpp
// Fires once for get/put/post/del; fires once per notification for observe().
using ResponseHandler = void (*)(const ClientResponse& res, void* userContext);

// Fires when a Confirmable request exhausts PULSECOAP_MAX_RETRANSMIT retries.
using TimeoutHandler = void (*)(void* userContext);

// Fires once per responding server during a discover() collection window.
using DiscoverHandler = void (*)(const Endpoint& server,
                                  const uint8_t* linkFormat, size_t length,
                                  void* userContext);
```

---

## ClientResponse

```cpp
struct ClientResponse {
    Code          code;
    const uint8_t* payload;
    size_t         payloadLength;
    ContentFormat  contentFormat;
};
```

Passed to `ResponseHandler` by const reference. Only valid for the duration of the callback — copy `payload` if you need it later.

---

## TransactionPool

```cpp
class TransactionPool { /* ... */ };
```

Tracks in-flight Confirmable messages and drives retransmission. Create one shared pool and pass it to `Client`. On ESP32 with separate server and client stacks, each `Client` can share or own its own pool.

```cpp
pulsecoap::TransactionPool txPool;
pulsecoap::Client client(transport, txPool);
```

The pool holds `PULSECOAP_MAX_TRANSACTIONS` slots (default 4). Each in-flight CON request or CON notify occupies one slot until ACK'd or timed out.

---

## Client class

### Constructor

```cpp
Client(Transport& transport, TransactionPool& transactions);
```

Both `transport` and `transactions` must outlive the `Client`.

---

### begin()

```cpp
bool begin(uint16_t localPort = 0);
```

Binds the transport to `localPort`. Pass `0` to let the OS pick an ephemeral port. Call once in `setup()`.

---

### get()

```cpp
bool get(const Endpoint& server, const char* path,
         ResponseHandler onResponse,
         void* userContext = nullptr,
         bool confirmable  = true);
```

Sends a GET request. `onResponse` fires once when the response arrives.

Returns `false` if no pending slot is free (`PULSECOAP_MAX_TRANSACTIONS`).

---

### put()

```cpp
bool put(const Endpoint& server, const char* path,
         const uint8_t* payload, size_t payloadLength,
         ContentFormat contentFormat,
         ResponseHandler onResponse,
         void* userContext = nullptr,
         bool confirmable  = true);
```

Sends a PUT request with the given payload.

---

### post()

```cpp
bool post(const Endpoint& server, const char* path,
          const uint8_t* payload, size_t payloadLength,
          ContentFormat contentFormat,
          ResponseHandler onResponse,
          void* userContext = nullptr,
          bool confirmable  = true);
```

Sends a POST request.

---

### del()

```cpp
bool del(const Endpoint& server, const char* path,
         ResponseHandler onResponse,
         void* userContext = nullptr,
         bool confirmable  = true);
```

Sends a DELETE request.

---

### observe()

```cpp
bool observe(const Endpoint& server, const char* path,
             ResponseHandler onResponse,
             void* userContext = nullptr);
```

Registers an Observe subscription on `path` (sent Confirmable so the registration itself is retried if lost). `onResponse` fires for the initial value and for every subsequent notification.

Multiple resources on the same server can be observed simultaneously — each call occupies its own pending slot and is tracked by `(server, path)`.

Returns `false` if no pending slot is free.

---

### cancelObserve()

```cpp
bool cancelObserve(const Endpoint& server, const char* path);
```

Sends a GET with `Observe=1` to deregister the `(server, path)` subscription (RFC 7641 §3.6). Matches by path, so two simultaneous observations on the same server are cancelled independently.

Returns `false` if no matching subscription is found.

---

### discover()

```cpp
bool discover(DiscoverHandler onDiscover,
              void* userContext = nullptr,
              uint16_t port = 5683);

// Overload for a custom multicast destination (e.g. IPv6 FF02::FD):
bool discover(const Endpoint& multicastEp,
              DiscoverHandler onDiscover,
              void* userContext = nullptr);
```

Sends a NON GET for `/.well-known/core` to the IANA CoAP all-nodes multicast address `224.0.1.187:port`. Every PulseCoAP server on the LAN that has joined the group responds unicast; `onDiscover` fires once per responder.

The slot stays active for `PULSECOAP_DISCOVER_TIMEOUT_MS` ms (default 5 s), then frees automatically. Returns `false` if no discover slot is free (`PULSECOAP_MAX_DISCOVERS`) or if the send fails.

```cpp
void onFound(const pulsecoap::Endpoint& ep,
             const uint8_t* linkFormat, size_t length, void*) {
    Serial.printf("Device at %d.%d.%d.%d: %.*s\n",
        ep.ip[0], ep.ip[1], ep.ip[2], ep.ip[3],
        (int)length, (char*)linkFormat);
}

// In setup():
client.discover(onFound);
```

**POSIX / LAN only**: multicast requires the underlying socket to join the multicast group. Call `transport.joinMulticastGroup("224.0.1.187")` (POSIX) or ensure the WiFi stack joins automatically (ESP32 does this).

---

### allNodesEndpoint()

```cpp
static Endpoint allNodesEndpoint(uint16_t port = 5683);
```

Returns the IANA CoAP all-nodes multicast endpoint `{ 224, 0, 1, 187 }:port`. Useful when you want to build the endpoint manually.

---

### setTimeoutHandler()

```cpp
void setTimeoutHandler(TimeoutHandler onTimeout);
```

Registers a callback fired when any CON request exhausts all retransmission attempts. The callback receives the `userContext` of the timed-out request.

```cpp
void onTimeout(void* /*ctx*/) {
    Serial.println("Request timed out — device not responding");
}

client.setTimeoutHandler(onTimeout);
```

---

### poll()

```cpp
void poll(uint32_t nowMs);
```

The main scheduler tick. Call every `loop()` iteration.

On each tick:
1. Calls `transport.receive()` — matches any incoming response or notification to a pending slot and fires the callback.
2. Drives retransmission of any in-flight CON requests whose timers have expired.
3. Cleans up expired Observe notifications (RST or timeout).
4. Frees expired discover slots.

Pass `millis()` or an equivalent monotonic millisecond counter.

---

## Confirmable vs. Non-confirmable

All request methods (`get`, `put`, `post`, `del`) default to `confirmable = true`. You can opt out for fire-and-forget sends:

```cpp
client.get(server, "/sensors/temp", onTemp, nullptr, /*confirmable=*/false);
```

NON requests are not retransmitted. Use them for high-frequency non-critical data (e.g. telemetry) where the overhead of ACKs outweighs the cost of an occasional dropped packet.

`observe()` always sends a CON registration — the subscription itself must be reliable. Notifications can be NON (the server decides via the `confirmable` parameter in `notify()`).
