#include "afc.hh"

#include <inttypes.h>

using namespace std;



vector<int16_t> afc_decode(const void* data, size_t size, bool small_frames) {
  static const int16_t coef[16][2] = {
    { 0x0000,  0x0000},
    { 0x0800,  0x0000},
    { 0x0000,  0x0800},
    { 0x0400,  0x0400},
    { 0x1000, -0x0800},
    { 0x0E00, -0x0600},
    { 0x0C00, -0x0400},
    { 0x1200, -0x0A00},
    { 0x1068, -0x08C8},
    { 0x12C0, -0x08FC},
    { 0x1400, -0x0C00},
    { 0x0800, -0x0800},
    { 0x0400, -0x0400},
    {-0x0400,  0x0400},
    {-0x0400,  0x0000},
    {-0x0800,  0x0000}};

  size_t frame_size = small_frames ? 5 : 9;
  if (size % frame_size != 0) {
    throw invalid_argument("input size is not a multiple of frame size");
  }

  size_t frame_count = size / frame_size;
  size_t output_sample_count = frame_count * 16;

  int16_t history[2] = {0, 0};
  vector<int16_t> output_samples(output_sample_count, 0);
  for (size_t frame_index = 0; frame_index < frame_count; frame_index++) {
    const int8_t* frame_data = reinterpret_cast<const int8_t*>(data) +
        (frame_index * frame_size);

    int16_t delta = 1 << ((frame_data[0] >> 4) & 0x0F);
    int16_t coef_table_index = frame_data[0] & 0x0F;

    int16_t nibbles[16];
    if (!small_frames) {
      for (size_t x = 0; x < 8; x++) {
        nibbles[2 * x + 0] = (frame_data[x + 1] >> 4) & 0x0F;
        nibbles[2 * x + 1] = (frame_data[x + 1] >> 0) & 0x0F;
      }
      for (size_t x = 0; x < 16; x++) {
        if (nibbles[x] >= 8) {
          nibbles[x] = nibbles[x] - 16;
        }
        nibbles[x] <<= 11;
      }
    } else {
      for (size_t x = 0; x < 4; x++) {
        nibbles[4 * x + 0] = (frame_data[x + 1] >> 6) & 0x03;
        nibbles[4 * x + 1] = (frame_data[x + 1] >> 4) & 0x03;
        nibbles[4 * x + 2] = (frame_data[x + 1] >> 2) & 0x03;
        nibbles[4 * x + 3] = (frame_data[x + 1] >> 0) & 0x03;
      }
      for (size_t x = 0; x < 16; x++) {
        if (nibbles[x] >= 2) {
          nibbles[x] = nibbles[x] - 4;
        }
        nibbles[x] <<= 13;
      }
    }

    for (size_t x = 0; x < 16; x++) {
      int32_t sample = delta * nibbles[x] +
          (static_cast<int32_t>(history[0]) * coef[coef_table_index][0]) +
          (static_cast<int32_t>(history[1]) * coef[coef_table_index][1]);
      sample >>= 11;
      if (sample > 0x7FFF) {
        sample = 0x7FFF;
      }
      if (sample < -0x8000) {
        sample = -0x8000;
      }
      output_samples[16 * frame_index + x] = static_cast<int16_t>(sample);
      history[1] = history[0];
      history[0] = static_cast<int16_t>(sample);
    }
  }

  return output_samples;
}
