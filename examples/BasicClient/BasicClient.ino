// PulseCoAP BasicClient
//
// GETs /sensors/temp from another PulseCoAP (or any RFC 7252) server every
// 5 seconds and prints the result. Set kServerIp to that device's address.
#include <PulseCoAP.h>
#include <WiFi.h>
#include <WiFiUdp.h>

const char* kWifiSsid = "YOUR_WIFI_SSID";
const char* kWifiPassword = "YOUR_WIFI_PASSWORD";

// The CoAP server's address, e.g. the ESP32 running BasicServer.ino.
pulsecoap::Endpoint kServer = {{192, 168, 1, 42}, 5683};

WiFiUDP udp;
pulsecoap::ArduinoUdpTransport transport(udp);
pulsecoap::TransactionPool transactions;
pulsecoap::Client client(transport, transactions);

unsigned long lastRequestAt = 0;

void onTempResponse(const pulsecoap::ClientResponse& res, void* /*ctx*/) {
    Serial.print("Temp response (code 0x");
    Serial.print(static_cast<uint8_t>(res.code), HEX);
    Serial.print("): ");
    Serial.write(res.payload, res.payloadLength);
    Serial.println();
}

void onRequestTimeout(void* /*ctx*/) {
    Serial.println("Request timed out (no response after all retries)");
}

void setup() {
    Serial.begin(115200);
    WiFi.begin(kWifiSsid, kWifiPassword);
    while (WiFi.status() != WL_CONNECTED) {
        delay(250);
        Serial.print(".");
    }
    Serial.print("\nWiFi connected, IP: ");
    Serial.println(WiFi.localIP());

    client.begin();
    client.setTimeoutHandler(onRequestTimeout);
}

void loop() {
    client.poll(millis());

    if (millis() - lastRequestAt > 5000) {
        lastRequestAt = millis();
        client.get(kServer, "/sensors/temp", onTempResponse);
    }
}
