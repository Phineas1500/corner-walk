#include <M5Unified.h>
#include <TinyGPS++.h>

// CoreS3 + DinBase, blue Port C, GPS/BDS Unit v1.1 (U032-V11).
constexpr int gpsRxPin = 18;
constexpr int gpsTxPin = 17;
constexpr unsigned long staleAfterMs = 3000;

TinyGPSPlus gps;
unsigned long lastSentenceMs = 0;
unsigned long lastDrawMs = 0;

void setup() {
  M5.begin(M5.config());
  M5.Power.setExtOutput(true);
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, gpsRxPin, gpsTxPin);

  M5.Display.setRotation(1);
  M5.Display.setTextSize(2);
  M5.Display.println("Walk GPS / Port C");
}

void loop() {
  while (Serial1.available()) {
    if (gps.encode(Serial1.read())) {
      lastSentenceMs = millis();
    }
  }

  const unsigned long now = millis();
  if (now - lastDrawMs < 1000) {
    delay(1);
    return;
  }
  lastDrawMs = now;

  const bool receiving = gps.passedChecksum() > 0 && now - lastSentenceMs < staleAfterMs;
  const bool fixed = receiving && gps.location.isValid() && gps.location.age() < staleAfterMs;
  const char* status = "Check GPS on blue Port C";
  if (receiving) {
    status = fixed ? "Position fix" : "GPS data / waiting for fix";
  }

  M5.Display.fillRect(0, 30, M5.Display.width(), M5.Display.height() - 30, 0);
  M5.Display.setCursor(0, 30);
  M5.Display.println(status);
  M5.Display.printf("Valid messages: %lu\n", (unsigned long)gps.passedChecksum());
  if (fixed) {
    M5.Display.printf("Lat: %.6f\nLon: %.6f\n", gps.location.lat(), gps.location.lng());
  }
  Serial.printf("%s | valid messages: %lu\n", status, (unsigned long)gps.passedChecksum());
}
