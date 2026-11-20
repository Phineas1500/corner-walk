#include "walk_upload.hpp"
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <atomic>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_random.h>

extern bool wifiOnline();

namespace {
struct Relay { char url[144], token[64]; };
struct Walk { unsigned start, end; char id[33]; };
Preferences prefs;
Relay relay{};
Walk pending{};
enum class State { none, queued, upload, build, install, done, romReady, shortWalk, failed };
std::atomic<State> state{State::none};
std::atomic<bool> running{false};
uint32_t nextAttempt;
bool accepted;

int request(bool upload, char& result) {
  char url[184], authorization[72];
  snprintf(url, sizeof(url), "%s/%s", relay.url, pending.id);
  snprintf(authorization, sizeof(authorization), "Bearer %s", relay.token);
  esp_http_client_config_t config{};
  config.url = url;
  config.method = upload ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  config.timeout_ms = 12000;
  config.disable_auto_redirect = true;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  auto client = esp_http_client_init(&config);
  if (!client) return -1;
  esp_http_client_set_header(client, "Authorization", authorization);
  esp_http_client_set_header(client, "Content-Type", "application/x-ndjson");
  unsigned remaining = pending.end - pending.start;
  int code = -1;
  if (esp_http_client_open(client, upload ? remaining : 0) == ESP_OK) {
    if (upload) {
      File file = LittleFS.open("/walk-log.jsonl", "r");
      if (file && file.seek(pending.start)) {
        char chunk[1024];
        while (remaining) {
          const int count = file.read(reinterpret_cast<uint8_t*>(chunk), std::min<unsigned>(sizeof(chunk), remaining));
          if (count <= 0) break;
          int offset = 0;
          while (offset < count) {
            const int n = esp_http_client_write(client, chunk + offset, count - offset);
            if (n <= 0) break;
            offset += n;
          }
          if (offset != count) break;
          remaining -= count;
        }
      }
    }
    if ((!upload || !remaining) && esp_http_client_fetch_headers(client) >= 0) {
      code = esp_http_client_get_status_code(client);
      if (!upload && code == 200 && esp_http_client_read(client, &result, 1) != 1) code = -1;
    }
  }
  esp_http_client_cleanup(client);
  return code;
}

void transfer(void*) {
  char result = 0;
  if (!accepted) {
    state = State::upload;
    const int code = request(true, result);
    if (code == 200 || code == 202) { accepted = true; state = State::build; }
    else if (code == 400 || code == 413 || code == 409) state = State::failed;
    else state = State::queued;
  } else {
    const int code = request(false, result);
    if (code == 404) accepted = false;
    else if (code == 200) {
      switch (result) {
        case 'b': state = State::build; break;
        case 'f': state = State::install; break;
        case 'i': state = State::done; break;
        case 'r': state = State::romReady; break;
        case 'n': state = State::shortWalk; break;
        case 'e': state = State::failed; break;
      }
    }
  }
  running = false;
  vTaskDelete(nullptr);
}
}

void walkUploadBegin() {
  if (!prefs.begin("walk-upload", false)) return;
  if (prefs.getBytesLength("relay") == sizeof(relay)) prefs.getBytes("relay", &relay, sizeof(relay));
  if (prefs.getBytesLength("pending") == sizeof(pending)) {
    prefs.getBytes("pending", &pending, sizeof(pending));
    state = State::queued;
  }
}

bool walkUploadPending() { return pending.end > pending.start; }

bool queueWalkUpload(unsigned start, unsigned end) {
  if (walkUploadPending() || end <= start) return false;
  Walk job{start, end, {}};
  uint32_t random[4];
  esp_fill_random(random, sizeof(random));
  snprintf(job.id, sizeof(job.id), "%08lx%08lx%08lx%08lx",
           (unsigned long)random[0], (unsigned long)random[1],
           (unsigned long)random[2], (unsigned long)random[3]);
  if (prefs.putBytes("pending", &job, sizeof(job)) != sizeof(job)) return false;
  pending = job;
  accepted = false;
  nextAttempt = millis();
  state = State::queued;
  return true;
}

void walkUploadService() {
  if (!walkUploadPending() || running) return;
  const State current = state;
  if (current >= State::done) {
    if (prefs.remove("pending")) pending = {};
    return;
  }
  if (!*relay.url || !wifiOnline() || int32_t(millis() - nextAttempt) < 0) return;
  nextAttempt = millis() + (accepted ? 3500 : 10000);
  running = true;
  if (xTaskCreate(transfer, "walk_upload", 8192, nullptr, 2, nullptr) != pdPASS) running = false;
}

const char* walkUploadStatus() {
  switch (state.load()) {
    case State::queued: return !*relay.url ? "Walk saved. Mac relay needs setup." : "Walk saved. Waiting to send.";
    case State::upload: return "Sending walk to your Mac...";
    case State::build: return "Mac is building the game...";
    case State::install: return "Installing on the Chromatic...";
    case State::done: return "Game installed on the Chromatic.";
    case State::romReady: return "ROM ready on your Mac.";
    case State::shortWalk: return "Walk saved. Route too short.";
    case State::failed: return "Walk saved. Check your Mac.";
    case State::none: return nullptr;
  }
  return nullptr;
}

bool walkUploadCommand(const char* command) {
  if (!strcmp(command, "WALK STATUS")) {
    Serial.printf("STATUS WALK relay=%d pending=%d state=%d\n", *relay.url != 0,
                  walkUploadPending(), (int)state.load());
    return true;
  }
  constexpr char prefix[] = "WALK RELAY\t";
  if (strncmp(command, prefix, sizeof(prefix) - 1)) return false;
  const char* url = command + sizeof(prefix) - 1;
  const char* token = strchr(url, '\t');
  if (running || !token || strncmp(url, "https://", 8) || token - url >= sizeof(relay.url) ||
      token - url < 13 || strlen(token + 1) < 32 || strlen(token + 1) >= sizeof(relay.token)) {
    Serial.println("STATUS WALK invalid relay");
    return true;
  }
  Relay updated{};
  memcpy(updated.url, url, token - url);
  strlcpy(updated.token, token + 1, sizeof(updated.token));
  if (prefs.putBytes("relay", &updated, sizeof(updated)) == sizeof(updated)) {
    relay = updated;
    Serial.println("STATUS WALK relay ready");
  } else Serial.println("STATUS WALK could not save relay");
  return true;
}
