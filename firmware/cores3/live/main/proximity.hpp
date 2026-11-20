#pragma once
#include <stddef.h>
#include <stdint.h>
void proximityBegin();
bool proximityCommand(char* command);
bool proximityTriggered();
void proximityToggle();
void setAiming(bool enabled);
bool isAiming();
bool autoPhotoEnabled();
const char* stickStatus();
const uint8_t* proximityImage(size_t& length);
bool captureTriggerPhoto();
size_t triggerPhotoSize();
void clearTriggerPhoto();
const char* triggerOpener();
