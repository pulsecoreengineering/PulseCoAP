// DtlsServer — PulseCoAP DTLS 1.2 server example (ESP32)
//
// Exposes one resource:
//   GET /temp  → returns a fake temperature reading (2.05 Content)
//
// Listens on CoAP-DTLS standard port 5684.
// Only clients that present the correct PSK identity + secret are accepted.
//
// Wiring: just an ESP32 with WiFi.  No extra hardware needed for this demo.
//
// Companion sketch: DtlsClient — runs on a second ESP32, GETs /temp.
//
// ── CONFIGURATION ──────────────────────────────────────────────────────────

// Enable DTLS — must come before #include <PulseCoAP.h>
#define PULSECOAP_ENABLE_DTLS 1

#include <WiFi.h>
#include <WiFiUDP.h>
#include <PulseCoAP.h>

// ── WiFi credentials ───────────────────────────────────────────────────────
static const char* WIFI_SSID = "YourSSID";
static const char* WIFI_PASS = "YourPassword";

// ── Shared PSK ─────────────────────────────────────────────────────────────
// Must match exactly on server and client.
static const char* PSK_IDENTITY = "sensor-node-01";
static const char* PSK_SECRET   = "pulsecoap-secret-key-2026"; // ≤ 32 chars

// ── Objects ────────────────────────────────────────────────────────────────
static WiFiUDP              wifiUdp;
static pulsecoap::DtlsTransport dtlsTransport(wifiUdp, /*server=*/true);
static pulsecoap::Server    server(dtlsTransport);

// ── Resource handler ───────────────────────────────────────────────────────
void handleTemp(const pulsecoap::Request& req,
                pulsecoap::Response& res,
                void* /*ctx*/) {
    // Simulate a temperature reading.
    float temp = 22.5f + (float)(millis() % 30) * 0.1f;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f C", temp);

    res.setCode(pulsecoap::Code::Content);
    res.setContentFormat(pulsecoap::ContentFormat::TextPlain);
    res.setPayload(reinterpret_cast<const uint8_t*>(buf), strlen(buf));
}

// ── setup / loop ───────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[DtlsServer] Booting…");

    // Connect to WiFi.
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[DtlsServer] Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\n[DtlsServer] IP: %s\n", WiFi.localIP().toString().c_str());

    // Register PSK (one entry per authorised client; add more with addPsk()).
    if (!dtlsTransport.addPsk(PSK_IDENTITY, PSK_SECRET)) {
        Serial.println("[DtlsServer] ERROR: PSK store full");
        while (true) delay(1000);
    }

    // Bind to CoAP-DTLS port.
    if (!dtlsTransport.begin(5684)) {
        Serial.println("[DtlsServer] ERROR: begin() failed");
        while (true) delay(1000);
    }

    // Register resources.
    server.addResource("/temp", pulsecoap::MethodGet, handleTemp, nullptr);

    Serial.println("[DtlsServer] Ready — listening on UDP :5684 (DTLS)");
    Serial.printf("[DtlsServer] PSK identity : %s\n", PSK_IDENTITY);
    Serial.printf("[DtlsServer] GET /temp    : fake temperature\n");
}

void loop() {
    server.loop();
}
