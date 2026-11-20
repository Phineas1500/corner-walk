#include <Arduino.h>
#include <LittleFS.h>
#include <M5Unified.h>
#include <TinyGPS++.h>
#include <WiFi.h>
#ifdef WALK_LIVE
#include <driver/uart.h>
#include "voice.hpp"
#include "walk_upload.hpp"
#endif
#include <esp_partition.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <fcntl.h>
#include <unistd.h>

constexpr char logFile[] = "/walk-log.jsonl";
constexpr char logPath[] = "/littlefs/walk-log.jsonl";
constexpr int gpsRx = 18, gpsTx = 17;  // DinBase blue Port C
#ifdef WALK_LIVE
// Keep the GPS on its own hardware UART while the Live driver streams I2S.
constexpr uart_port_t gpsPort = UART_NUM_1;
esp_err_t gpsPortStatus = ESP_ERR_INVALID_STATE;
void startGps() {
  uart_config_t config{};
  config.baud_rate = 115200;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  gpsPortStatus = uart_param_config(gpsPort, &config);
  if (gpsPortStatus == ESP_OK)
    gpsPortStatus = uart_set_pin(gpsPort, gpsTx, gpsRx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (gpsPortStatus == ESP_OK)
    gpsPortStatus = uart_driver_install(gpsPort, 4096, 0, 0, nullptr, 0);
}
#endif
TinyGPSPlus gps;
const char* storageError = nullptr;
unsigned long saved = 0, lastMessageMs = 0, lastScreenMs = 0;
uint32_t lastDate = 0, lastSecond = 0;
bool activeFix = false;
#ifdef WALK_LIVE
bool recordingWalk = false;
bool indoorWalk = false;
unsigned indoorStop = 0;
constexpr unsigned maxIndoorStops = 12;
unsigned walkStartOffset = 0;
#endif
uint32_t lastFixMs = 0;
File download;
size_t remaining = 0;
uint32_t lastTransferMs = 0;
// Long enough for the USB command, 32-byte SSID and 63-byte WPA passphrase.
char command[256];  // Also fits a broker URL and its scoped device token.
size_t commandLength = 0;
bool commandOverflow = false;
bool hasWifiProfile = false;
uint32_t lastWifiAttempt = 0;
bool scanInProgress = false;
volatile uint8_t wifiFailure = 0;

bool wifiOnline() {
  return WiFi.status() == WL_CONNECTED && uint32_t(WiFi.localIP()) != 0;
}

void startWifi() {
  // The ESP32 Wi-Fi driver stores the hotspot profile in NVS, outside the journal.
  WiFi.persistent(true);
  WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
    uint8_t reason = info.wifi_sta_disconnected.reason;
    if (reason != WIFI_REASON_ASSOC_LEAVE) wifiFailure = reason;
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
  wifi_config_t config{};
  hasWifiProfile = esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && config.sta.ssid[0];
  if (hasWifiProfile) WiFi.begin();
  lastWifiAttempt = millis();
}

void joinWifi(char* ssid) {
  char* password = strchr(ssid, '\t');
  if (password) *password++ = '\0';
  if (!password || !*ssid || strlen(ssid) > 32 || strlen(password) < 8 || strlen(password) > 63) {
    Serial.println("STATUS WIFI invalid profile");
    return;
  }
  lastWifiAttempt = millis();
  wifiFailure = 0;
  if (WiFi.begin(ssid, password) == WL_CONNECT_FAILED) {
    Serial.println("STATUS WIFI could not start");
    return;
  }
  hasWifiProfile = true;
  Serial.println("STATUS WIFI joining");
}

void startWifiScan(bool passive) {
  if (!scanInProgress) {
    WiFi.setAutoReconnect(false);
    if (hasWifiProfile && !wifiOnline()) {
      // Stop the driver's background connection scan before starting discovery.
      WiFi.mode(WIFI_OFF);
      WiFi.mode(WIFI_STA);
    }
    scanInProgress = WiFi.scanNetworks(true, true, passive, 420) == WIFI_SCAN_RUNNING;
    if (!scanInProgress) WiFi.setAutoReconnect(true);
  }
  Serial.println(scanInProgress ? "STATUS WIFI scanning" : "STATUS WIFI scan failed");
}

void reportWifiScan() {
  int count = WiFi.scanComplete();
  if (count < 0) {
    Serial.println(count == WIFI_SCAN_RUNNING ? "STATUS WIFI scanning" : "STATUS WIFI scan failed");
    return;
  }
  // Hex keeps arbitrary network names from inserting serial protocol delimiters.
  const char digits[] = "0123456789abcdef";
  Serial.print("STATUS WIFI networks");
  for (int i = 0; i < count; ++i) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    char encoded[65];
    size_t length = std::min<size_t>(ssid.length(), 32);
    for (size_t j = 0; j < length; ++j) {
      uint8_t ch = ssid[j];
      encoded[2*j] = digits[ch >> 4];
      encoded[2*j+1] = digits[ch & 15];
    }
    encoded[2*length] = '\0';
    Serial.printf(" %s", encoded);
  }
  Serial.println();
  WiFi.scanDelete();
}

bool blankLogPartition() {
  const auto* part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                              ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) return false;
  uint8_t block[1024];
  for (size_t pos = 0; pos < part->size; pos += sizeof(block)) {
    size_t count = std::min<size_t>(sizeof(block), part->size - pos);
    if (esp_partition_read(part, pos, block, count) != ESP_OK) return false;
    for (size_t i = 0; i < count; ++i) {
      if (block[i] != 0xff) return false;
    }
  }
  return true;
}

bool appendLine(const char* line) {
  int fd = open(logPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return false;
  const size_t size = strlen(line);
  bool written = write(fd, line, size) == static_cast<ssize_t>(size);
  return close(fd) == 0 && written;
}

void startJournal() {
  // Initialize empty storage only. Preserve existing data if mounting fails.
  if (!LittleFS.begin(false) && !(blankLogPartition() && LittleFS.begin(true))) {
    storageError = "Flash mount failed";
    return;
  }
#ifndef WALK_LIVE
  if (!appendLine("{\"type\":\"start\"}\n")) storageError = "Flash write failed";
#endif
}

#ifdef WALK_LIVE
unsigned journalSize() {
  File file = LittleFS.open(logFile, "r");
  return file ? file.size() : 0;
}

void beginWalk(bool indoor) {
  if (storageError || recordingWalk || walkUploadPending()) return;
  walkStartOffset = journalSize();
  char entry[128] = "{\"type\":\"start\"}\n";
  if (indoor)
    snprintf(entry, sizeof(entry),
             "{\"type\":\"start\"}\n{\"type\":\"stop\",\"index\":1,\"walk_ms\":%lld}\n",
             (long long)(esp_timer_get_time() / 1000));
  if (!appendLine(entry)) {
    storageError = "Couldn't start the walk";
    return;
  }
  indoorWalk = indoor;
  indoorStop = indoor ? 1 : 0;
  recordingWalk = true;
  saved = lastDate = lastSecond = 0;
}

bool canAdvanceIndoorWalk() {
  return recordingWalk && indoorWalk && indoorStop < maxIndoorStops;
}

void advanceIndoorWalk() {
  if (!canAdvanceIndoorWalk()) return;
  char entry[96];
  snprintf(entry, sizeof(entry), "{\"type\":\"stop\",\"index\":%u,\"walk_ms\":%lld}\n",
           indoorStop + 1, (long long)(esp_timer_get_time() / 1000));
  if (appendLine(entry)) ++indoorStop;
  else storageError = "Couldn't save next stop";
}

void endWalk() {
  if (!recordingWalk) return;
  if (queueWalkUpload(walkStartOffset, journalSize())) {
    recordingWalk = false;
    indoorWalk = false;
  } else storageError = "Couldn't queue walk. Try End Walk.";
}
#endif

void recordRmc() {
  // TinyGPS++ commits a date on every RMC but speed only on an active RMC.
  // Consume both update flags here so a void RMC can never reuse an old fix.
  if (!gps.date.isUpdated()) return;
  const uint32_t date = gps.date.value();
  activeFix = gps.speed.isUpdated() && gps.location.isValid();
  const double speed = gps.speed.mps();
  if (!activeFix) return;
  lastFixMs = millis();
#ifdef WALK_LIVE
  if (!recordingWalk || indoorWalk) return;
#endif
  const uint32_t second = gps.time.value() / 100;
  if (storageError || !gps.time.isValid() || gps.date.month() < 1 || gps.date.month() > 12 ||
      gps.date.day() < 1 || gps.date.day() > 31 || (date == lastDate && second == lastSecond)) return;

  char line[168];
  snprintf(line, sizeof(line),
           "{\"type\":\"fix\",\"walk_ms\":%lld,\"time\":\"%04u-%02u-%02uT%02u:%02u:%02uZ\","
           "\"lat\":%.6f,\"lon\":%.6f,\"speed_mps\":%.2f}\n",
           (long long)(esp_timer_get_time() / 1000), gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(),
           gps.time.minute(), gps.time.second(), gps.location.lat(), gps.location.lng(), speed);
  if (!appendLine(line)) {
    storageError = "Flash write failed";
    return;
  }
  ++saved;
  lastDate = date;
  lastSecond = second;
}

const char* statusText() {
  if (storageError) return storageError;
#ifdef WALK_LIVE
  if (indoorWalk) return "Recording indoor walk";
#endif
  if (!gps.passedChecksum() || millis() - lastMessageMs > 3000) return "Check GPS on blue Port C";
  if (!activeFix || millis() - lastFixMs > 3000) return "Waiting for GPS fix";
#ifdef WALK_LIVE
  if (!recordingWalk) return "GPS ready";
#endif
  return saved ? "Recording" : "Waiting for GPS time";
}

void drawScreen() {
#ifdef WALK_LIVE
  voiceDraw();
#else
  M5.Display.fillRect(0, 32, M5.Display.width(), M5.Display.height() - 32, 0);
  M5.Display.setCursor(0, 32);
  M5.Display.println(statusText());
  M5.Display.printf("Saved this run: %lu\n", saved);
  M5.Display.printf("GPS messages: %lu\n", (unsigned long)gps.passedChecksum());
  if (gps.satellites.isValid() && gps.satellites.age() < 3000) {
    M5.Display.printf("Satellites used: %lu\n", (unsigned long)gps.satellites.value());
  }
  if (wifiOnline()) M5.Display.printf("Wi-Fi: %s\n", WiFi.localIP().toString().c_str());
  else M5.Display.println(hasWifiProfile ? "Wi-Fi: joining hotspot" : "Wi-Fi: set up over USB");
#endif
}

void handleCommand() {
#ifdef WALK_LIVE
  if (voiceCommand(command)) return;
#endif
  if (!strcmp(command, "STATUS")) {
    Serial.printf("STATUS %s | saved=%lu messages=%lu wifi=%s last_wifi_error=%u", statusText(), saved,
                  (unsigned long)gps.passedChecksum(), wifiOnline() ? "connected" : "offline", wifiFailure);
#ifdef WALK_LIVE
    Serial.printf(" bytes=%lu bad_checksum=%lu port_power=%u uart=%s",
                  (unsigned long)gps.charsProcessed(), (unsigned long)gps.failedChecksum(),
                  M5.Power.getExtOutput(), esp_err_to_name(gpsPortStatus));
    Serial.printf(" indoor=%d stop=%u", indoorWalk, indoorStop);
#endif
    Serial.println();
  } else if (!strcmp(command, "WIFI SCAN") || !strcmp(command, "WIFI PASSIVE")) {
    startWifiScan(!strcmp(command, "WIFI PASSIVE"));
  } else if (!strcmp(command, "WIFI LIST")) {
    reportWifiScan();
  } else if (!strncmp(command, "WIFI JOIN\t", 10)) {
    joinWifi(command + 10);
  } else if (!strncmp(command, "WIFI CHECK\t", 11)) {
    if (wifiOnline() && WiFi.SSID() == (command + 11))
      Serial.printf("STATUS WIFI connected %s\n", WiFi.localIP().toString().c_str());
    else
      Serial.println("STATUS WIFI waiting");
  } else if (!strcmp(command, "DUMP")) {
    download = LittleFS.open(logFile, "r");
    if (!download) {
      Serial.println("ERROR cannot open walk log");
      return;
    }
    remaining = download.size();  // fixes appended during export belong to the next snapshot
    lastTransferMs = millis();
    Serial.printf("BEGIN %u\n", (unsigned)remaining);
  }
}

void serviceUsb() {
  if (download) {
    if (!remaining) {
      download.close();
      Serial.println("END");
      return;
    }
    if (millis() - lastTransferMs > 5000) {
      download.close();
      return;
    }
    uint8_t buf[128];
    const int room = Serial.availableForWrite();
    if (room <= 0) return;
    const size_t count = std::min(remaining, std::min(sizeof(buf), (size_t)room));
    const size_t pos = download.position();
    const size_t got = download.read(buf, count);
    if (!got) { download.close(); Serial.println("ERROR reading walk log"); return; }
    const size_t sent = Serial.write(buf, got);
    if (sent < got) download.seek(pos + sent);
    remaining -= sent;
    if (sent) lastTransferMs = millis();
    return;
  }
  while (Serial.available() && !download) {
    const char ch = Serial.read();
    if (ch == '\n') {
      command[commandLength] = '\0';
      if (!commandOverflow) handleCommand();
      memset(command, 0, sizeof(command));
      commandLength = 0;
      commandOverflow = false;
    } else if (ch != '\r') {
      if (ch && commandLength < sizeof(command) - 1) command[commandLength++] = ch;
      else commandOverflow = true;
    }
  }
}

void setup() {
#ifdef WALK_LIVE
  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  M5.begin(cfg);
#else
  M5.begin(M5.config());
#endif
  M5.Power.setExtOutput(true);
  Serial.begin(115200);
#ifdef WALK_LIVE
  startGps();
#else
  Serial1.setRxBufferSize(4096);
  Serial1.begin(115200, SERIAL_8N1, gpsRx, gpsTx);
#endif
  M5.Display.setRotation(1);
#ifndef WALK_LIVE
  M5.Display.setTextSize(2);
  M5.Display.println("Walk logger");
#endif
  startJournal();
  startWifi();
#ifdef WALK_LIVE
  voiceBegin();
  walkUploadBegin();
#endif
  drawScreen();
}

void loop() {
#ifdef WALK_LIVE
  uint8_t buffer[256];
  int count = gpsPortStatus == ESP_OK ? uart_read_bytes(gpsPort, buffer, sizeof(buffer), 0) : 0;
  for (int i = 0; i < count; ++i) {
    if (gps.encode(buffer[i])) {
#else
  while (Serial1.available()) {
    if (gps.encode(Serial1.read())) {
#endif
      lastMessageMs = millis();
      recordRmc();
    }
  }
  serviceUsb();
#ifdef WALK_LIVE
  voiceService();
  walkUploadService();
#endif
  if (scanInProgress && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    scanInProgress = false;
    WiFi.setAutoReconnect(true);
    if (hasWifiProfile && !wifiOnline()) WiFi.begin();
    lastWifiAttempt = millis();
  }
  if (!scanInProgress && hasWifiProfile && !wifiOnline() && millis() - lastWifiAttempt >= 30000) {
    lastWifiAttempt = millis();
    WiFi.reconnect();
  }
  if (millis() - lastScreenMs >= 1000) {
    lastScreenMs = millis();
    drawScreen();
  }
  delay(1);
}
