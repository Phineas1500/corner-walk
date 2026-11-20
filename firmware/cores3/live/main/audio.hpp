#pragma once
#include <esp_err.h>
#include <stdint.h>
#include <stddef.h>
namespace audio {
esp_err_t open(void*);
void close(void*);
esp_err_t read(void*, int16_t*, size_t);
esp_err_t write(void*, const int16_t*, size_t);
}
