#include "audio.hpp"
#include <M5Unified.h>
#include <driver/i2s_std.h>
#include <algorithm>

namespace audio {
static i2s_chan_handle_t tx, rx;
static bool txOn, rxOn;
constexpr int frames = 480; // 20 ms of 24 kHz audio

static bool amplifier(uint8_t reg, uint16_t value) {
  uint8_t bytes[] = {uint8_t(value >> 8), uint8_t(value)};
  return M5.In_I2C.writeRegister(0x36, reg, bytes, sizeof(bytes), 400000);
}

void close(void*) {
  amplifier(0x04, 0x4000);
  M5.In_I2C.writeRegister8(0x40, 0x00, 0xff, 400000);
  if (rxOn) i2s_channel_disable(rx);
  if (txOn) i2s_channel_disable(tx);
  rxOn = txOn = false;
  if (rx) i2s_del_channel(rx);
  if (tx) i2s_del_channel(tx);
  rx = tx = nullptr;
}

esp_err_t open(void*) {
  // K128 ES7210 and AW88298 register settings from M5Unified's CoreS3 board tables.
  static constexpr uint8_t mic[][2] = {
    {0,0xff},{0,0x41},{1,0x1f},{6,0},{7,0x20},{8,0x10},{9,0x30},{10,0x30},
    {0x20,0x0a},{0x21,0x2a},{0x22,0x0a},{0x23,0x2a},{2,0xc1},{4,1},{5,0},
    {0x11,0x60},{0x40,0x42},{0x41,0x70},{0x42,0x70},{0x43,0x1b},{0x44,0x1b},
    {0x45,0},{0x46,0},{0x47,0},{0x48,0},{0x49,0},{0x4a,0},{0x4b,0},{0x4c,0xff},{1,0x14}
  };
  if (!M5.In_I2C.bitOn(0x58, 0x02, 0x04, 400000)) return ESP_FAIL;
  for (auto &r : mic)
    if (!M5.In_I2C.writeRegister8(0x40, r[0], r[1], 400000)) return ESP_FAIL;
  i2s_chan_config_t bus = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  bus.dma_desc_num = 6;
  bus.dma_frame_num = frames;
  bus.auto_clear = true;
  auto err = i2s_new_channel(&bus, &tx, &rx);
  i2s_std_config_t fmt{};
  fmt.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000);
  fmt.slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  fmt.gpio_cfg = {GPIO_NUM_0, GPIO_NUM_34, GPIO_NUM_33, GPIO_NUM_13, GPIO_NUM_14, {}};
  if (err == ESP_OK) err = i2s_channel_init_std_mode(tx, &fmt);
  if (err == ESP_OK) err = i2s_channel_init_std_mode(rx, &fmt);
  if (err == ESP_OK) txOn = (err = i2s_channel_enable(tx)) == ESP_OK;
  if (err == ESP_OK) rxOn = (err = i2s_channel_enable(rx)) == ESP_OK;
  if (err == ESP_OK && !(amplifier(0x61, 0x0673) && amplifier(0x04, 0x4040) &&
                         amplifier(0x05, 0x0008) && amplifier(0x06, 0x14c5) &&
                         amplifier(0x0c, 0x0064))) err = ESP_FAIL;
  if (err != ESP_OK) close(nullptr);
  return err;
}

esp_err_t read(void*, int16_t* pcm, size_t count) {
  if (!rxOn || count > frames) return ESP_ERR_INVALID_SIZE;
  int16_t stereo[frames * 2];
  size_t got = 0;
  auto err = i2s_channel_read(rx, stereo, count * 4, &got, 250);
  if (err != ESP_OK || got != count * 4) return ESP_FAIL;
  for (size_t i=0; i<count; ++i) pcm[i] = stereo[2*i];
  return ESP_OK;
}

esp_err_t write(void*, const int16_t* pcm, size_t count) {
  if (!txOn) return ESP_ERR_INVALID_STATE;
  int16_t stereo[frames * 2];
  while (count) {
    size_t n = std::min<size_t>(count, frames);
    for (size_t i=0; i<n; ++i) stereo[2*i] = stereo[2*i+1] = pcm[i] / 2;
    size_t sent = 0;
    if (i2s_channel_write(tx, stereo, n * 4, &sent, 250) != ESP_OK || sent != n * 4) return ESP_FAIL;
    count -= n;
    pcm += n;
  }
  return ESP_OK;
}
}
