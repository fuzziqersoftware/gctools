#include "wav.hh"

#include <inttypes.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <vector>

using namespace std;


wav_header::wav_header(uint32_t num_samples, uint16_t num_channels,
    uint32_t sample_rate, uint16_t bits_per_sample, bool is_float) {
  this->riff_magic = bswap32(0x52494646);
  this->file_size = num_samples * num_channels * bits_per_sample / 8 +
      sizeof(wav_header) - 8;
  this->wave_magic = bswap32(0x57415645);
  this->fmt_magic = bswap32(0x666d7420);
  this->fmt_size = 16;
  this->format = is_float ? 3 : 1;
  this->num_channels = num_channels;
  this->sample_rate = sample_rate;
  this->byte_rate = num_channels * sample_rate * bits_per_sample / 8;
  this->block_align = num_channels * bits_per_sample / 8;
  this->bits_per_sample = bits_per_sample;
  this->data_magic = bswap32(0x64617461);
  this->data_size = num_samples * num_channels * bits_per_sample / 8;
}

void save_wav(const char* filename, const vector<int16_t>& samples,
    size_t sample_rate, size_t num_channels) {
  wav_header header(samples.size(), num_channels, sample_rate, 16);
  auto f = fopen_unique(filename, "wb");
  fwrite(&header, sizeof(wav_header), 1, f.get());
  fwrite(samples.data(), sizeof(int16_t), samples.size(), f.get());
}

void save_wav(const char* filename, const vector<float>& samples,
    size_t sample_rate, size_t num_channels) {
  wav_header header(samples.size(), num_channels, sample_rate, 32, true);
  auto f = fopen_unique(filename, "wb");
  fwrite(&header, sizeof(wav_header), 1, f.get());
  fwrite(samples.data(), sizeof(float), samples.size(), f.get());
}

vector<float> convert_samples_to_float(const std::vector<int16_t>& samples) {
  vector<float> ret;
  ret.reserve(samples.size());
  for (int16_t sample : samples) {
    if (sample == -0x8000) {
      ret.emplace_back(-1.0f);
    } else {
      ret.emplace_back(static_cast<float>(sample) / 32767.0f);
    }
  }
  return ret;
}

vector<int16_t> convert_samples_to_int(const std::vector<float>& samples) {
  vector<int16_t> ret;
  ret.reserve(samples.size());
  for (float sample : samples) {
    if (sample >= 1.0f) {
      ret.emplace_back(0x7FFF);
    } else if (sample <= -1.0f) {
      ret.emplace_back(-0x7FFF);
    } else {
      ret.emplace_back(static_cast<int16_t>(sample * 32767.0f));
    }
  }
  return ret;
}
