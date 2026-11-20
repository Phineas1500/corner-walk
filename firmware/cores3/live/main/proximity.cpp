#include "proximity.hpp"
#include "camera.hpp"
#include "../../../shared/sensor_link.hpp"
#include <M5Unified.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <atomic>

namespace {
sensorLink::Pair peer{};
Preferences settings;
QueueHandle_t approaches;
bool radioReady, enabled, sawEvent;
uint32_t lastBoot, lastEvent, received, lastStart;
std::atomic<uint32_t> lastPacket{0};
std::atomic<unsigned> lastDistance{0};
constexpr size_t jpegLimit = 10240;
uint8_t* jpeg;
size_t photoSize;
uint8_t* preview;
size_t previewSize;
unsigned previewFrames;
char opener[257];
std::atomic<bool> hasOpener{false};

struct Detection { uint32_t boot, event, at; };

void receive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(sensorLink::Packet) || memcmp(info->src_addr,peer.mac,6)) return;
  sensorLink::Packet p;
  memcpy(&p,data,sizeof(p));
  if (p.magic != sensorLink::magic) return;
  uint32_t now = millis();
  lastPacket = now;
  lastDistance = p.distanceMm;
  if (!p.event || (sawEvent && p.boot == lastBoot && p.event <= lastEvent)) return;
  sawEvent = true;
  lastBoot = p.boot;
  lastEvent = p.event;
  Detection event{p.boot, p.event, now};
  xQueueOverwrite(approaches, &event);
}
}

void proximityBegin() {
  if (settings.begin("walk-camera",false)) enabled = settings.getBool("enabled",true);
  approaches = xQueueCreate(1,sizeof(Detection));
  radioReady = approaches && sensorLink::begin(peer) && esp_now_register_recv_cb(receive)==ESP_OK;
}

bool proximityCommand(char* cmd) {
  if (!strcmp(cmd,"SENSOR ID")) { sensorLink::printIdentity("core"); return true; }
  if (!strncmp(cmd,"SENSOR PAIR ",12)) {
    bool ok = sensorLink::savePair(cmd+12);
    Serial.println(ok ? "STATUS sensor paired" : "STATUS pairing failed");
    if (ok) { Serial.flush(); delay(100); ESP.restart(); }
    return true;
  }
  if (strcmp(cmd,"SENSOR STATUS")) return false;
  uint32_t last = lastPacket.load();
  Serial.printf("STATUS SENSOR pair=%d enabled=%d age_ms=%lu mm=%u events=%lu photo_bytes=%u opener=%d aiming=%d previews=%u\n",
    radioReady, enabled, (unsigned long)(last ? millis()-last : UINT32_MAX), lastDistance.load(),
    (unsigned long)received, (unsigned)photoSize, hasOpener.load(), isAiming(), previewFrames);
  return true;
}

bool proximityTriggered() {
  Detection d{};
  if (!radioReady || xQueueReceive(approaches,&d,0)!=pdTRUE) return false;
  ++received;
  return enabled && !isAiming() && millis()-d.at < 3000 && (!lastStart || millis()-lastStart >= 30000);
}

void proximityToggle() {
  enabled = !enabled;
  settings.putBool("enabled",enabled);
}

bool isAiming() { return preview != nullptr; }
void setAiming(bool on) {
  if (on && !preview) preview = static_cast<uint8_t*>(heap_caps_malloc(jpegLimit,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if (!on) { free(preview); preview=nullptr; previewSize=0; }
}

bool autoPhotoEnabled() { return enabled; }

const char* stickStatus() {
  uint32_t last = lastPacket.load();
  if (!radioReady) return "unpaired";
  return last && millis()-last < 4000 ? "connected" : "waiting";
}

const uint8_t* proximityImage(size_t& length) {
  if (isAiming()) {
    previewSize=0;
    if (captureWalkPhoto(nullptr,preview,jpegLimit,&previewSize)==ESP_OK) ++previewFrames;
    length = previewSize;
    return preview;
  }
  length = photoSize;
  return jpeg;
}

bool captureTriggerPhoto() {
  lastStart = millis();
  clearTriggerPhoto();
  jpeg = static_cast<uint8_t*>(heap_caps_malloc(jpegLimit,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if (!jpeg) return false;
  const auto result = captureWalkPhoto(nullptr,jpeg,jpegLimit,&photoSize);
  if (result!=ESP_OK || !photoSize) { clearTriggerPhoto(); return false; }
  Serial.printf("STATUS SENSOR captured bytes=%u\n",(unsigned)photoSize);
  return true;
}

void clearTriggerPhoto() {
  free(jpeg); jpeg=nullptr; photoSize=0;
  hasOpener=false; opener[0]=0;
}

const char* triggerOpener() { return hasOpener.load() ? opener : nullptr; }
size_t triggerPhotoSize() { return photoSize; }

// These hooks run on the SDK's signaling task. The UI owns the buffer from
// before start() until the SDK finishes the session, including failed starts.
extern "C" size_t walk_photo_capacity() { return photoSize ? 32+4*((photoSize+2)/3) : 0; }
extern "C" bool walk_append_photo(char* body,size_t capacity) {
  if (!photoSize) return true;
  size_t end = strlen(body);
  constexpr char field[] = ",\"walk_photo\":\"";
  if (!end || body[end-1]!='}' || end+walk_photo_capacity()>capacity) return false;
  char* out=body+end-1;
  memcpy(out,field,sizeof(field)-1); out+=sizeof(field)-1;
  size_t n=0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out),capacity-(out-body)-3,
      &n,jpeg,photoSize)!=0) return false;
  memcpy(out+n,"\"}",3);
  return true;
}
extern "C" bool walk_read_opener(const char* data,size_t length) {
  if (!photoSize) return true;
  auto* root=cJSON_ParseWithLength(data,length);
  auto* field=cJSON_GetObjectItemCaseSensitive(root,"walk_opener");
  const char* value=cJSON_GetStringValue(field);
  bool ok=value && *value && strlen(value)<sizeof(opener);
  if (ok) { strlcpy(opener,value,sizeof(opener)); hasOpener=true; }
  cJSON_Delete(root);
  return ok;
}
extern "C" const char* walk_scene_greeting() { return triggerOpener(); }
