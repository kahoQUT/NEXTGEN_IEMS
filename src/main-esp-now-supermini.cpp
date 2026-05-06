#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include "SharedPayload.h"

// ==========================================
// 1. Substation (LilyGo) MAC Address
// ==========================================
// Replace with the actual MAC address of your LilyGo Substation
uint8_t broadcastAddress[] = {0xF0, 0x24, 0xF9, 0x93, 0x8F, 0x24}; 

// Map "MINI-002" to an integer UID to save bandwidth
const uint32_t DEVICE_UID = 2; 
#define SLEEP_TIME_SEC 30

// ==========================================
// 2. Core Architecture: Binary Buffer (RTC Memory)
// ==========================================

#define MAX_BUFFER_SIZE 15 // Store up to 15 offline records
RTC_DATA_ATTR Payload readingBuffer[MAX_BUFFER_SIZE];
RTC_DATA_ATTR int bufferCount = 0; // Number of unsent records currently queued

RTC_DATA_ATTR float cumulativeImport = 5000.0;
RTC_DATA_ATTR uint32_t messageCounter = 0; // The SEQ number

// ==========================================
// 3. State Machine Variables (for waiting ACK)
// ==========================================
volatile bool ackReceived = false;
volatile bool deliverySuccess = false;

// ESP-NOW send result callback (interrupt function)
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  deliverySuccess = (status == ESP_NOW_SEND_SUCCESS);
  ackReceived = true; // Notify main loop: ACK received
}

void setup() {
  Serial.begin(115200);
  delay(3000); // For debugging logs during development, remove in production
  Serial.println("\n\n--- SuperMini Fault-Tolerant Wake ---");
  
  // 1. Determine wakeup cause to handle cold boots vs deep sleep
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  if (wakeup_reason != ESP_SLEEP_WAKEUP_TIMER) {
      // Not a timer wakeup means it is a power-on reset or hardware reset
      Serial.println("Cold Boot detected. Generating random starting Sequence.");
      // Initialize sequence with a random number to prevent Redis deduplication collisions
      messageCounter = esp_random() & 0xFFFFFF; 
  }

  unsigned long startTime = millis();

  // 2. Generate new data and store into RTC buffer
  messageCounter++;
  cumulativeImport += random(1, 10) / 100.0;
  
  // Check if buffer is full; if full, drop the oldest record (FIFO)
  if (bufferCount >= MAX_BUFFER_SIZE) {
      Serial.println("Warning: Buffer full. Dropping oldest reading.");
      for(int i = 1; i < MAX_BUFFER_SIZE; i++) {
          readingBuffer[i-1] = readingBuffer[i];
      }
      bufferCount = MAX_BUFFER_SIZE - 1;
  }
  
  // Map values directly to the new binary struct
  readingBuffer[bufferCount].uid = DEVICE_UID;
  readingBuffer[bufferCount].seq = messageCounter;
  readingBuffer[bufferCount].kwh_import = cumulativeImport;
  readingBuffer[bufferCount].kwh_export = 0.0; // Simulated export value
  readingBuffer[bufferCount].voltage = 235.0 + random(-5, 6);
  readingBuffer[bufferCount].battery_v = 3.7 + random(-1, 2)/10.0;
  readingBuffer[bufferCount].community_id = 1;
  readingBuffer[bufferCount].unit_id = 15;
  
  bufferCount++;
  
  Serial.printf("Current Buffer Size: %d/%d\n", bufferCount, MAX_BUFFER_SIZE);
  Serial.printf("Generated UID: %u | SEQ: %u\n", readingBuffer[bufferCount-1].uid, readingBuffer[bufferCount-1].seq);

  // 3. Initialize RF antenna and ESP-NOW
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
      Serial.println("ESP-NOW Init Failed");
      return;
  }
  esp_now_register_send_cb(OnDataSent);

  esp_now_peer_info_t peerInfo;
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false; 
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Failed to add peer");
      return;
  }

  // 4. Batch sending mechanism 
  int successfullySent = 0;
  
  for (int i = 0; i < bufferCount; i++) {
      ackReceived = false;
      deliverySuccess = false;
      
      Serial.printf("Sending SEQ [%u]... ", readingBuffer[i].seq);
      
      // Use the new sizeof(Payload)
      esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &readingBuffer[i], sizeof(Payload));
      
      if (result == ESP_OK) {
          // Wait synchronously for ACK (max 100 ms)
          unsigned long waitStart = millis();
          while (!ackReceived && (millis() - waitStart < 100)) {
              delay(1); 
          }
          
          if (ackReceived && deliverySuccess) {
              Serial.println("OK");
              successfullySent++;
          } else {
              Serial.println("FAIL (No ACK). Stopping flush.");
              break; // Stop immediately on failure to save power
          }
      } else {
          Serial.println("Send Error");
          break;
      }
  }

  // 5. Clean up buffer 
  if (successfullySent > 0) {
      // Shift remaining unsent data forward
      for (int i = 0; i < bufferCount - successfullySent; i++) {
          readingBuffer[i] = readingBuffer[i + successfullySent];
      }
      bufferCount -= successfullySent;
      Serial.printf("Queue flushed. Remaining in buffer: %d\n", bufferCount);
  }

  unsigned long trueActiveTime = millis() - startTime;
  Serial.printf("[True Active Time]: %lu ms. Sleeping for %d sec...\n", trueActiveTime, SLEEP_TIME_SEC);

  // 6. Power down and go to sleep
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup(SLEEP_TIME_SEC * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {

}