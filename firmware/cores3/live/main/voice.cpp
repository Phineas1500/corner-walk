#include <Arduino.h>
#include <M5Unified.h>
#include <Preferences.h>
#include <TinyGPS++.h>
#include <WiFi.h>
#include <cJSON.h>
#include <ctime>
#include <atomic>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_crt_bundle.h>
#include <netdb.h>
#include "audio.hpp"
#include "camera.hpp"
#include "proximity.hpp"
#include "voice.hpp"
#include "walk_screen.hpp"
#include "walk_upload.hpp"
#include "live_esp32.h"

extern TinyGPSPlus gps;
extern bool activeFix;
extern bool recordingWalk;
extern bool indoorWalk;
extern void beginWalk(bool indoor);
extern void advanceIndoorWalk();
extern void endWalk();
extern uint32_t lastFixMs;
extern unsigned long lastScreenMs;
extern const char* storageError;
extern bool appendLine(const char*);
extern bool wifiOnline();

namespace {
struct Profile { char url[144]; char token[64]; };
Profile profile{};
Preferences prefs;
bool initialized, online, tracking, stopping, hasAnchor;
bool namingCorner;
bool sensorChat, savedScene;
bool finishWalkAfterChat;
const char* sensorNotice = nullptr;
uint32_t activeSince;
int64_t photoWalkMs, chatWalkMs, mediaStartWalkMs;
constexpr uint32_t chatLimitMs = 10 * 60 * 1000;
uint32_t started, polls, noteCount, transcriptRevision;
double latitude, longitude;
char anchorTime[32], lastNote[120];
String heard, reply;
live_transcript_t pendingChat{};
std::atomic<size_t> lastPhotoBytes{0};
std::atomic<esp_err_t> lastPhotoError{ESP_OK};
std::atomic<bool> checkingRelay{false};
}
extern "C" const char* walk_session_url() { return profile.url; }

// Exercise the board's DNS and TLS without credentials, microphone or session.
// Reaching the configured broker anonymously returns HTTP 401.
static void checkRelay(void*) {
  const String url(profile.url);
  int slash = url.indexOf('/', 8);
  const String host = url.substring(8, slash);
  struct addrinfo hints{}, *address = nullptr;
  hints.ai_family = AF_INET;
  int dns = getaddrinfo(host.c_str(), nullptr, &hints, &address);
  if (address) freeaddrinfo(address);
  esp_http_client_config_t config{};
  config.url = profile.url;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 8000;
  config.disable_auto_redirect = true;
  config.method = HTTP_METHOD_POST;
  auto client = esp_http_client_init(&config);
  int code = 0;
  esp_err_t result = ESP_FAIL;
  if (client && dns == 0) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, "{}", 2);
    result = esp_http_client_perform(client);
    code = esp_http_client_get_status_code(client);
  }
  if (client) esp_http_client_cleanup(client);
  Serial.printf("STATUS VOICE check dns=%d utc_set=%d tls_http=%s code=%d\n",
                dns, time(nullptr) > 1700000000, esp_err_to_name(result), code);
  checkingRelay = false;
  vTaskDelete(nullptr);
}

static bool initLive() {
  if (initialized) return true;
  if (!*profile.token || !*profile.url) return false;
  live_esp32_config_t cfg{};
  cfg.model = "gpt-live-1.1";
  cfg.responses_model = "gpt-6-luna";
  cfg.responses_web_search = true;
  cfg.capture_photo = [](void* context, uint8_t* jpeg, size_t cap, size_t* len) {
    const auto err = captureWalkPhoto(context, jpeg, cap, len);
    lastPhotoBytes = err == ESP_OK ? *len : 0;
    lastPhotoError = err;
    return err;
  };
  cfg.api_key = profile.token; // Device-specific broker secret. The project key lives on the Mac.
  cfg.audio_open = audio::open;
  cfg.audio_close = audio::close;
  cfg.read = audio::read;
  cfg.write = audio::write;
  initialized = live_esp32_init(&cfg) == ESP_OK;
  if (initialized) live_esp32_network_changed(wifiOnline());
  return initialized;
}

void voiceBegin() {
  proximityBegin();
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");
  if (prefs.begin("walk-voice", false) && prefs.getBytesLength("broker") == sizeof(profile)) {
    prefs.getBytes("broker", &profile, sizeof(profile));
    initLive();
  }
}

static void tapVoice(bool nameCorner, bool fromSensor = false) {
  if (finishWalkAfterChat || (fromSensor && !recordingWalk)) return;
  if (checkingRelay || !initLive()) return;
  live_esp32_status_t s{};
  live_esp32_status(&s);
  if (s.busy || tracking) {
    if (!fromSensor && s.busy) { live_esp32_stop(); stopping = true; }
    return;
  }
  if (!wifiOnline()) {
    if (fromSensor) sensorNotice = "Auto photo needs the hotspot.";
    return;
  }
  sensorNotice = nullptr;
  setAiming(false);
  if (fromSensor) photoWalkMs = esp_timer_get_time() / 1000;
  if (fromSensor && !captureTriggerPhoto()) {
    sensorNotice = "Camera capture failed.";
    return;
  }
  if (live_esp32_start() != ESP_OK) { clearTriggerPhoto(); return; }
  if (fromSensor) {
    lastPhotoBytes = triggerPhotoSize();
    lastPhotoError = ESP_OK;
  }
  started = millis();
  chatWalkMs = esp_timer_get_time() / 1000;
  tracking = true;
  sensorChat = fromSensor;
  savedScene = false;
  activeSince = 0;
  mediaStartWalkMs = 0;
  namingCorner = nameCorner;
  stopping = false;
  heard = ""; reply = ""; transcriptRevision = s.transcript_revision;
  pendingChat = {};
  hasAnchor = activeFix && millis() - lastFixMs < 3000 && gps.location.isValid();
  latitude = gps.location.lat(); longitude = gps.location.lng();
  anchorTime[0] = 0;
  time_t now = time(nullptr);
  if (hasAnchor && gps.date.isValid() && gps.time.isValid()) {
    snprintf(anchorTime,sizeof(anchorTime),"%04u-%02u-%02uT%02u:%02u:%02uZ",gps.date.year(),
             gps.date.month(),gps.date.day(),gps.time.hour(),gps.time.minute(),gps.time.second());
  } else if (now > 1700000000) {
    struct tm utc{}; gmtime_r(&now,&utc);
    strftime(anchorTime,sizeof(anchorTime),"%Y-%m-%dT%H:%M:%SZ",&utc);
  }
}

static bool appendJson(cJSON* json) {
  char* line = cJSON_PrintUnformatted(json);
  if (!line) return false;
  String record(line);
  cJSON_free(line);
  if (!record.length() || !record.concat('\n')) return false;
  return appendLine(record.c_str());
}

// The SDK retains eight mutable caption blocks. Journal each final version once
// as it leaves that window, then flush the tail when the conversation closes.
static void flushChatBefore(uint32_t firstRetained) {
  if (!recordingWalk) return;
  for (unsigned i = 0; i < pendingChat.count; ++i) {
    const auto& block = pendingChat.blocks[i];
    if (block.id >= firstRetained) break;
    auto* row = cJSON_CreateObject();
    if (!row) { storageError = "Chat memory full"; return; }
    cJSON_AddStringToObject(row, "type", "chat");
    cJSON_AddNumberToObject(row, "session_ms", started);
    if (*anchorTime) cJSON_AddStringToObject(row, "session_time", anchorTime);
    cJSON_AddNumberToObject(row, "start_ms", block.start_ms);
    cJSON_AddNumberToObject(row, "end_ms", block.end_ms);
    // Blocks can be flushed minutes later. Locate the beginning of the
    // utterance on the same monotonic clock used by the GPS journal.
    if (mediaStartWalkMs && (block.start_ms || block.end_ms))
      cJSON_AddNumberToObject(row, "walk_ms", mediaStartWalkMs + block.start_ms);
    cJSON_AddStringToObject(row, "speaker", block.speaker == LIVE_SPEAKER_USER ? "user" : "assistant");
    cJSON_AddStringToObject(row, "text", block.text);
    if (!appendJson(row)) storageError = "Chat save failed";
    cJSON_Delete(row);
  }
}

static void saveNote() {
  if (!recordingWalk || !heard.length()) return;
  auto* json = cJSON_CreateObject();
  if (!json) { storageError = "Voice note memory full"; return; }
  cJSON_AddStringToObject(json,"type","said");
  cJSON_AddNumberToObject(json,"walk_ms",chatWalkMs);
  if (*anchorTime) cJSON_AddStringToObject(json,"time",anchorTime);
  else cJSON_AddNullToObject(json,"time");
  if (hasAnchor) {
    cJSON_AddNumberToObject(json,"lat",latitude);
    cJSON_AddNumberToObject(json,"lon",longitude);
  } else {
    cJSON_AddNullToObject(json,"lat"); cJSON_AddNullToObject(json,"lon");
  }
  cJSON_AddStringToObject(json,"text",heard.c_str());
  cJSON_AddStringToObject(json,"reply",reply.c_str());
  bool ok = appendJson(json);
  cJSON_Delete(json);
  if (!ok) storageError="Voice note save failed";
  else { ++noteCount; snprintf(lastNote,sizeof(lastNote),"%s",heard.c_str()); }
}

void voiceService() {
  M5.update();
  auto point = M5.Touch.getDetail();
  if (point.wasPressed()) {
    const WalkAction action = walkActionAt(point.x, point.y);
    switch (action) {
      case WalkAction::toggleAuto: proximityToggle(); break;
      case WalkAction::aim:
        if (!tracking) setAiming(true);
        break;
      case WalkAction::done: setAiming(false); break;
      case WalkAction::talk:
      case WalkAction::nameCorner:
        if (!tracking && !isAiming()) tapVoice(action == WalkAction::nameCorner);
        break;
      case WalkAction::endChat:
        if (tracking && !stopping) { live_esp32_stop(); stopping = true; }
        break;
      case WalkAction::startWalk:
      case WalkAction::startIndoor:
        if (!tracking) {
          beginWalk(action == WalkAction::startIndoor);
          if (recordingWalk) {
            noteCount = 0;
            if (indoorWalk && !autoPhotoEnabled()) proximityToggle();
          }
        }
        break;
      case WalkAction::nextStop:
        if (!finishWalkAfterChat) advanceIndoorWalk();
        break;
      case WalkAction::endWalk:
        if (tracking) {
          finishWalkAfterChat = true;
          if (!stopping) { live_esp32_stop(); stopping = true; }
        } else endWalk();
        break;
      case WalkAction::none: break;
    }
    if (action != WalkAction::none) lastScreenMs = millis() - 1000;
  }
  if (proximityTriggered() && !tracking) tapVoice(false, true);
  if (!initialized || millis() - polls < 100) return;
  polls = millis();
  bool connected = wifiOnline();
  if (connected != online) { online=connected; live_esp32_network_changed(online); }
  if (!tracking) return;
  live_esp32_status_t s{};
  live_esp32_status(&s);
  if (s.active && !activeSince) {
    activeSince = millis();
    mediaStartWalkMs = esp_timer_get_time() / 1000;
  }
  if (sensorChat && !savedScene && triggerOpener()) {
    auto* row = cJSON_CreateObject();
    if (row) {
      cJSON_AddStringToObject(row,"type","scene");
      cJSON_AddStringToObject(row,"text",triggerOpener());
      cJSON_AddNumberToObject(row,"session_ms",started);
      cJSON_AddNumberToObject(row,"walk_ms",photoWalkMs);
      if (*anchorTime) cJSON_AddStringToObject(row,"time",anchorTime);
      if (hasAnchor) {
        cJSON_AddNumberToObject(row,"lat",latitude);
        cJSON_AddNumberToObject(row,"lon",longitude);
      }
      if (!appendJson(row)) storageError = "Scene save failed";
      cJSON_Delete(row);
    }
    savedScene = true;
  }
  if (s.transcript_revision != transcriptRevision) {
    static live_transcript_t t;
    live_esp32_transcripts(&t);
    if (t.count) {
      if (!namingCorner) {
        flushChatBefore(t.blocks[0].id);
        pendingChat = t;
      }
      String user, assistant;
      for (unsigned i=0;i<t.count;++i) {
        auto &text = t.blocks[i].speaker == LIVE_SPEAKER_USER ? user : assistant;
        if (text.length()) text += ' ';
        text += t.blocks[i].text;
      }
      if (user.length()) heard = user;
      if (assistant.length()) reply = assistant;
    }
    transcriptRevision = s.transcript_revision;
  }
  if (!s.busy && millis()-started > 500) {
    if (namingCorner) saveNote();
    else flushChatBefore(UINT32_MAX);
    if (sensorChat && !savedScene) sensorNotice = "Auto photo chat failed. Check Wi-Fi and relay.";
    clearTriggerPhoto();
    tracking = false;
    if (finishWalkAfterChat) { endWalk(); finishWalkAfterChat = false; }
  } else if (!stopping && (millis()-started > (namingCorner ? 60000u : chatLimitMs) || !connected ||
      (sensorChat && activeSince && !heard.length() && millis()-activeSince > 60000))) {
    live_esp32_stop();
    stopping = true;
  }
}

void voiceDraw() {
  live_esp32_status_t s{};
  if (initialized) live_esp32_status(&s);
  const bool chat = s.busy || tracking;
  const WalkView view = isAiming() ? WalkView::aiming : chat ? WalkView::chat : WalkView::home;
  const char* status = recordingWalk ? sensorNotice : walkUploadStatus();
  if (s.photo_busy) status = "Taking a photo...";
  else if (chat || strstr(s.status, "failed") || strstr(s.status, "Failed")) status = s.status;
  const char* caption = reply.length() ? reply.c_str() : triggerOpener() ? triggerOpener() : lastNote;
  drawWalkScreen({view, initialized, namingCorner, noteCount,
                  (unsigned long)s.photos_captured + sensorChat, status, caption});
}

bool voiceCommand(char* cmd) {
  if (walkUploadCommand(cmd)) return true;
  if (walkScreenCommand(cmd)) return true;
  if (proximityCommand(cmd)) return true;
  if (strncmp(cmd,"VOICE ",6)) return false;
  live_esp32_status_t s{};
  if (initialized) live_esp32_status(&s);
  if (!strncmp(cmd,"VOICE SET\t",10)) {
    char* url=cmd+10;
    char* token=strchr(url,'\t');
    if (token) *token++=0;
    if (!token || strncmp(url,"https://",8) || strlen(url)>=sizeof(profile.url) ||
        strlen(token)<32 || strlen(token)>=sizeof(profile.token) || s.busy || tracking || checkingRelay) {
      Serial.println("STATUS VOICE invalid configuration"); return true;
    }
    Profile next{};
    strlcpy(next.url,url,sizeof(next.url)); strlcpy(next.token,token,sizeof(next.token));
    if (prefs.putBytes("broker",&next,sizeof(next)) != sizeof(next)) {
      Serial.println("STATUS VOICE storage error"); return true;
    }
    profile=next;
    Serial.printf("STATUS VOICE %s\n",initLive() ? "ready" : "init failed");
  } else if (!strcmp(cmd,"VOICE STATUS")) {
    Serial.printf("STATUS VOICE ready=%d busy=%d active=%d mic=%lu tx=%lu rx=%lu played=%lu notes=%lu heard=%u reply=%u photos=%lu photo_errors=%lu jpeg=%u camera=%s state=%s\n",
      initialized, s.busy, s.active, (unsigned long)s.mic_peak, (unsigned long)s.sent_packets,
      (unsigned long)s.received_packets, (unsigned long)s.played_samples, (unsigned long)noteCount,
      (unsigned)heard.length(), (unsigned)reply.length(), (unsigned long)s.photos_captured + sensorChat,
      (unsigned long)s.photo_errors, (unsigned)lastPhotoBytes.load(), esp_err_to_name(lastPhotoError.load()), s.status);
  } else if (!strcmp(cmd,"VOICE PHOTO")) {
    Serial.printf("STATUS VOICE photo %s\n", initialized && !s.busy &&
      live_esp32_photo_test() == ESP_OK ? "started" : "unavailable");
  } else if (!strcmp(cmd,"VOICE CHECK")) {
    if (initialized && !s.busy && !checkingRelay.exchange(true)) {
      if (xTaskCreate(checkRelay, "relay_check", 8192, nullptr, 2, nullptr) != pdPASS)
        checkingRelay = false;
    }
    Serial.println(checkingRelay ? "STATUS VOICE checking relay" : "STATUS VOICE check unavailable");
  } else if (!strcmp(cmd,"VOICE AUDIO")) {
    Serial.printf("STATUS VOICE audio %s\n",initialized && !s.busy && live_esp32_audio_test()==ESP_OK ? "started" : "unavailable");
  } else if (!strcmp(cmd,"VOICE STOP")) {
    if (initialized && s.busy) { live_esp32_stop(); stopping=true; }
    Serial.println("STATUS VOICE stopped");
  } else { Serial.println("STATUS VOICE unknown command"); }
  return true;
}
