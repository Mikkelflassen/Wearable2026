// Wearable2026 - group conversation bracelet
// Board: ESP32-C3 Super Mini (HW-466AB). Flash the SAME sketch on every
// bracelet; each one takes its id from its own chip.
//
// Arduino IDE: Tools > Board > "ESP32C3 Dev Module",
//              Tools > USB CDC On Boot > "Enabled"   (otherwise no serial output)
// PlatformIO:  just `pio run -t upload` from the repository root.
//
// The bracelets talk to each other with ESP-NOW (the Wi-Fi radio, but no
// router or password needed). Everything a bracelet sends or hears is also
// copied to USB serial, and anything typed/sent on USB serial is passed on to
// the radio. That is how sim/sim.py plays the bracelets you have not built yet.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif
#include "Core.h"

using namespace w26;

struct Button {
  uint8_t pin;
  bool down;
  bool raw;
  uint32_t since;
};

// ---- wiring (see README) ---------------------------------------------------
// Buttons and snaps connect the pin to GND; the internal pull-ups are used.
const uint8_t PIN_BTN_AGREE = 3;
const uint8_t PIN_BTN_DISAGREE = 4;
const uint8_t PIN_SNAP_TALK = 5;   // the three snaps sewn onto the bracelet
const uint8_t PIN_SNAP_TOPIC = 6;
const uint8_t PIN_SNAP_PAUSE = 7;  // (the loose snap on the wire goes to GND)
const uint8_t PIN_MOTOR = 10;      // to the transistor/driver, NOT straight to the motor
const uint8_t PIN_LED = 8;         // on-board LED, lit when LOW

const uint8_t RADIO_CHANNEL = 1;   // must be the same on all bracelets

// ---- feel: tune these while testing on the wrist ---------------------------
const uint16_t DEBOUNCE_MS = 40;
const uint16_t SNAP_DEBOUNCE_MS = 80;
const uint16_t DOUBLE_PRESS_MS = 400;  // two presses within this = "comment"
const uint16_t CLEAR_HOLD_MS = 1500;   // hold both buttons = withdraw break wish
const uint16_t PATTERN_GAP_MS = 700;   // silence between two patterns

// on, off, on, off ... in milliseconds, ending with 0
const uint16_t PAT_AGREE[] = {150, 0};
const uint16_t PAT_DISAGREE[] = {150, 150, 150, 0};
const uint16_t PAT_COMMENT[] = {150, 150, 150, 150, 150, 0};
const uint16_t PAT_YOUR_TURN[] = {700, 0};
const uint16_t PAT_TOPIC_TURN[] = {400, 200, 400, 0};
const uint16_t PAT_BREAK[] = {80, 80, 80, 80, 80, 80, 80, 80, 900, 0};
const uint16_t PAT_ACK[] = {40, 0};

const uint16_t* patternSteps(Pattern p) {
  switch (p) {
    case P_AGREE: return PAT_AGREE;
    case P_DISAGREE: return PAT_DISAGREE;
    case P_COMMENT: return PAT_COMMENT;
    case P_YOUR_TURN: return PAT_YOUR_TURN;
    case P_TOPIC_TURN: return PAT_TOPIC_TURN;
    case P_BREAK: return PAT_BREAK;
    default: return PAT_ACK;
  }
}

Core core;
const uint8_t BROADCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t radioQueue;
bool radioOk = false;

// ---- radio -----------------------------------------------------------------
void handleRadioPacket(const uint8_t* data, int len) {
  char buf[W26_LINE];
  if (len <= 0 || len >= W26_LINE) return;
  memcpy(buf, data, len);
  memset(buf + len, 0, W26_LINE - len);
  xQueueSend(radioQueue, buf, 0);  // handled in loop(), not in the Wi-Fi task
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

void radioSend(const char* line) {
  if (radioOk) esp_now_send(BROADCAST, (const uint8_t*)line, strlen(line));
}

void setupRadio() {
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);  // plenty for one room, gentler on the small battery
  esp_wifi_set_channel(RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("# ERROR: esp_now_init failed");
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("# ERROR: esp_now_add_peer failed");
    return;
  }
  esp_now_register_recv_cb(onRadio);
  radioOk = true;
}

// ---- serial ----------------------------------------------------------------
void handleSerialLine(char* line, uint32_t now) {
  if (line[0] == '!') {
    char reply[96];
    handleCommand(core, line, now, reply, sizeof(reply));
    if (reply[0]) {
      Serial.print("# ");
      Serial.println(reply);
    }
  } else if (strncmp(line, "W26 ", 4) == 0) {
    core.onLine(line, now);  // from a simulated bracelet on the laptop...
    radioSend(line);         // ...and on to any real bracelets
  }
}

void readSerial(uint32_t now) {
  static char line[W26_LINE];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line[n] = 0;
      if (n) handleSerialLine(line, now);
      n = 0;
    } else if (n < W26_LINE - 1) {
      line[n++] = c;
    }
  }
}

// ---- snaps -----------------------------------------------------------------
void readSnaps(uint32_t now) {
  static Snap candidate = SNAP_NONE, stable = SNAP_NONE;
  static uint32_t since = 0;
  Snap raw = SNAP_NONE;
  if (digitalRead(PIN_SNAP_TALK) == LOW) raw = SNAP_TALK;
  else if (digitalRead(PIN_SNAP_TOPIC) == LOW) raw = SNAP_TOPIC;
  else if (digitalRead(PIN_SNAP_PAUSE) == LOW) raw = SNAP_PAUSE;
  if (raw != candidate) {
    candidate = raw;
    since = now;
  }
  if (candidate != stable && now - since >= SNAP_DEBOUNCE_MS) {
    stable = candidate;
    core.setSnap(stable, now);
  }
}

// ---- buttons ---------------------------------------------------------------
Button btnAgree = {PIN_BTN_AGREE, false, false, 0};
Button btnDisagree = {PIN_BTN_DISAGREE, false, false, 0};

// Returns true on the moment the button goes down.
bool pressed(Button& b, uint32_t now) {
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.raw) {
    b.raw = raw;
    b.since = now;
  }
  if (b.raw != b.down && now - b.since >= DEBOUNCE_MS) {
    b.down = b.raw;
    return b.down;
  }
  return false;
}

void readButtons(uint32_t now) {
  static char waiting = 0;        // a single press waiting to see if a second follows
  static uint32_t waitingSince = 0;
  static uint32_t bothSince = 0;
  static bool bothHandled = false;

  bool a = pressed(btnAgree, now);
  bool d = pressed(btnDisagree, now);

  if (btnAgree.down && btnDisagree.down) {  // both held: withdraw break wish
    if (bothSince == 0) {
      bothSince = now;
      bothHandled = false;
      waiting = 0;
    }
    if (!bothHandled && now - bothSince >= CLEAR_HOLD_MS) {
      bothHandled = true;
      core.clearPause(now);
    }
    return;
  }
  bothSince = 0;

  char hit = a ? 'A' : (d ? 'D' : 0);
  if (hit) {
    if (waiting == hit) {  // second press of the same button
      core.press('C', now);
      waiting = 0;
    } else {
      if (waiting) core.press(waiting, now);
      waiting = hit;
      waitingSince = now;
    }
  }
  if (waiting && now - waitingSince >= DOUBLE_PRESS_MS) {
    core.press(waiting, now);
    waiting = 0;
  }
}

// ---- vibration motor -------------------------------------------------------
void runMotor(uint32_t now) {
  static const uint16_t* steps = nullptr;
  static uint8_t step = 0;
  static uint32_t stepEnd = 0, quietUntil = 0;

  if (!steps) {
    if ((int32_t)(now - quietUntil) < 0) return;
    Pattern p = core.popPattern();
    if (p == P_NONE) return;
    Serial.print("# vibe ");
    Serial.println(patternName(p));
    steps = patternSteps(p);
    step = 0;
    stepEnd = now + steps[0];
    digitalWrite(PIN_MOTOR, HIGH);
    return;
  }
  if ((int32_t)(now - stepEnd) < 0) return;
  step++;
  if (steps[step] == 0) {
    digitalWrite(PIN_MOTOR, LOW);
    steps = nullptr;
    quietUntil = now + PATTERN_GAP_MS;
    return;
  }
  digitalWrite(PIN_MOTOR, (step % 2 == 0) ? HIGH : LOW);
  stepEnd = now + steps[step];
}

// ---- setup / loop ----------------------------------------------------------
void setup() {
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  pinMode(PIN_BTN_AGREE, INPUT_PULLUP);
  pinMode(PIN_BTN_DISAGREE, INPUT_PULLUP);
  pinMode(PIN_SNAP_TALK, INPUT_PULLUP);
  pinMode(PIN_SNAP_TOPIC, INPUT_PULLUP);
  pinMode(PIN_SNAP_PAUSE, INPUT_PULLUP);

  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);  // never stall when running on battery with no USB attached
#endif

  radioQueue = xQueueCreate(8, W26_LINE);
  setupRadio();

  // A 4-digit id from the chip's unique MAC address (1000-9999).
  uint64_t mac = ESP.getEfuseMac();
  uint32_t h = (uint32_t)(mac ^ (mac >> 16) ^ (mac >> 32)) * 2654435761UL;
  uint16_t id = 1000 + (h >> 8) % 9000;
  core.begin(id, millis(), (uint16_t)esp_random());

  Serial.print("# bracelet ");
  Serial.print(id);
  Serial.println(" ready");
}

void loop() {
  uint32_t now = millis();

  char line[W26_LINE];
  while (xQueueReceive(radioQueue, line, 0) == pdTRUE) {
    core.onLine(line, now);
    Serial.println(line);  // let the laptop simulator hear the real bracelets
  }
  readSerial(now);
  readSnaps(now);
  readButtons(now);
  core.tick(now);

  while (core.popLine(line, sizeof(line))) {
    radioSend(line);
    Serial.println(line);
  }
  runMotor(now);
  digitalWrite(PIN_LED, core.isSpeaker() ? LOW : HIGH);  // lit while you are the speaker
  delay(2);
}
