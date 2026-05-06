#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SharedPayload.h" 
// config
#include "secrets.h"

// --- Network & MQTT Config ---
const bool ENABLE_MQTT = true;

const char* ssid = SECRET_WIFI_SSID;
const char* password = SECRET_WIFI_PASS;
const char* mqtt_server = SECRET_MQTT_SERVER; 
const int mqtt_port = SECRET_MQTT_PORT;

const char* mqtt_topic = SECRET_MQTT_TOPIC;

// --- LoRa & OLED Pins ---
#define SCK 5
#define MISO 19
#define MOSI 27
#define SS 18
#define RST 14
#define DIO0 26
#define OLED_SDA 21
#define OLED_SCL 22
#define LORA_BAND SECRET_LORA_BAND 

Adafruit_SSD1306 display(128, 64, &Wire, -1);
WiFiClient espClient;
PubSubClient client(espClient);

// --- Interrupt Safe Variables (ISR) ---
volatile bool newLoRaPacket = false;
volatile bool sizeMismatchError = false; 
volatile int errorPacketSize = 0;        

Payload globalPayload;
volatile int globalRssi = 0;
volatile float globalSnr = 0.0;
volatile int globalPacketSize = 0;

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
        if (client.connect(clientId.c_str())) {
            Serial.println("[MQTT] Connected to Broker");
            updateScreen("GATEWAY", "MQTT Connected", "Waiting for LoRa...");
        } else {
            Serial.print("[MQTT] Connection Failed, rc=");
            Serial.print(client.state());
            Serial.println(" retrying in 5 seconds...");
            delay(5000);
        }
    }
}

// --- LoRa Receive Interrupt ---
void onLoRaReceive(int packetSize) {
    if (packetSize == 0) return;
    
    if (packetSize == sizeof(Payload)) {
        // Valid Data Payload: Read and trigger processing
        LoRa.readBytes((uint8_t*)&globalPayload, sizeof(globalPayload));
        globalRssi = LoRa.packetRssi();
        globalSnr = LoRa.packetSnr();
        globalPacketSize = packetSize;
        newLoRaPacket = true; 
    } 
    else if (packetSize == sizeof(AckPayload)) {
        // Silently ignore ACK packets meant for other nodes to prevent spamming errors
        return;
    }
    else {
        // Invalid size: Trigger error flag for main loop
        errorPacketSize = packetSize;
        sizeMismatchError = true;
    }
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
        Serial.println("[System Error] LoRa Initialization Failed!");
        while (1);
    }
    
    // LoRa Configuration (Must match Substation exactly)
    LoRa.setSpreadingFactor(10);
    LoRa.setSignalBandwidth(125E3);
    LoRa.setSyncWord(0xF3);
    LoRa.enableCrc();
    LoRa.setTxPower(14); // Required for sending ACKs

    LoRa.onReceive(onLoRaReceive);
    LoRa.receive();

    if (!ENABLE_MQTT) {
        updateScreen("TEST MODE", "LoRa Rx Only", "MQTT Offline");
        Serial.println("[System] Gateway Ready. Waiting for LoRa...");
    }
}

// --- Main Loop ---
void loop() {
    // 1. Maintain MQTT Connection
    if (ENABLE_MQTT) {
        if (!client.connected()) {
            reconnect_mqtt();
        }
        client.loop(); 
    }

    // 2. DEBUG: Handle LoRa Packet Size Error Safely
    if (sizeMismatchError) {
        sizeMismatchError = false; 
        Serial.println("\n[DEBUG - LORA RX ERROR] ======================");
        Serial.printf("=> Packet Size Mismatch!\n");
        Serial.printf("=> Expected: %d bytes | Received: %d bytes\n", sizeof(Payload), errorPacketSize);
        Serial.println("==============================================\n");
        
        updateScreen("RX ERROR", "Size Mismatch", "Got: " + String(errorPacketSize) + "B");
        LoRa.receive(); 
    }

    // 3. Process Successful LoRa Packet
    if (newLoRaPacket) {
        newLoRaPacket = false; 

        // --- SECTION A: LORA RECEIVE DEBUG ---
        Serial.println("\n[DEBUG - LORA RX] <<<<<<<<<<<<<<<<<<<<<<<<<<<<");
        Serial.printf("=> LoRa Packet Received!\n");
        Serial.printf("=> Size: %d Bytes | RSSI: %d dBm | SNR: %.1f dB\n", globalPacketSize, globalRssi, globalSnr);
        Serial.printf("=> Parsed Data -> UID: %u | SEQ: %u\n", globalPayload.uid, globalPayload.seq);

        updateScreen("LORA RX OK", "UID: " + String(globalPayload.uid), "RSSI: " + String(globalRssi));

        // --- SECTION B: IMMEDIATELY SEND ACK ---
        AckPayload ack;
        ack.uid = globalPayload.uid;
        ack.seq = globalPayload.seq;

        Serial.println("[DEBUG - LORA TX] >>>>>>>>>>>>>>>>>>>>>>>>>>>>");
        Serial.printf("=> Sending ACK for UID: %u | SEQ: %u\n", ack.uid, ack.seq);
        
        LoRa.beginPacket();
        LoRa.write((uint8_t*)&ack, sizeof(ack));
        LoRa.endPacket();
        
        Serial.println("=> ACK Sent Successfully!");

        // --- SECTION C: MQTT FORWARDING ---
        if (ENABLE_MQTT) {
            if (client.connected()) {
                Serial.println("[DEBUG - MQTT TX] >>>>>>>>>>>>>>>>>>>>>>>>>>>>");
                Serial.printf("=> Publishing to Topic: %s\n", mqtt_topic);
                
                bool pubSuccess = client.publish(mqtt_topic, (const uint8_t*)&globalPayload, sizeof(globalPayload));
                
                if (pubSuccess) {
                    Serial.println("=> Result: SUCCESS! Binary payload sent to Broker.");
                    updateScreen("DATA FWD", "UID: " + String(globalPayload.uid), "Pub: SUCCESS");
                } else {
                    Serial.println("=> Result: FAILED! Payload exceeded limits or connection dropped.");
                    updateScreen("DATA FWD", "UID: " + String(globalPayload.uid), "Pub: FAILED");
                }
            } else {
                Serial.println("[DEBUG - MQTT TX] Warning: Cannot publish, MQTT is disconnected.");
            }
        }
        Serial.println("----------------------------------------------");
        
        // --- SECTION D: RESUME LISTENING ---
        // Ensure radio goes back to listening mode after all processing is done
        LoRa.receive(); 
    }
}