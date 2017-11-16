#include "wav.hh"

#include <inttypes.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <vector>

using namespace std;


wav_header::wav_header(uint32_t num_samples, uint16_t num_channels,
    uint32_t sample_rate, uint16_t bits_per_sample) {
  this->riff_magic = bswap32(0x52494646);
  this->file_size = num_samples * num_channels * bits_per_sample / 8 +
      sizeof(wav_header) - 8;
  this->wave_magic = bswap32(0x57415645);
  this->fmt_magic = bswap32(0x666d7420);
  this->fmt_size = 16;
  this->format = 1;
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
