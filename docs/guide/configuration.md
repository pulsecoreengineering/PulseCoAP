# Configuration

All configuration is done with `#define` before `#include <PulseCoAP.h>`, or via `-D` compiler flags. Invalid values fail at **compile time** with a clear message.

## Message sizing

### `PULSECOAP_MAX_MSG_SIZE`

**Default:** `256` bytes

Maximum size of a single CoAP message (header + token + options + payload) that PulseCoAP will encode or decode. RFC 7252 recommends keeping messages within a single IP datagram to avoid fragmentation; 256 bytes is a safe default for constrained links.

Raise this if:
- Your payloads are larger than ~200 bytes and you are not using block-wise transfer.
- You are running on a POSIX host with plenty of RAM.

```cpp
#define PULSECOAP_MAX_MSG_SIZE 512
#include <PulseCoAP.h>
```

Memory cost: one `rxBuffer_[PULSECOAP_MAX_MSG_SIZE]` in both `Server` and `Client`.

### `PULSECOAP_MAX_OPTIONS`

**Default:** `16`

Maximum number of CoAP options PulseCoAP will index per message. 16 comfortably covers `Uri-Path` (multiple segments), `Uri-Query`, `Content-Format`, `Observe`, `ETag`, and `Block1`/`Block2` in one message.

### `PULSECOAP_MAX_TOKEN_LEN`

**Default:** `8`

Fixed by RFC 7252 (tokens are 0–8 bytes). Exposed for `static_assert` use; not normally changed.

---

## Reliability (retransmission)

### `PULSECOAP_MAX_TRANSACTIONS`

**Default:** `4`

How many Confirmable messages can be in flight at once (client- or server-side). Each slot costs approximately `PULSECOAP_MAX_MSG_SIZE + 20` bytes.

Raise if your application sends many concurrent CON requests. Lower on tight AVR targets.

### `PULSECOAP_ACK_TIMEOUT_MS`

**Default:** `2000` ms

RFC 7252 §4.8 base retransmit interval. The actual first timeout is randomised within `[ACK_TIMEOUT, ACK_TIMEOUT × ACK_RANDOM_FACTOR]` and then doubled on each retry.

### `PULSECOAP_ACK_RANDOM_FACTOR_NUM` / `PULSECOAP_ACK_RANDOM_FACTOR_DEN`

**Default:** `3 / 2` (= 1.5)

ACK_RANDOM_FACTOR expressed as a fraction to avoid linking `libm` on platforms that prefer not to. Default matches RFC 7252.

### `PULSECOAP_MAX_RETRANSMIT`

**Default:** `4`

RFC 7252 default MAX_RETRANSMIT. After this many retries with no ACK, the transaction is abandoned and the `TimeoutHandler` fires.

---

## Server sizing

### `PULSECOAP_MAX_RESOURCES`

**Default:** `8`

Maximum resources registered with `addResource()`. Each slot holds a path pointer, method mask, handler pointer, and a few flags (~16–24 bytes).

### `PULSECOAP_MAX_OBSERVERS`

**Default:** `8`

Maximum simultaneous Observe subscriptions across all observable resources. Each slot holds an `Endpoint`, a token (up to 8 bytes), and a resource index (~28 bytes).

### `PULSECOAP_MAX_URI_PATH_LEN`

**Default:** `64` bytes

Maximum byte length of a resource path including the NUL terminator. Paths longer than this are silently truncated in `PendingRequest`.

### `PULSECOAP_MAX_DEFERRED`

**Default:** `2`

Maximum simultaneous deferred (separate) responses. Raise if multiple handlers can have slow work in flight at once. Each slot costs `~(4 + PULSECOAP_MAX_TOKEN_LEN)` bytes.

### `PULSECOAP_MAX_LINK_FORMAT_LEN`

**Default:** `256` bytes

Maximum byte length of the CoRE Link Format body served at `/.well-known/core`. One resource entry takes roughly `len(path) + 10` bytes. Raise if you register many resources with long paths or `rt=` values.

---

## URI template parameters

### `PULSECOAP_MAX_PATH_PARAMS`

**Default:** `4`

Maximum number of `:name` segments in a URI template. For example, `/devices/:dev/ch/:ch` has 2 params, so 4 is generous.

### `PULSECOAP_MAX_PATH_PARAM_LEN`

**Default:** `16` bytes (including NUL)

Maximum byte length of a single parameter name or value. `"temperature"` = 11 chars; `"sensor-node-17"` = 14 chars — 16 is tight but sufficient for typical embedded paths. Raise if you have longer segment strings.

---

## Role selection

By default both `Server` and `Client` are compiled in. Define **one** of these to strip the other half out of the build:

```cpp
// Drop the server — client only:
#define PULSECOAP_ROLE_CLIENT_ONLY
#include <PulseCoAP.h>

// Drop the client — server only:
#define PULSECOAP_ROLE_SERVER_ONLY
#include <PulseCoAP.h>
```

Defining both at once is a compile-time error.

---

## Optional modules

### Block-wise transfer (RFC 7959)

**Default:** `PULSECOAP_ENABLE_BLOCKWISE 0` (disabled)

Block1/Block2 allows payloads larger than `PULSECOAP_MAX_MSG_SIZE` by splitting them across multiple messages. Off by default because most constrained-device payloads fit in one datagram and the reassembly state adds RAM.

```cpp
#define PULSECOAP_ENABLE_BLOCKWISE 1
#include <PulseCoAP.h>
```

Related macros when block-wise is enabled:

| Macro | Default | Meaning |
|---|---|---|
| `PULSECOAP_BLOCK_SZX` | `4` | Block size exponent; actual size = 2^(SZX+4). SZX=4 → 256 B |
| `PULSECOAP_MAX_BLOCK1_SESSIONS` | `2` | Simultaneous server-side Block1 (upload) reassembly sessions |
| `PULSECOAP_BLOCK1_MAX_BODY` | `1024` bytes | Capacity of each Block1 reassembly buffer |
| `PULSECOAP_BLOCK2_MAX_BODY` | `1024` bytes | Client-side Block2 receive buffer per pending request |

Ensure `block_size + ~20-byte message overhead ≤ PULSECOAP_MAX_MSG_SIZE`.

### DTLS (RFC 6347)

**Default:** `PULSECOAP_ENABLE_DTLS 0` (disabled)

CoAP over DTLS (CoAPs, `coaps://`). Requires mbedTLS. PulseCoAP's own message/routing code stays zero-heap; only the mbedTLS handshake uses heap internally.

```cpp
#define PULSECOAP_ENABLE_DTLS 1
#include <PulseCoAP.h>
```

Related macros:

| Macro | Default | Meaning |
|---|---|---|
| `PULSECOAP_DTLS_MAX_SESSIONS` | `4` | Simultaneous DTLS sessions |
| `PULSECOAP_DTLS_MAX_PSK_ENTRIES` | `8` | PSK identity/key pairs in the static key store |
| `PULSECOAP_DTLS_IDENTITY_MAX_LEN` | `32` bytes | Max length of a PSK identity string |
| `PULSECOAP_DTLS_PSK_MAX_LEN` | `32` bytes | Max length of a PSK secret |
| `PULSECOAP_DTLS_HANDSHAKE_TIMEOUT_MS` | `10000` ms | Max time before a handshake is abandoned |

See [Transports & Platforms](transports.md) for how to wire up `DtlsTransport`.

### PulseTrace

**Default:** `PULSECOAP_ENABLE_TRACE 0` (disabled)

Hook for retransmit / timeout / drop / duplicate events. Enable to integrate with the PulseTrace diagnostic library. Off by default so PulseCoAP has zero Pulse-ecosystem dependencies unless you opt in.

---

## Multicast discovery

### `PULSECOAP_MAX_DISCOVERS`

**Default:** `2`

Maximum simultaneous active `discover()` operations (slots freed automatically after `PULSECOAP_DISCOVER_TIMEOUT_MS`).

### `PULSECOAP_DISCOVER_TIMEOUT_MS`

**Default:** `5000` ms

How long a `discover()` slot stays open collecting responses. RFC 7252 §8.2 says servers should randomise their reply within `[0, ACK_TIMEOUT]` to avoid floods — 5 000 ms catches the slowest responders.

---

## Overriding per-sketch

Because all macros are `#ifndef`-guarded, defining them before the include wins:

```cpp
// At the top of your .ino or in a config header included first:
#define PULSECOAP_MAX_MSG_SIZE      512
#define PULSECOAP_MAX_RESOURCES      16
#define PULSECOAP_MAX_OBSERVERS      16
#define PULSECOAP_MAX_TRANSACTIONS    8
#define PULSECOAP_ROLE_SERVER_ONLY
#include <PulseCoAP.h>
```
