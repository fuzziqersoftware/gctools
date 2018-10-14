#include "wav.hh"

#include <inttypes.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>
#include <vector>

using namespace std;


struct SaveWAVHeader {
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

  SaveWAVHeader(uint32_t num_samples, uint16_t num_channels,
      uint32_t sample_rate, uint16_t bits_per_sample, bool is_float) {
    this->riff_magic = bswap32(0x52494646);
    this->file_size = num_samples * num_channels * bits_per_sample / 8 +
        sizeof(SaveWAVHeader) - 8;
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
};

void save_wav(const char* filename, const vector<int16_t>& samples,
    size_t sample_rate, size_t num_channels) {
  SaveWAVHeader header(samples.size(), num_channels, sample_rate, 16, false);
  auto f = fopen_unique(filename, "wb");
  fwrite(&header, sizeof(SaveWAVHeader), 1, f.get());
  fwrite(samples.data(), sizeof(int16_t), samples.size(), f.get());
}

void save_wav(const char* filename, const vector<float>& samples,
    size_t sample_rate, size_t num_channels) {
  SaveWAVHeader header(samples.size(), num_channels, sample_rate, 32, true);
  auto f = fopen_unique(filename, "wb");
  fwrite(&header, sizeof(SaveWAVHeader), 1, f.get());
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



// note: this isn't the same as the structure defined at the top of this file.
// that structure is what we save wav files. when loading files, we might
// encounter chunks that this program never creates by default, but we should be
// able to handle them anyway, so the struct is split here

struct RIFFHeader {
  uint32_t riff_magic;   // 0x52494646 big-endian ('RIFF')
  uint32_t file_size;    // size of file - 8
};

struct WAVHeader {
  uint32_t wave_magic;   // 0x57415645 big-endian ('WAVE')

  uint32_t fmt_magic;    // 0x666d7420
  uint32_t fmt_size;     // 16
  uint16_t format;       // 1 = PCM, 3 = float
  uint16_t num_channels;
  uint32_t sample_rate;
  uint32_t byte_rate;    // num_channels * sample_rate * bits_per_sample / 8
  uint16_t block_align;  // num_channels * bits_per_sample / 8
  uint16_t bits_per_sample;
};

struct RIFFChunkHeader {
  uint32_t magic;
  uint32_t size;
};


WAVContents load_wav(const char* filename) {
  auto f_unique = fopen_unique(filename, "rb");
  FILE* f = f_unique.get();

  // check the RIFF header
  uint32_t magic;
  freadx(f, &magic, sizeof(uint32_t));
  if (magic != 0x46464952) {
    throw runtime_error(string_printf("unknown file format: %08" PRIX32, magic));
  }

  uint32_t file_size;
  freadx(f, &file_size, sizeof(uint32_t));

  WAVContents contents;

  // iterate over the chunks
  WAVHeader wav;
  wav.wave_magic = 0;
  for (;;) {
    RIFFChunkHeader chunk_header;
    freadx(f, &chunk_header, sizeof(RIFFChunkHeader));

    if (chunk_header.magic == 0x45564157) { // 'WAVE'
      memcpy(&wav, &chunk_header, sizeof(RIFFChunkHeader));
      freadx(f, reinterpret_cast<uint8_t*>(&wav) + sizeof(RIFFChunkHeader),
          sizeof(WAVHeader) - sizeof(RIFFChunkHeader));

      // check the header info. we only support 1-channel, 16-bit sounds for now
      if (wav.wave_magic != 0x45564157) { // 'WAVE'
        throw runtime_error(string_printf("sound has incorrect wave_magic (%" PRIX32 ")", wav.wave_magic));
      }
      if (wav.fmt_magic != 0x20746D66) { // 'fmt '
        throw runtime_error(string_printf("sound has incorrect fmt_magic (%" PRIX32 ")", wav.fmt_magic));
      }
      if (wav.num_channels > 2) {
        throw runtime_error(string_printf("sound has too many channels (%" PRIu16 ")", wav.num_channels));
      }

      contents.sample_rate = wav.sample_rate;
      contents.num_channels = wav.num_channels;

    } else if (chunk_header.magic == 0x61746164) { // 'data'
      if (wav.wave_magic == 0) {
        throw runtime_error("data chunk is before WAVE chunk");
      }

      contents.samples.resize((8 * chunk_header.size) / wav.bits_per_sample);
      contents.seconds = static_cast<float>(contents.samples.size()) / contents.sample_rate;

      // 32-bit float
      if ((wav.format == 3) && (wav.bits_per_sample == 32)) {
        freadx(f, contents.samples.data(), contents.samples.size() * sizeof(float));

      // 16-bit signed int
      } else if ((wav.format == 1) && (wav.bits_per_sample == 16)) {
        vector<int16_t> int_samples(contents.samples.size());
        freadx(f, int_samples.data(), int_samples.size() * sizeof(int16_t));
        for (size_t x = 0; x < int_samples.size(); x++) {
          if (int_samples[x] == -0x8000) {
            contents.samples[x] = -1.0f;
          } else {
            contents.samples[x] = static_cast<float>(int_samples[x]) / 32767.0f;
          }
        }

      // 8-bit unsigned int
      } else if ((wav.format == 1) && (wav.bits_per_sample == 8)) {
        vector<uint8_t> int_samples(contents.samples.size());
        freadx(f, int_samples.data(), int_samples.size() * sizeof(uint8_t));
        for (size_t x = 0; x < int_samples.size(); x++) {
          contents.samples[x] = (static_cast<float>(int_samples[x]) / 128.0f) - 1.0f;
        }
      } else {
        throw runtime_error("sample width is not supported (format=%" PRIu16 ", bits_per_sample=%" PRIu16 ")");
      }

      break;

    } else {
      fseek(f, chunk_header.size, SEEK_CUR);
    }
  }

  return contents;
}
