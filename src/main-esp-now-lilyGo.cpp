#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SharedPayload.h" 

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

// --- Display Helper ---
void updateScreen(String title, String line1, String line2) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("=== " + title + " ===");
    display.println("");
    display.println(line1);
    display.println(line2);
    display.display();
}

// --- ESP-NOW Callback ---
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    // 1. Strict size validation
    if(len != sizeof(Payload)) {
        Serial.println("[Error] Payload size mismatch!");
        return;
    }

    // 2. Map binary data to struct
    Payload payload;
    memcpy(&payload, incomingData, sizeof(payload));

    Serial.printf("[ESP-NOW] Rx | UID: %d | SEQ: %d\n", payload.uid, payload.seq);

    // 3. Forward exact binary via LoRa
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&payload, sizeof(payload));
    LoRa.endPacket();

    Serial.printf("[LoRa] Tx | Size: %d | RSSI: %d\n", sizeof(payload), LoRa.packetRssi());
    updateScreen("RELAY ACTIVE", "UID: " + String(payload.uid), "SEQ: " + String(payload.seq));
}

void setup() {
    Serial.begin(115200);
    delay(5000);
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(WHITE);

    SPI.begin(SCK, MISO, MOSI, SS);
    LoRa.setPins(SS, RST, DIO0);
    if (!LoRa.begin(LORA_BAND)) {
        updateScreen("ERROR", "LoRa Init Failed", "");
        while (1);
    }
    
    // LoRa Optimization for Broadcast
    LoRa.setSpreadingFactor(10); 
    LoRa.setSignalBandwidth(125E3);
    LoRa.setSyncWord(0xF3);
    LoRa.enableCrc();
    LoRa.setTxPower(14);

    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        Serial.println("[Error] ESP-NOW Init Failed");
        return;
    }
    esp_now_register_recv_cb(OnDataRecv);
    String fullMac = WiFi.macAddress();
    Serial.println(">>> MAC Address: " + fullMac + " <<<");
    updateScreen("SUBSTATION", "Ready to Relay", "MAC: " + fullMac);
}

void loop() {
}