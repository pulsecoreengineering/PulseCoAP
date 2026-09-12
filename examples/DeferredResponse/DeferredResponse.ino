// PulseCoAP — DeferredResponse
//
// Demonstrates separate (non-piggybacked) responses (RFC 7252 §5.2.2).
//
// Problem: a resource handler sometimes needs to do slow work — read an
// I2C sensor, query flash, wait for a lock — before it can reply. If it
// blocks, the server stops handling other requests and the client will
// retransmit the CON packet repeatedly.
//
// Solution: set res.deferred = true and save req.deferHandle. The server
// immediately sends an empty ACK to stop client retransmission, then you
// call server.respond(handle, ...) whenever the work is done — even many
// loop() iterations later.
//
// This sketch simulates a "slow sensor" that takes 2 seconds to produce
// a reading. The client (coap-cli or a second ESP32) sends a GET; the
// server ACKs instantly, waits 2 s, then sends the real response.
//
// Test from PC (libcoap):
//   coap-client -m get coap://<IP>/sensors/slow
//   # You will see a ~2 second pause before the response arrives —
//   # that is the deferred handler doing its fake slow work.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";

WiFiUDP                        udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server              server(transport);

// ── Deferred request tracker ─────────────────────────────────────────────
// In a real application you might have several deferred requests at once.
// Here we track one for simplicity.
struct PendingWork {
    bool          active     = false;
    pulsecoap::DeferHandle handle = pulsecoap::kInvalidDeferHandle;
    uint32_t      startedMs  = 0;
    static constexpr uint32_t kWorkDurationMs = 2000; // simulate 2 s of work
};
static PendingWork pending;

// ── Slow sensor simulation ────────────────────────────────────────────────
float readSlowSensor() {
    // Pretend this took 2 seconds (already waited in loop())
    return 22.5f + (float)(millis() % 500) / 100.0f;
}

// ── Resource handler ──────────────────────────────────────────────────────
void handleSlowSensor(const pulsecoap::Request& req, pulsecoap::Response& res, void*) {
    if (pending.active) {
        // Already processing a request — tell the client to try again later
        res.code = pulsecoap::Code::ServiceUnavailable;
        Serial.println("[handler] Busy — rejecting second request");
        return;
    }

    // Defer: server sends empty ACK immediately; we store the handle
    res.deferred = true;
    pending.active    = true;
    pending.handle    = req.deferHandle;
    pending.startedMs = millis();
    Serial.printf("[handler] Deferred — ACK sent, starting slow work (handle=%u)\n",
                  pending.handle);
}

// ── setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== PulseCoAP DeferredResponse ===");

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.println("Test: coap-client -m get coap://<IP>/sensors/slow");

    server.addResource("/sensors/slow", pulsecoap::MethodGet, handleSlowSensor);
    server.begin(5683);
    Serial.println("Ready.\n");
}

// ── loop ──────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    server.poll(now);

    // Check if the slow work is done
    if (pending.active &&
        (now - pending.startedMs >= PendingWork::kWorkDurationMs)) {

        float value = readSlowSensor();
        char buf[16];
        snprintf(buf, sizeof(buf), "%.2f", value);

        Serial.printf("[loop] Work done — sending deferred response: %s\n", buf);
        server.respond(pending.handle,
                       pulsecoap::Code::Content,
                       buf,
                       pulsecoap::ContentFormat::TextPlain);

        pending.active = false;
        pending.handle = pulsecoap::kInvalidDeferHandle;
    }
}
