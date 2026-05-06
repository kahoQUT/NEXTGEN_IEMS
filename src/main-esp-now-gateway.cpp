#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SharedPayload.h" // Must be identical to Substation

// --- Network & MQTT Config ---
const bool ENABLE_MQTT = false;

const char* ssid = "TP-Link_0E25";
const char* password = "18933512";
const char* mqtt_server = "192.168.1.100"; // Mosquitto IP or Domain
const int mqtt_port = 1883;
const char* mqtt_topic = "ems/zoneA/meters";

// --- LoRa & OLED Pins ---
#define SCK 5
#define MISO 19
#define MOSI 27
#define SS 18
#define RST 14
#define DIO0 26
#define OLED_SDA 21
#define OLED_SCL 22
#define LORA_BAND 915E6

Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClient espClient;
PubSubClient client(espClient);

// --- Display Helper ---
void updateScreen(String title, String line1, String line2) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("=== " + title + " ===");
    display.println(line1);
    display.println(line2);
    display.display();
}

// --- Network Setup ---
void setup_wifi() {
    delay(10);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WiFi] Connected!");
}

void reconnect_mqtt() {
    while (!client.connected()) {
        String clientId = "LilyGo-Gateway-" + String(random(0xffff), HEX);
        // Add username/password here if Mosquitto has ACL configured:
        // if (client.connect(clientId.c_str(), "gateway_user", "password")) {
        if (client.connect(clientId.c_str())) {
            Serial.println("[MQTT] Connected");
            updateScreen("GATEWAY", "MQTT Connected", "Waiting for LoRa...");
        } else {
            Serial.print("[MQTT] Failed, rc=");
            Serial.print(client.state());
            Serial.println(" retry in 5s");
            delay(5000);
        }
    }
}
//onLoRaReceive
void onLoRaReceive(int packetSize) {
    if (packetSize <= 0) {
        return;
    }

    if (packetSize != sizeof(Payload)) {
        Serial.printf("[Error] LoRa packet size mismatch. Expected: %d, Got: %d\n",
                      sizeof(Payload), packetSize);
        updateScreen("RX ERROR", "Size Mismatch", "Got " + String(packetSize) + " bytes");
        return;
    }

    Payload payload;
    LoRa.readBytes((uint8_t*)&payload, sizeof(payload));

    Serial.printf("[LoRa] Rx | UID: %u | SEQ: %u | RSSI: %d | SNR: %.1f\n",
                  payload.uid, payload.seq, LoRa.packetRssi(), LoRa.packetSnr());

    updateScreen("LORA RX OK", "UID: " + String(payload.uid),
                 "RSSI: " + String(LoRa.packetRssi()));

    if (ENABLE_MQTT) {
        bool pubSuccess = client.publish(mqtt_topic, (const uint8_t*)&payload, sizeof(payload));
        if (pubSuccess) {
            Serial.println("[MQTT] Binary Payload Published!");
            updateScreen("DATA FWD", "UID: " + String(payload.uid), "Pub: SUCCESS");
        } else {
            Serial.println("[MQTT] Publish Failed");
            updateScreen("DATA FWD", "UID: " + String(payload.uid), "Pub: FAILED");
        }
    } else {
        Serial.println("[MQTT] Skipped (Offline Mode)");
    }

    LoRa.receive(); // re-enter receive mode
}
// --- Initialization ---
void setup() {
    Serial.begin(115200);
    delay(5000);

    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(WHITE);
    updateScreen("GATEWAY", "Booting...", "");

    if (ENABLE_MQTT) {
        setup_wifi();
        client.setServer(mqtt_server, mqtt_port);
    } else {
        Serial.println("[System] MQTT & WiFi is DISABLED. LoRa Rx Test Mode Only");
    }

    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, DIO0);
    if (!LoRa.begin(LORA_BAND)) {
        updateScreen("ERROR", "LoRa Init Failed", "");
        while (1);
    }
    LoRa.setSpreadingFactor(10);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setSyncWord(0xF3);
    LoRa.enableCrc();

    LoRa.onReceive(onLoRaReceive);
    LoRa.receive();

    if (!ENABLE_MQTT) {
        updateScreen("TEST MODE", "LoRa Rx Only", "MQTT Offline");
        Serial.println("[System] Gateway Ready. Waiting for LoRa...");
    }
}

// --- Main Loop (Polling LoRa & MQTT) ---
void loop() {
    if (ENABLE_MQTT) {
        if (!client.connected()) {
            reconnect_mqtt();
        }
        client.loop(); 
    }
}