#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>

namespace sensorLink {
struct Packet {
  uint32_t magic, boot, event;
  uint16_t distanceMm, reserved;
};
constexpr uint32_t magic = 0x31465743;
struct Pair { uint8_t mac[6]; uint8_t key[16]; };
inline void printIdentity(const char* board) {
  uint8_t mac[6];
  if (esp_wifi_get_mac(WIFI_IF_STA,mac)!=ESP_OK) { Serial.println("STATUS Wi-Fi unavailable"); return; }
  Serial.printf("STATUS SENSOR ID %s %02x%02x%02x%02x%02x%02x\n",
                board,mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}
inline bool hexBytes(const char* text, uint8_t* bytes, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t value = 0;
    for (int j = 0; j < 2; ++j) {
      char c = *text++;
      if (c >= '0' && c <= '9') value = value * 16 + c - '0';
      else if (c >= 'a' && c <= 'f') value = value * 16 + c - 'a' + 10;
      else return false;
    }
    bytes[i] = value;
  }
  return true;
}
inline bool savePair(const char* args) {
  if (strlen(args) != 45 || args[12] != ' ') return false;
  Pair p{};
  if (!hexBytes(args, p.mac, 6) || !hexBytes(args+13, p.key, 16)) return false;
  Preferences prefs;
  return prefs.begin("walk-sensors", false) && prefs.putBytes("peer", &p, sizeof(p)) == sizeof(p);
}
inline bool begin(Pair& p) {
  Preferences prefs;
  if (!prefs.begin("walk-sensors", true) || prefs.getBytes("peer", &p, sizeof(p)) != sizeof(p)) return false;
  if (esp_now_init() != ESP_OK) return false;
  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, p.mac, 6);
  memcpy(peer.lmk, p.key, 16);
  peer.encrypt = true;
  peer.ifidx = WIFI_IF_STA;
  return esp_now_add_peer(&peer) == ESP_OK;
}
}
