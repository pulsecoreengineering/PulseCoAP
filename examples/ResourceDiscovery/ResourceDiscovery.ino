// PulseCoAP — ResourceDiscovery
//
// Demonstrates RFC 6690 resource discovery via /.well-known/core.
//
// A CoAP server auto-registers /.well-known/core at begin(). A client
// queries it to learn what resources the server exposes — their paths,
// whether they are observable, and their resource types — before sending
// any data requests. This is the CoAP equivalent of HTTP's OPTIONS or
// a Swagger spec: the server describes itself.
//
// The /.well-known/core response uses CoRE Link Format (Content-Format 40):
//   </temp>;obs;rt="temperature",</humidity>;obs;rt="humidity",</uptime>
//
// After discovering the resources, the client GETs each one.
//
// This sketch runs server + client on one ESP32 (loopback over WiFi).
//
// Test from PC:
//   coap-client -m get coap://<IP>/.well-known/core
//   # You will see the CoRE Link Format string printed to the terminal.

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

// ── Server handlers ───────────────────────────────────────────────────────
void handleTemp(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "%.1f", 24.3f);
    res.code = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload(buf);
}

void handleHumidity(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    res.code = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload("61");
}

void handleUptime(const pulsecoap::Request&, pulsecoap::Response& res, void*) {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%lu", millis() / 1000UL);
    res.code = pulsecoap::Code::Content;
    res.contentFormat = pulsecoap::ContentFormat::TextPlain;
    res.setPayload(buf);
}

// ── Client callbacks ──────────────────────────────────────────────────────
void onDiscovery(const pulsecoap::ClientResponse& res, void*) {
    Serial.println("\n[discovery] /.well-known/core response:");
    Serial.println("─────────────────────────────────────────");
    Serial.write(res.payload, res.payloadLength);
    Serial.println("\n─────────────────────────────────────────");
    Serial.println("Now querying each discovered resource...\n");

    // After discovering, fetch each resource
    // (In a real app you would parse the CoRE Link Format string here)
    client.get(loopbackEp, "/temp",     [](const pulsecoap::ClientResponse& r, void*) {
        char buf[r.payloadLength + 1];
        memcpy(buf, r.payload, r.payloadLength); buf[r.payloadLength] = '\0';
        Serial.printf("[/temp]     %s\n", buf);
    }, nullptr);

    client.get(loopbackEp, "/humidity", [](const pulsecoap::ClientResponse& r, void*) {
        char buf[r.payloadLength + 1];
        memcpy(buf, r.payload, r.payloadLength); buf[r.payloadLength] = '\0';
        Serial.printf("[/humidity] %s\n", buf);
    }, nullptr);

    client.get(loopbackEp, "/uptime",   [](const pulsecoap::ClientResponse& r, void*) {
        char buf[r.payloadLength + 1];
        memcpy(buf, r.payload, r.payloadLength); buf[r.payloadLength] = '\0';
        Serial.printf("[/uptime]   %s s\n", buf);
    }, nullptr);
}

// ── setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== PulseCoAP ResourceDiscovery ===");

    WiFi.begin(kSsid, kPassword);
    while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
    auto ip = WiFi.localIP();
    Serial.printf("IP: %s\n\n", ip.toString().c_str());

    // Register resources with rt= (resource type) for richer discovery output
    server.addResource("/temp",     pulsecoap::MethodGet, handleTemp,     nullptr, true);
    server.addResource("/humidity", pulsecoap::MethodGet, handleHumidity, nullptr, true);
    server.addResource("/uptime",   pulsecoap::MethodGet, handleUptime,   nullptr, false);

    server.setResourceType("/temp",     "temperature");
    server.setResourceType("/humidity", "humidity");
    // /uptime intentionally has no rt= to show the difference in the output

    server.begin(5683);
    Serial.println("Server resources registered:");
    Serial.println("  /temp      (observable, rt=temperature)");
    Serial.println("  /humidity  (observable, rt=humidity)");
    Serial.println("  /uptime    (not observable)");
    Serial.println("  /.well-known/core  (auto-registered)");

    // Client: discover the server before talking to it
    loopbackEp.ip[0]=ip[0]; loopbackEp.ip[1]=ip[1];
    loopbackEp.ip[2]=ip[2]; loopbackEp.ip[3]=ip[3];
    loopbackEp.port = 5683;

    clientUdp.begin(5684);
    client.begin();
    delay(200);

    Serial.println("\nClient: GET /.well-known/core ...");
    client.get(loopbackEp, "/.well-known/core", onDiscovery);
}

// ── loop ──────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();
    server.poll(now);
    client.poll(now);
}
