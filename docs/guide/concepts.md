# Concepts

## What is CoAP?

CoAP (Constrained Application Protocol, RFC 7252) is a lightweight request/response protocol designed for constrained devices and networks. It uses UDP, so there is no connection setup — a device can send a request in a single datagram and receive a response in another.

The model is deliberately REST-like. Every resource on a device has a path (`/sensors/temp`, `/actuators/led`). Clients issue requests using method codes (GET, PUT, POST, DELETE). Servers return response codes (2.05 Content, 4.04 Not Found, …). If you know HTTP, CoAP feels familiar with a much smaller price tag: a complete request and response can fit in under 30 bytes.

```
HTTP (TCP, heavyweight)           CoAP (UDP, lightweight)
───────────────────────────       ─────────────────────────────────
GET /temp HTTP/1.1                0x40 0x01 0x00 0x01 0xB4 "temp"
Host: 192.168.1.42                  ↑     ↑    ↑    ↑    ↑
Content-Type: text/plain            |     |  msg-id token path
Accept: */*                        CON   GET
...20+ more header lines...
                                  ~10 bytes total
```

## Message types

Every CoAP message is one of four types:

| Type | Abbreviation | Meaning |
|---|---|---|
| **Confirmable** | CON | Reliable — the sender retransmits until it receives an ACK |
| **Non-confirmable** | NON | Fire-and-forget — no ACK required |
| **Acknowledgement** | ACK | Confirms receipt of a CON message |
| **Reset** | RST | Signals that a CON message could not be processed |

PulseCoAP defaults to CON for client requests (retransmitted automatically) and NON for server Observe notifications (configurable — see `notify()`).

## Reliability layer

CON messages are the backbone of CoAP's reliability. When a client sends a CON GET, it starts a retransmission timer. If no ACK arrives within `PULSECOAP_ACK_TIMEOUT_MS` (default 2 s), the message is retransmitted. After `PULSECOAP_MAX_RETRANSMIT` retries (default 4) the request is abandoned and the timeout handler fires.

PulseCoAP tracks all in-flight CON messages in a `TransactionPool`. You create one shared pool and hand it to the `Client`:

```cpp
pulsecoap::TransactionPool transactions;
pulsecoap::Client client(transport, transactions);
```

The pool is shared so that retransmission slots can be reused across multiple concurrent requests.

## Piggybacked vs. separate responses

A server normally replies to a CON request immediately in the same datagram — this is a **piggybacked** response (ACK + response code in one packet). It is what PulseCoAP does by default.

When the handler needs more time (reading a slow sensor, waiting for a mutex), it can set `res.deferred = true`. The server sends an empty ACK immediately to stop client retransmission, then sends the real response as a separate CON message later via `server.respond(handle, ...)`. The client receives the data just the same.

## Observe (RFC 7641)

CoAP Observe turns a resource into a live subscription. The client sends a GET with an Observe option set to 0 (register). The server records the subscriber, responds with the current value, and then sends a new notification every time the value changes — until the client sends a GET with Observe=1 (deregister), or goes silent (the server detects this via RST or a failed CON notification).

```
Client (ESP32 #2)                 Server (ESP32 #1)
─────────────────                 ─────────────────
GET /sensors/temp Obs=0 ──────→   registers observer, replies with 24.1
                         ←──────  24.1  (initial value)
                         ←──────  24.3  (server called notify())
                         ←──────  24.5
GET /sensors/temp Obs=1 ──────→   deregisters observer
```

In PulseCoAP:
- Server: `addResource(..., /*observable=*/true)` then `server.notify(path, payload, len)` to push.
- Client: `client.observe(server, path, callback)` to register; `client.cancelObserve(server, path)` to deregister.

## Resource paths and URI templates

Resource paths follow URI path conventions. PulseCoAP supports two forms:

**Exact paths** — registered and matched verbatim:
```cpp
server.addResource("/sensors/temp", MethodGet, handleTemp);
```

**URI templates** — a `:name` segment matches any single path segment:
```cpp
server.addResource("/sensors/:id", MethodGet, handleSensor);
// Matches /sensors/0, /sensors/temp, /sensors/humidity, ...
// Inside the handler: req.pathParam("id") returns the matched segment.
```

Exact paths always win over templates when both could match.

## Resource discovery (RFC 6690)

A CoAP server describes itself through `/.well-known/core`, which returns a CoRE Link Format string listing all registered resources:

```
</sensors/temp>;obs;rt="temperature",</sensors/humidity>;obs;rt="humidity"
```

PulseCoAP registers `/.well-known/core` automatically at `server.begin()`. On the client side, `client.discover()` sends a multicast GET to the IANA CoAP all-nodes address (`224.0.1.187:5683`) — every PulseCoAP server on the LAN responds unicast with its resource list.

## Transport seam

PulseCoAP never calls a socket API directly. All network I/O goes through a `Transport` interface with three methods:

```cpp
bool begin(uint16_t localPort);
bool send(const Endpoint& to, const uint8_t* data, size_t length);
size_t receive(uint8_t* buffer, size_t capacity, Endpoint& from);
```

This is what lets the same protocol code run on Arduino WiFiUDP, lwIP raw sockets, POSIX UDP, and DTLS without any changes to `Server` or `Client`. See [Transports & Platforms](transports.md) for the adapters PulseCoAP ships.

## Glossary

| Term | Meaning |
|---|---|
| **CON** | Confirmable message — retransmitted until ACK'd |
| **NON** | Non-confirmable message — fire-and-forget |
| **Token** | A 1–8 byte identifier that matches a response to its request |
| **Endpoint** | `{ ip[4], port }` or `{ v6[16], port }` — the remote address |
| **Observe** | RFC 7641 subscription — server pushes updates without the client polling |
| **Deferred response** | Server ACKs a CON request immediately, sends the real response later |
| **CoRE Link Format** | RFC 6690 text format for describing resource paths and attributes |
| **Block-wise** | RFC 7959 — fragments large payloads across multiple messages |
| **DTLS** | RFC 6347 — datagram TLS; CoAP over DTLS is called CoAPs (coaps://) |
| **TransactionPool** | PulseCoAP's fixed-size pool that tracks in-flight CON messages and drives retransmission |
