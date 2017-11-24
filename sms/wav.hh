#include <inttypes.h>
#include <stddef.h>

#include <vector>


struct wav_header {
  uint32_t riff_magic;   // 0x52494646
  uint32_t file_size;    // size of file - 8
  uint32_t wave_magic;   // 0x57415645

  uint32_t fmt_magic;    // 0x666d7420
  uint32_t fmt_size;     // 16
  uint16_t format;       // 1 = PCM
  uint16_t num_channels;
  uint32_t sample_rate;
  uint32_t byte_rate;    // num_channels * sample_rate * bits_per_sample / 8
  uint16_t block_align;  // num_channels * bits_per_sample / 8
  uint16_t bits_per_sample;

  uint32_t data_magic;   // 0x64617461
  uint32_t data_size;    // num_samples * num_channels * bits_per_sample / 8

  wav_header(uint32_t num_samples, uint16_t num_channels, uint32_t sample_rate,
      uint16_t bits_per_sample, bool is_float = false);
};

void save_wav(const char* filename, const std::vector<int16_t>& samples,
    size_t sample_rate, size_t num_channels);
void save_wav(const char* filename, const std::vector<float>& samples,
    size_t sample_rate, size_t num_channels);

std::vector<float> convert_samples_to_float(const std::vector<int16_t>&);
std::vector<int16_t> convert_samples_to_int(const std::vector<float>&);
