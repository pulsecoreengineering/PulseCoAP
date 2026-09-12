// DtlsClient — PulseCoAP DTLS 1.2 client example (ESP32)
//
// GETs /temp from the DtlsServer example over DTLS 1.2 + PSK every 5 s.
// Prints the plaintext response to Serial.
//
// How the non-blocking handshake works in practice
// -------------------------------------------------
// 1. The first client.get() call allocates a DTLS session and starts the
//    handshake.  It returns an ID immediately without blocking loop().
// 2. loop() calls client.loop() on every iteration.  client.loop() calls
//    dtlsTransport.receive() which drives the handshake via mbedTLS each
//    time a packet arrives from the server.
// 3. CoAP's CON retransmit engine retries the GET after 2 s if no ACK
//    arrives — which is what drives additional handshake steps.
// 4. Once the handshake finishes (typically 1–2 round trips, < 200 ms on
//    a normal LAN), the pending GET goes through mbedtls_ssl_write and
//    the server responds with 2.05 Content.
//
// ── CONFIGURATION ──────────────────────────────────────────────────────────

#define PULSECOAP_ENABLE_DTLS 1

#include <WiFi.h>
#include <WiFiUDP.h>
#include <PulseCoAP.h>

// ── WiFi credentials ───────────────────────────────────────────────────────
static const char* WIFI_SSID = "YourSSID";
static const char* WIFI_PASS = "YourPassword";

// ── Server address ─────────────────────────────────────────────────────────
// Set this to the IP printed by the DtlsServer sketch.
static const char* SERVER_IP = "192.168.1.100";
static const uint16_t SERVER_PORT = 5684;

// ── Shared PSK (must match DtlsServer exactly) ─────────────────────────────
static const char* PSK_IDENTITY = "sensor-node-01";
static const char* PSK_SECRET   = "pulsecoap-secret-key-2026";

// ── Objects ────────────────────────────────────────────────────────────────
static WiFiUDP              wifiUdp;
static pulsecoap::DtlsTransport dtlsTransport(wifiUdp, /*server=*/false);
static pulsecoap::Client    client(dtlsTransport);

// ── Response callback ──────────────────────────────────────────────────────
void onTempResponse(const pulsecoap::Response& res, void* /*ctx*/) {
    if (res.code() == pulsecoap::Code::Content) {
        // Null-terminate the payload for printing.
        char buf[64] = {};
        size_t n = res.payloadLen() < sizeof(buf) - 1 ? res.payloadLen() : sizeof(buf) - 1;
        memcpy(buf, res.payload(), n);
        Serial.printf("[DtlsClient] Temperature: %s\n", buf);
    } else {
        Serial.printf("[DtlsClient] Error response: 0x%02X\n",
                      static_cast<uint8_t>(res.code()));
    }
}

// ── Helpers ────────────────────────────────────────────────────────────────
static pulsecoap::Endpoint makeServerEndpoint() {
    pulsecoap::Endpoint ep;
    // Parse dotted-decimal IP into ep.ip[].
    IPAddress addr;
    addr.fromString(SERVER_IP);
    ep.ip[0] = addr[0]; ep.ip[1] = addr[1];
    ep.ip[2] = addr[2]; ep.ip[3] = addr[3];
    ep.port  = SERVER_PORT;
    return ep;
}

// ── setup / loop ───────────────────────────────────────────────────────────
static uint32_t lastGetMs   = 0;
static bool     initialized = false;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[DtlsClient] Booting…");

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[DtlsClient] Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\n[DtlsClient] IP: %s\n", WiFi.localIP().toString().c_str());

    // Register PSK — must match the server.
    if (!dtlsTransport.addPsk(PSK_IDENTITY, PSK_SECRET)) {
        Serial.println("[DtlsClient] ERROR: addPsk failed");
        while (true) delay(1000);
    }

    // Bind to an ephemeral local port.
    if (!dtlsTransport.begin(5683)) {
        Serial.println("[DtlsClient] ERROR: begin() failed");
        while (true) delay(1000);
    }

    Serial.printf("[DtlsClient] Will GET coaps://%s:%u/temp every 5 s\n",
                  SERVER_IP, SERVER_PORT);
    initialized = true;
}

void loop() {
    if (!initialized) return;

    client.loop();

    uint32_t now = millis();
    if (now - lastGetMs >= 5000) {
        lastGetMs = now;
        pulsecoap::Endpoint server = makeServerEndpoint();
        int id = client.get(server, "/temp", onTempResponse, nullptr,
                            pulsecoap::MsgType::Con);
        if (id >= 0)
            Serial.printf("[DtlsClient] GET /temp sent (id=%d)\n", id);
        else
            Serial.println("[DtlsClient] GET /temp failed (DTLS handshake in progress or table full — will retry)");
    }
}
