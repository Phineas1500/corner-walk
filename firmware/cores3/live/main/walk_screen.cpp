#include "walk_screen.hpp"
#include "proximity.hpp"
#include "walk_upload.hpp"
#include <M5Unified.h>
#include <TinyGPS++.h>
#include <WiFi.h>
#include <algorithm>

extern TinyGPSPlus gps;
extern unsigned long saved;
extern bool hasWifiProfile;
extern bool recordingWalk;
extern bool indoorWalk;
extern unsigned indoorStop;
extern bool canAdvanceIndoorWalk();
extern bool wifiOnline();
extern const char* statusText();

namespace {
constexpr int inset = 8, gap = 6, buttonHeight = 44;
constexpr uint16_t background = 0x08e3, surface = 0x19c7, border = 0x338b;
constexpr uint16_t ink = 0xf7bf, muted = 0xa5d7, accent = 0x6f17, dark = 0x0924;
M5Canvas frame(&M5.Display);
bool triedCanvas;

struct Button {
  int x, y, w, h;
  WalkAction action;
};
Button buttons[6];
unsigned buttonCount;

void drawButton(lgfx::LGFXBase& g, int x, int y, int w, const char* label,
                WalkAction action, bool highlight = false) {
  buttons[buttonCount++] = {x, y, w, buttonHeight, action};
  g.fillRoundRect(x, y, w, buttonHeight, 6, highlight ? accent : surface);
  g.drawRoundRect(x, y, w, buttonHeight, 6, highlight ? accent : border);
  g.setFont(&fonts::Font2);
  g.setTextSize(1.25f);
  if (g.textWidth(label) > w - 10) g.setTextSize(1);
  g.setTextColor(highlight ? dark : action == WalkAction::none ? muted : ink);
  g.drawCenterString(label, x + w / 2, y + (buttonHeight - g.fontHeight()) / 2);
}

void cameraPanel(lgfx::LGFXBase& g, int top, int bottom) {
  size_t length = 0;
  const uint8_t* image = proximityImage(length);
  const int w = g.width() - 2 * inset, h = bottom - top;
  const float scale = std::min(w / 320.f, h / 240.f);
  const int x = (g.width() - int(320 * scale)) / 2;
  const int y = top + (h - int(240 * scale)) / 2;
  g.fillRect(inset, top, w, h, surface);
  if (!length || !g.drawJpg(image, length, x, y, w, h, 0, 0, scale)) {
    g.setTextColor(muted);
    g.drawCenterString("Waiting for image", g.width() / 2, top + (h - g.fontHeight()) / 2);
  }
}

// Keep the newest caption lines inside the space above the controls.
void paragraph(lgfx::LGFXBase& g, const char* text, int top, int bottom) {
  if (!text || !*text) return;
  const int w = g.width() - 2 * inset;
  const int lines = (bottom - top) / g.fontHeight();
  if (lines < 1) return;
  const size_t budget = lines * (w / g.textWidth("M"));
  const size_t length = strlen(text);
  if (length > budget) {
    text += length - budget;
    while ((*text & 0xc0) == 0x80) ++text;
  }
  g.setClipRect(inset, top, w, lines * g.fontHeight());
  g.setScrollRect(inset, top, w, lines * g.fontHeight(), background);
  g.setTextScroll(true);
  g.setCursor(inset, top);
  g.print(text);
  g.setTextScroll(false);
  g.clearClipRect();
}
}

WalkAction walkActionAt(int x, int y) {
  for (unsigned i = 0; i < buttonCount; ++i) {
    const Button& b = buttons[i];
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return b.action;
  }
  return WalkAction::none;
}

void drawWalkScreen(const WalkScreenState& state) {
  const int w = M5.Display.width(), h = M5.Display.height();
  if (!triedCanvas) {
    triedCanvas = true;
    frame.setPsram(true);
    frame.setColorDepth(16);
    frame.createSprite(w, h);
  }
  lgfx::LGFXBase& g = frame.getBuffer() ? static_cast<lgfx::LGFXBase&>(frame) : M5.Display;
  const int bottomRow = h - inset - buttonHeight;
  const int topRow = bottomRow - gap - buttonHeight;
  const int walkRow = topRow - gap - buttonHeight;
  const int full = w - 2 * inset, left = (full - gap) / 2, rightX = inset + left + gap;
  const bool tall = h >= 280;
  const bool aim = state.view == WalkView::aiming, chat = state.view == WalkView::chat;
  const bool photo = chat && triggerPhotoSize();
  const bool online = wifiOnline();
  g.fillScreen(background);
  g.setTextWrap(false, false);
  g.setFont(&fonts::Font2);
  g.setTextSize(1.25f);
  g.setTextColor(ink);
  g.drawString(aim ? "Aim camera" : photo ? "Trigger photo" : chat ? "Walk chat" : "Corner Walk", inset, 5);
  g.setTextSize(1);
  g.setTextColor(online ? accent : muted);
  g.drawRightString(online ? "Wi-Fi on" : "Wi-Fi off", w - inset, 7);
  g.drawFastHLine(inset, 29, full, border);

  buttonCount = 0;
  if (aim) {
    cameraPanel(g, 35, bottomRow - 42);
    g.setTextColor(ink);
    g.drawCenterString("Center your object", w / 2, bottomRow - 38);
    g.setTextColor(muted);
    g.drawCenterString("Auto photos paused while aiming", w / 2, bottomRow - 21);
    drawButton(g, inset, bottomRow, full, "Done", WalkAction::done, true);
  } else {
    const int statusY = walkRow - (tall ? 56 : 20);
    if (photo) {
      cameraPanel(g, 35, std::max(55, statusY - 4));
    } else {
      g.setTextColor(ink);
      g.drawString(statusText(), inset, 35);
      g.setFont(&fonts::Font0);
      g.setTextColor(muted);
      g.setCursor(inset, tall ? 63 : 55);
      if (indoorWalk) g.printf("Stop %u", indoorStop);
      else g.printf("Points %lu   GPS msgs %lu", saved, (unsigned long)gps.passedChecksum());
      g.setCursor(inset, tall ? 82 : 68);
      if (!indoorWalk) {
        if (gps.satellites.isValid() && gps.satellites.age() < 3000)
          g.printf("Satellites %lu   ", (unsigned long)gps.satellites.value());
        else g.print("Satellites --   ");
      }
      g.printf("Stick %s", stickStatus());
      g.setCursor(inset, tall ? 101 : 81);
      if (online) g.print(WiFi.localIP().toString());
      else g.print(hasWifiProfile ? "Joining hotspot" : "Set up Wi-Fi via USB");
    }
    g.setFont(&fonts::Font0);
    g.setTextSize(1);
    g.setTextColor(muted);
    g.setCursor(inset, statusY);
    g.printf("Corners %lu    Photos %lu", state.corners, state.photos);
    g.setTextColor(ink);
    paragraph(g, state.status, statusY + 11, statusY + (tall ? 31 : 20));
    g.setTextColor(muted);
    paragraph(g, state.caption, statusY + (tall ? 31 : 20), walkRow - 3);

    if (indoorWalk) {
      drawButton(g, inset, walkRow, left, canAdvanceIndoorWalk() ? "Next Stop" : "12 stops",
                 canAdvanceIndoorWalk() ? WalkAction::nextStop : WalkAction::none);
      drawButton(g, rightX, walkRow, w - inset - rightX, "End & Install", WalkAction::endWalk);
    } else if (recordingWalk)
      drawButton(g, inset, walkRow, full, "End Walk & Install", WalkAction::endWalk);
    else if (walkUploadPending())
      drawButton(g, inset, walkRow, full, "Walk sent or queued", WalkAction::none);
    else if (!chat) {
      drawButton(g, inset, walkRow, left, "Start Walk", WalkAction::startWalk, true);
      drawButton(g, rightX, walkRow, w - inset - rightX, "Indoor Walk", WalkAction::startIndoor);
    }

    drawButton(g, inset, topRow, left, autoPhotoEnabled() ? "Auto: ON" : "Auto: OFF",
               WalkAction::toggleAuto, autoPhotoEnabled());
    drawButton(g, rightX, topRow, w - inset - rightX, chat ? "In chat" : "Aim camera",
               chat ? WalkAction::none : WalkAction::aim);
    if (chat) {
      drawButton(g, inset, bottomRow, full, state.namingCorner ? "End and save corner" : "End chat",
                 WalkAction::endChat, true);
    } else if (state.voiceReady) {
      drawButton(g, inset, bottomRow, left, "Talk", WalkAction::talk, true);
      drawButton(g, rightX, bottomRow, w - inset - rightX, "Name corner", WalkAction::nameCorner);
    } else {
      drawButton(g, inset, bottomRow, full, "Set up voice via USB", WalkAction::none);
    }
  }
  if (frame.getBuffer()) frame.pushSprite(0, 0);
}

// Read the LCD itself over local USB to diagnose a reported layout issue.
bool walkScreenCommand(const char* cmd) {
  if (strcmp(cmd, "UI SCREEN")) return false;
  size_t length = 0;
  auto* png = static_cast<uint8_t*>(M5.Display.createPng(&length, 0, 0, M5.Display.width(), M5.Display.height()));
  if (png && length) {
    Serial.printf("PNG %u\n", (unsigned)length);
    Serial.write(png, length);
    Serial.println();
  } else Serial.println("ERROR screen capture");
  free(png);
  return true;
}
