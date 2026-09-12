// PulseCoAP — TwoDevicePeerToPeer  (SERVER sketch)
//
// Flash this on ESP32-A.
// Flash TwoDevicePeerToPeer_Client on ESP32-B.
// Both must be on the same WiFi network.
//
// ESP32-A exposes:
//   GET  /data/temp      → current fake temperature (observable)
//   GET  /data/humidity  → current fake humidity (observable)
//   PUT  /control/led    → set LED state: payload "on" or "off"
//
// ESP32-B will:
//   1. GET /.well-known/core to discover resources
//   2. Observe /data/temp — prints every push
//   3. PUT /control/led "on" every 10 s and "off" 5 s later
//
// Serial Monitor (115200) shows the ESP32-A IP — enter it in the
// Client sketch before flashing ESP32-B.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";

static const int kLedPin = 2; // built-in LED on most ESP32 dev boards

WiFiUDP                        udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::Server              server(transport);

// ── Dummy data ────────────────────────────────────────────────────────────
static float g_temp = 24.0f;
static int   g_hum  = 61;
static float g_tDir = 0.1f;
static int   g_hDir = 1;

void updateData() {
    g_temp += g_tDir; if (g_temp > 28 || g_temp < 20) g_tDir = -g_tDir;
    g_hum  += g_hDir; if (g_hum  > 75 || g_hum  < 50) g_hDir = -g_hDir;
}

// ── Handlers ──────────────────────────────────────────────────────────────
void handleTemp(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[12];
    snprintf(buf, sizeof(buf), "%.1f", g_temp);
    res.code = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload(buf);
}

void handleHumidity(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d", g_hum);
    res.code = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload(buf);
}

void handleLed(const pulsecoap::Request& req, pulsecoap::Response& res, void*) {
    // Resource registered as MethodPut only — server routing already ensures
    // only PUT reaches here, so no extra method check is needed.
    const uint8_t* p = req.payload();
    size_t         l = req.payloadLength();

    if (l == 2 && memcmp(p, "on", 2) == 0) {
        digitalWrite(kLedPin, HIGH);
        res.code = pulsecoap::Code::Changed;
        res.setPayload("LED on");
        Serial.println("[server] LED → ON");
    } else if (l == 3 && memcmp(p, "off", 3) == 0) {
        digitalWrite(kLedPin, LOW);
        res.code = pulsecoap::Code::Changed;
        res.setPayload("LED off");
        Serial.println("[server] LED → OFF");
    } else {
        res.code = pulsecoap::Code::BadRequest;
        res.setPayload("payload must be 'on' or 'off'");
    }
}

// ── setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    pinMode(kLedPin, OUTPUT);
    delay(300);
    Serial.println("\n=== PulseCoAP P2P Server ===");

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    Serial.printf("\n\n>>> Server IP: %s   (enter this in the Client sketch) <<<\n\n",
                  WiFi.localIP().toString().c_str());

    server.addResource("/data/temp",     pulsecoap::MethodGet,         handleTemp,     nullptr, true);
    server.addResource("/data/humidity", pulsecoap::MethodGet,         handleHumidity, nullptr, true);
    server.addResource("/control/led",   pulsecoap::MethodPut,         handleLed);

    server.setResourceType("/data/temp",     "temperature");
    server.setResourceType("/data/humidity", "humidity");

    server.begin(5683);
    Serial.println("Resources: /data/temp  /data/humidity  /control/led");
    Serial.println("Ready.\n");
}

// ── loop ──────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    server.poll(now);

    static uint32_t lastNotify = 0;
    if (now - lastNotify >= 3000) {
        lastNotify = now;
        updateData();

        char t[12], h[8];
        snprintf(t, sizeof(t), "%.1f", g_temp);
        snprintf(h, sizeof(h), "%d", g_hum);

        server.notify("/data/temp",     (const uint8_t*)t, strlen(t),
                      pulsecoap::ContentFormat::TextPlain);
        server.notify("/data/humidity", (const uint8_t*)h, strlen(h),
                      pulsecoap::ContentFormat::TextPlain);
    }
}
