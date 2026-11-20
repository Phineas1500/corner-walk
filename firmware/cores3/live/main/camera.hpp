#pragma once
#include <esp_err.h>
#include <cstddef>
#include <cstdint>

esp_err_t captureWalkPhoto(void*, uint8_t* jpeg, size_t capacity, size_t* length);
