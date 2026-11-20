#pragma once

enum class WalkView { home, aiming, chat };
enum class WalkAction {
  none, toggleAuto, aim, done, talk, nameCorner, endChat, startWalk, startIndoor, nextStop, endWalk
};

struct WalkScreenState {
  WalkView view;
  bool voiceReady;
  bool namingCorner;
  unsigned long corners, photos;
  const char* status;
  const char* caption;
};

void drawWalkScreen(const WalkScreenState& state);
WalkAction walkActionAt(int x, int y);
bool walkScreenCommand(const char* command);
