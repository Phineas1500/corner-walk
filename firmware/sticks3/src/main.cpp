#include <Preferences.h>
#include <Wire.h>

#include <M5Unified.h>
#include <VL53L0X.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "../../shared/sensor_link.hpp"

// StickS3's legacy HAT row maps StickC G0 (SDA) to G8 and G26 (SCL) to G0.
constexpr int sda = 8, scl = 0;
VL53L0X tof;
sensorLink::Packet frame{};
sensorLink::Pair peer{};
bool found, paired, latched = true;
volatile bool delivered;
uint32_t nearSince, clearSince, nextSend, lastScreen, triggerTime;
uint16_t mm;
uint8_t channel = 1;
char command[72];
size_t used;

void setup() {
  auto config = M5.config();
  config.internal_mic = config.internal_spk = false;
  M5.begin(config);
  Serial.begin(115200);
  M5.Display.setRotation(1);
  M5.Display.setTextSize(2);
  Wire.begin(sda, scl, 100000);
  tof.setBus(&Wire);
  tof.setTimeout(100);
  found = tof.readReg(VL53L0X::IDENTIFICATION_MODEL_ID) == 0xee && tof.init();
  if (found) tof.startContinuous(100);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
  paired = sensorLink::begin(peer);
  if (paired) esp_now_register_send_cb([](const uint8_t*, esp_now_send_status_t status) { delivered = status == ESP_NOW_SEND_SUCCESS; });
  frame.magic = sensorLink::magic;
  frame.boot = esp_random();
}

void usb() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (used < sizeof(command)-1) command[used++] = c; continue; }
    command[used] = 0;
    if (!strncmp(command, "SENSOR PAIR ", 12)) {
      bool ok = sensorLink::savePair(command+12);
      Serial.println(ok ? "STATUS sensor paired" : "STATUS pairing failed");
      if (ok) { Serial.flush(); delay(80); ESP.restart(); }
    } else if (!strcmp(command,"SENSOR ID")) {
      sensorLink::printIdentity("stick");
    } else if (!strcmp(command, "STATUS")) {
      Serial.printf("STATUS tof=%d mm=%u pair=%d delivered=%d event=%lu channel=%u\n", found, mm, paired, delivered, (unsigned long)frame.event, channel);
    }
    memset(command, 0, sizeof(command)); used = 0;
  }
}

void loop() {
  usb();
  const uint32_t now = millis();
  if (found) {
    // Read the status and millimetres from the same sample before clearing it.
    uint32_t wait = millis();
    while (!(tof.readReg(VL53L0X::RESULT_INTERRUPT_STATUS) & 7) && millis()-wait < 250) delay(5);
    bool timedOut = millis()-wait >= 250;
    uint8_t sample[12]{};
    tof.readMulti(VL53L0X::RESULT_RANGE_STATUS,sample,sizeof(sample));
    timedOut |= tof.last_status != 0;
    tof.writeReg(VL53L0X::SYSTEM_INTERRUPT_CLEAR,1);
    mm = (uint16_t(sample[10]) << 8) | sample[11];
    uint8_t rangeStatus = (sample[0] >> 3) & 0x0f;
    bool valid = !timedOut && rangeStatus == 11 && mm > 0 && mm < 8190;
    bool near = valid && mm >= 100 && mm <= 550;
    bool clear = !timedOut && (rangeStatus == 4 || mm >= 8190 || (valid && mm >= 750));
    if (!near) nearSince = 0;
    else if (!nearSince) nearSince = now;
    if (!clear) clearSince = 0;
    else if (!clearSince) clearSince = now;
    if (clearSince && now - clearSince >= 500) latched = false;
    if (!latched && nearSince && now - nearSince >= 800) {
      latched = true; ++frame.event; triggerTime = now; nextSend = now;
    }
    frame.distanceMm = valid ? mm : 0;
  } else delay(20);
  if (paired && int32_t(now - nextSend) >= 0) {
    if (!delivered) {
      channel = channel % 11 + 1;
      esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    }
    auto message = frame;
    // Only a fresh approach can trigger a photo after a link outage.
    if (!frame.event || now - triggerTime > 3000) message.event = 0;
    esp_now_send(peer.mac, reinterpret_cast<const uint8_t*>(&message), sizeof(message));
    nextSend = now + (delivered ? 400 : 140);
  }
  if (now - lastScreen >= 450) {
    lastScreen = now;
    M5.Display.setCursor(0,0);
    M5.Display.printf("Corner Walk          \nToF: %s         \n", found ? "ready" : "check HAT");
    if (frame.distanceMm) M5.Display.printf("%u mm          \n", mm);
    else M5.Display.println("Out of range      ");
    M5.Display.printf("Core: %-12s\nScans: %lu     ", !paired ? "pair via USB" : delivered ? "linked" : "searching", (unsigned long)frame.event);
  }
}
