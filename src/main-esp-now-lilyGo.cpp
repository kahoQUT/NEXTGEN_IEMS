#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "SharedPayload.h" 

// config
#include "secrets.h"

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

// --- Interrupt-Safe Variables ---
volatile bool hasNewDataToRelay = false;
Payload pendingPayload;

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
// This function only handles "receiving", not "sending" or "waiting"
void OnDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
    if(len != sizeof(Payload)) {
        Serial.println("[Error] Payload size mismatch!");
        return;
    }
    // Copy data to global variable and set a flag to notify loop() to process it
    memcpy((uint8_t*)&pendingPayload, incomingData, sizeof(pendingPayload));
    hasNewDataToRelay = true; 
}

// --- Core Logic for LoRa Transmission and Waiting for ACK ---
bool sendLoRaWithAck(Payload data) {
    int maxRetries = 3;             // Maximum number of retries
    unsigned long timeoutMs = 1500; // Timeout for waiting for ACK (1.5 seconds)

    for (int attempt = 1; attempt <= maxRetries; attempt++) {
        Serial.printf("\n[LoRa] Tx Attempt %d/%d | UID: %d | SEQ: %d\n", attempt, maxRetries, data.uid, data.seq);
        
        // 1. Send data
        LoRa.beginPacket();
        LoRa.write((uint8_t*)&data, sizeof(data));
        LoRa.endPacket();

        // 2. Immediately switch to receive mode, prepare to listen for Gateway's ACK
        LoRa.receive(); 
        
        unsigned long startTime = millis();
        bool ackReceived = false;

        // 3. Continuously check for received packets within the Timeout period
        while (millis() - startTime < timeoutMs) {
            int packetSize = LoRa.parsePacket();
            
            if (packetSize == sizeof(AckPayload)) {
                AckPayload ack;
                LoRa.readBytes((uint8_t*)&ack, sizeof(ack));
                
                // Check if this ACK is for the data we just sent
                if (ack.uid == data.uid && ack.seq == data.seq) {
                    ackReceived = true;
                    break; // Successfully received, exit the waiting loop
                }
            }
        }

        // 4. Evaluate the result
        if (ackReceived) {
            Serial.println("[LoRa] TX SUCCESS: ACK Received!");
            return true; // Mission accomplished
        } else {
            Serial.println("[LoRa] TX FAILED: ACK Timeout.");
            delay(500); // Wait briefly before retrying to avoid band congestion
        }
    }

    Serial.println("[LoRa] TX CRITICAL: Max retries reached. Data dropped.");
    return false;
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
    // Triggered when ESP-NOW receives new data
    if (hasNewDataToRelay) {
        hasNewDataToRelay = false; // Reset the flag
        
        updateScreen("RELAYING...", "UID: " + String(pendingPayload.uid), "SEQ: " + String(pendingPayload.seq));
        
        // Execute transmission and retry logic
        bool success = sendLoRaWithAck(pendingPayload);
        
        if (success) {
            updateScreen("RELAY SUCCESS", "UID: " + String(pendingPayload.uid), "ACK Received");
        } else {
            updateScreen("RELAY FAILED", "UID: " + String(pendingPayload.uid), "Timeout Dropped");
        }
    }
}