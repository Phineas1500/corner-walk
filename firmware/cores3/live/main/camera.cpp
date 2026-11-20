#include "camera.hpp"
#include <M5Unified.h>
#include <esp_camera.h>
#include <img_converters.h>
#include <esp_timer.h>
#include <cstring>

namespace {
constexpr uint8_t gc0308 = 0x21;
constexpr uint32_t busHz = 400000;

camera_config_t cameraPins() {
  camera_config_t cfg{};
  cfg.pin_pwdn = cfg.pin_reset = cfg.pin_xclk = -1;
  cfg.pin_sccb_sda = cfg.pin_sccb_scl = -1;
  cfg.sccb_i2c_port = M5.In_I2C.getPort();
  cfg.pin_d0 = 39; cfg.pin_d1 = 40; cfg.pin_d2 = 41; cfg.pin_d3 = 42;
  cfg.pin_d4 = 15; cfg.pin_d5 = 16; cfg.pin_d6 = 48; cfg.pin_d7 = 47;
  cfg.pin_vsync = 46; cfg.pin_href = 38; cfg.pin_pclk = 45;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_RGB565;
  cfg.frame_size = FRAMESIZE_QVGA;
  cfg.fb_count = 1;
  cfg.fb_location = CAMERA_FB_IN_PSRAM;
  cfg.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  return cfg;
}

struct JpegBuffer {
  uint8_t* bytes;
  size_t capacity;
  size_t size = 0;
  bool overflow = false;
};

size_t writeJpeg(void* context, size_t index, const void* data, size_t len) {
  auto& buffer = *static_cast<JpegBuffer*>(context);
  if (index > buffer.capacity || len > buffer.capacity - index) buffer.overflow = true;
  if (!buffer.overflow && len) {
    memcpy(buffer.bytes + index, data, len);
    buffer.size = index + len;
  }
  return len;
}
}

// M5Unified already owns the control bus for touch, power and audio. The GC0308
// uses 8-bit registers. Route its SCCB transactions through that same bus.
extern "C" {
int __wrap_SCCB_Init(int, int) { return ESP_ERR_NOT_SUPPORTED; }
int __wrap_SCCB_Use_Port(int port) {
  return port == M5.In_I2C.getPort() ? ESP_OK : ESP_ERR_INVALID_ARG;
}
int __wrap_SCCB_Deinit() { return ESP_OK; }
int __wrap_SCCB_Probe(uint8_t addr) {
  uint8_t id = 0;
  return addr == gc0308 && M5.In_I2C.readRegister(addr, 0, &id, 1, busHz) && id == 0x9b
      ? ESP_OK : ESP_ERR_NOT_FOUND;
}
uint8_t __wrap_SCCB_Read(uint8_t addr, uint8_t reg) {
  return M5.In_I2C.readRegister8(addr, reg, busHz);
}
int __wrap_SCCB_Write(uint8_t addr, uint8_t reg, uint8_t value) {
  return M5.In_I2C.writeRegister8(addr, reg, value, busHz) ? 0 : -1;
}
}

esp_err_t captureWalkPhoto(void*, uint8_t* jpeg, size_t capacity, size_t* length) {
  if (!jpeg || !length || capacity < 4) return ESP_ERR_INVALID_ARG;
  *length = 0;
  if (!M5.In_I2C.bitOn(0x58, 0x03, 0x01, busHz)) return ESP_FAIL;
  const auto cfg = cameraPins();
  vTaskDelay(pdMS_TO_TICKS(15));
  auto err = esp_camera_init(&cfg);
  if (err == ESP_OK) {
    const auto deadline = esp_timer_get_time() + 1000000;
    err = ESP_ERR_TIMEOUT;
    unsigned frames = 0;
    while (esp_timer_get_time() < deadline) {
      if (!esp_camera_available_frames()) {
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      auto* frame = esp_camera_fb_get();
      if (!frame) break;
      if (++frames >= 4) {
        err = ESP_ERR_INVALID_SIZE;
        if (frame->width == 320 && frame->height == 240 && frame->len == 320 * 240 * 2) {
          for (uint8_t quality : {65, 45, 25, 12}) {
            JpegBuffer output{jpeg, capacity};
            if (frame2jpg_cb(frame, quality, writeJpeg, &output) && !output.overflow &&
                output.size >= 4 && jpeg[0] == 0xff && jpeg[1] == 0xd8 &&
                jpeg[output.size - 2] == 0xff && jpeg[output.size - 1] == 0xd9) {
              *length = output.size;
              err = ESP_OK;
              break;
            }
          }
        }
      }
      esp_camera_fb_return(frame);
      if (frames >= 4) break;
    }
    esp_camera_deinit();
  }
  M5.In_I2C.bitOff(0x58, 0x03, 0x01, busHz);
  return err;
}
