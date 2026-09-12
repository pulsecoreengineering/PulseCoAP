// PulseCoAP — MultiObserve
//
// Demonstrates multiple simultaneous Observe subscriptions (RFC 7641).
//
// A single Client instance subscribes to THREE different resources on the
// same server, each with its own callback. Updates for /temp, /humidity,
// and /uptime arrive independently — cancelling one does not affect the
// others.
//
// This sketch runs server + client on one ESP32 (loopback over WiFi).
// To split across two boards: move the Server block to a separate sketch
// and replace loopbackEp with the real server's IP.
//
// What you will see on Serial (every 3 seconds):
//   [temp]     24.3 °C
//   [humidity] 61 %
//   [uptime]   12 s
//   [temp]     24.4 °C
//   ...
// After 30 seconds the temp subscription is cancelled; only humidity
// and uptime keep arriving.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <PulseCoAP.h>
#include <PulseCoAPTransportArduinoUDP.h>

const char* kSsid     = "YOUR_WIFI_SSID";
const char* kPassword = "YOUR_WIFI_PASSWORD";

// ── CoAP stack ────────────────────────────────────────────────────────────
WiFiUDP                        serverUdp;
pulsecoap::ArduinoUdpTransport serverTransport(serverUdp);
pulsecoap::Server              server(serverTransport);

WiFiUDP                        clientUdp;
pulsecoap::ArduinoUdpTransport clientTransport(clientUdp);
pulsecoap::TransactionPool     txPool;
pulsecoap::Client              client(clientTransport, txPool);

pulsecoap::Endpoint loopbackEp;
bool tempCancelled = false;

// ── Dummy sensor data ─────────────────────────────────────────────────────
static float g_temp = 24.0f;
static int   g_hum  = 61;
static float g_tempDir = 0.1f;
static int   g_humDir  = 1;

void updateData() {
    g_temp += g_tempDir;
    if (g_temp > 28.0f || g_temp < 20.0f) g_tempDir = -g_tempDir;
    g_hum += g_humDir;
    if (g_hum > 75 || g_hum < 50) g_humDir = -g_humDir;
}

// ── Server handlers ───────────────────────────────────────────────────────
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

void handleUptime(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000UL);
    res.code = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload(buf);
}

// ── Client observe callbacks — one per resource ───────────────────────────
void onTemp(const pulsecoap::ClientResponse& res, void*) {
    char buf[res.payloadLength + 1];
    memcpy(buf, res.payload, res.payloadLength); buf[res.payloadLength] = '\0';
    Serial.printf("[temp]     %s °C\n", buf);
}

void onHumidity(const pulsecoap::ClientResponse& res, void*) {
    char buf[res.payloadLength + 1];
    memcpy(buf, res.payload, res.payloadLength); buf[res.payloadLength] = '\0';
    Serial.printf("[humidity] %s %%\n", buf);
}

void onUptime(const pulsecoap::ClientResponse& res, void*) {
    char buf[res.payloadLength + 1];
    memcpy(buf, res.payload, res.payloadLength); buf[res.payloadLength] = '\0';
    Serial.printf("[uptime]   %s s\n", buf);
}

// ── setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== PulseCoAP MultiObserve ===");

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    auto ip = WiFi.localIP();
    Serial.printf("IP: %s\n", ip.toString().c_str());

    // Server
    server.addResource("/temp",     pulsecoap::MethodGet, handleTemp,     nullptr, true);
    server.addResource("/humidity", pulsecoap::MethodGet, handleHumidity, nullptr, true);
    server.addResource("/uptime",   pulsecoap::MethodGet, handleUptime,   nullptr, true);
    server.begin(5683);

    // Client (loopback)
    loopbackEp.ip[0]=ip[0]; loopbackEp.ip[1]=ip[1];
    loopbackEp.ip[2]=ip[2]; loopbackEp.ip[3]=ip[3];
    loopbackEp.port = 5683;

    clientUdp.begin(5684);
    client.begin();
    delay(200);

    // Subscribe to all three independently
    client.observe(loopbackEp, "/temp",     onTemp,     nullptr);
    client.observe(loopbackEp, "/humidity", onHumidity, nullptr);
    client.observe(loopbackEp, "/uptime",   onUptime,   nullptr);

    Serial.println("Subscribed to /temp, /humidity, /uptime");
    Serial.println("(temp subscription will cancel after 30 s)\n");
}

// ── loop ──────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    server.poll(now);
    client.poll(now);

    // Push updates every 3 seconds
    static uint32_t lastNotify = 0;
    if (now - lastNotify >= 3000) {
        lastNotify = now;
        updateData();

        char t[12], h[8], u[16];
        snprintf(t, sizeof(t), "%.1f", g_temp);
        snprintf(h, sizeof(h), "%d",   g_hum);
        snprintf(u, sizeof(u), "%lu",  now / 1000UL);

        server.notify("/temp",     (const uint8_t*)t, strlen(t),
                      pulsecoap::ContentFormat::TextPlain);
        server.notify("/humidity", (const uint8_t*)h, strlen(h),
                      pulsecoap::ContentFormat::TextPlain);
        server.notify("/uptime",   (const uint8_t*)u, strlen(u),
                      pulsecoap::ContentFormat::TextPlain);
    }

    // Cancel the /temp subscription after 30 seconds
    if (!tempCancelled && now >= 30000) {
        tempCancelled = true;
        client.cancelObserve(loopbackEp, "/temp");
        Serial.println("\n[client] /temp subscription cancelled.");
        Serial.println("[client] Only /humidity and /uptime will keep arriving.\n");
    }
}
