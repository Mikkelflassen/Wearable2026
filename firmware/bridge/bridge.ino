// Wearable2026 - USB radio bridge
// Flash this on a SPARE ESP32-C3 that stays plugged into the laptop. It is not
// a bracelet: it has no buttons, snaps or motor and does not count as a person
// in the group. It only passes messages between the laptop and the bracelets:
//
//   radio (ESP-NOW)  ->  USB serial      everything the bracelets send
//   USB serial       ->  radio           "W26 ..." lines from sim/sim.py
//
// So sim/sim.py --port <bridge port> lets the pretend bracelets talk to real
// bracelets that run on battery, and shows when a real bracelet vibrates.
//
// Upload exactly like the bracelet, but point at firmware/bridge.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

const uint8_t RADIO_CHANNEL = 1;  // must match RADIO_CHANNEL in bracelet.ino
const int BRIDGE_LINE = 64;
const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t radioQueue;
bool radioOk = false;

void handleRadioPacket(const uint8_t* data, int len) {
  char buf[BRIDGE_LINE];
  if (len <= 0 || len >= BRIDGE_LINE) return;
  memcpy(buf, data, len);
  memset(buf + len, 0, BRIDGE_LINE - len);
  xQueueSend(radioQueue, buf, 0);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRadio(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  handleRadioPacket(data, len);
}
#else
void onRadio(const uint8_t* mac, const uint8_t* data, int len) {
  handleRadioPacket(data, len);
}
#endif

void setup() {
  Serial.begin(115200);
  radioQueue = xQueueCreate(16, BRIDGE_LINE);
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() == ESP_OK) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) == ESP_OK) {
      esp_now_register_recv_cb(onRadio);
      radioOk = true;
    }
  }
  Serial.println(radioOk ? "# radio bridge ready" : "# ERROR: radio did not start");
}

void loop() {
  char line[BRIDGE_LINE];
  while (xQueueReceive(radioQueue, line, 0) == pdTRUE) Serial.println(line);

  static char in[BRIDGE_LINE];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      in[n] = 0;
      if (n && strncmp(in, "W26 ", 4) == 0 && radioOk)
        esp_now_send(BROADCAST, (const uint8_t*)in, n);
      else if (n && in[0] == '!')
        Serial.println("# this is the radio bridge: use the bracelet's own buttons and snaps");
      n = 0;
    } else if (n < BRIDGE_LINE - 1) {
      in[n++] = c;
    }
  }
  delay(2);
}
