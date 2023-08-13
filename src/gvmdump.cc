#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Image.hh>
#include <phosg/Strings.hh>
#include <string>

using namespace std;

struct GVMFileEntry {
  be_uint16_t file_num;
  char name[28];
  be_uint32_t unknown[2];
} __attribute__((packed));

struct GVMFileHeader {
  be_uint32_t magic; // 'GVMH'
  // Note: Add 8 to this value (it doesn't include magic and size). Also, yes,
  // it really is little-endian.
  le_uint32_t header_size;
  be_uint16_t flags;
  be_uint16_t num_files;
  GVMFileEntry entries[0];
} __attribute__((packed));

// Note: most of these formats are named after those in puyotools but are
// currently unimplemented here
enum GVRColorTablePixelFormat {
  INTENSITY_A8 = 0x00,
  RGB565 = 0x10,
  RGB5A3 = 0x20,
  MASK = 0xF0,
};

enum GVRDataFlag {
  HAS_MIPMAPS = 0x01,
  HAS_EXTERNAL_COLOR_TABLE = 0x02,
  HAS_INTERNAL_COLOR_TABLE = 0x08,
  DATA_FLAG_MASK = 0x0F,
};

enum class GVRDataFormat : uint8_t {
  INTENSITY_4 = 0x00,
  INTENSITY_8 = 0x01,
  INTENSITY_A4 = 0x02,
  INTENSITY_A8 = 0x03,
  RGB565 = 0x04,
  RGB5A3 = 0x05,
  ARGB8888 = 0x06,
  INDEXED_4 = 0x08,
  INDEXED_8 = 0x09,
  DXT1 = 0x0E,
};

struct GVRHeader {
  be_uint32_t magic; // 'GVRT'
  // See comment in GVMFileHeader about header_size - data_size behaves the same
  // way here.
  le_uint32_t data_size;
  be_uint16_t unknown;
  uint8_t format_flags; // High 4 bits are pixel format, low 4 are data flags
  GVRDataFormat data_format;
  be_uint16_t width;
  be_uint16_t height;
} __attribute__((packed));

struct GVPHeader {
  be_uint32_t magic; // 'GVPL'
  // See comment in GVMFileHeader about header_size - data_size behaves the same
  // way here.
  le_uint32_t data_size;
  uint8_t unknown_a1;
  uint8_t entry_format; // 0 = A8, 1 = RGB565, 2 = RGB5A3
  uint8_t unknown_a2[4];
  be_uint16_t num_entries;
} __attribute__((packed));

uint32_t decode_rgb5a3(uint16_t c) {
  if (c & 0x8000) { // RGB555
    //                 1rrrrrgggggbbbbb
    // rrrrrrrrggggggggbbbbbbbbaaaaaaaa
    return ((c << 17) & 0xF8000000) | ((c << 12) & 0x07000000) | // R
        ((c << 14) & 0x00F80000) | ((c << 9) & 0x00070000) | // G
        ((c << 11) & 0x0000F800) | ((c << 6) & 0x00000700) | // B
        0x000000FF; // A
  } else { // ARGB3444
    //                 0aaarrrrggggbbbb
    // rrrrrrrrggggggggbbbbbbbbaaaaaaaa
    return ((c << 20) & 0xF0000000) | // R high
        ((c << 16) & 0x0FF00000) | // R low and G high
        ((c << 12) & 0x000FF000) | // G low and B high
        ((c << 8) & 0x00000F00) | // B low
        ((c >> 7) & 0x000000E0) | ((c >> 10) & 0x0000001C) | ((c >> 13) & 0x00000003); // A
  }
}

uint32_t decode_rgb565(uint16_t c) {
  //                 rrrrrggggggbbbbb
  // rrrrrrrrggggggggbbbbbbbbaaaaaaaa
  return ((c << 16) & 0xF8000000) | ((c << 11) & 0x07000000) | // R
      ((c << 13) & 0x00FC0000) | ((c << 7) & 0x00030000) | // G
      ((c << 11) & 0x0000F800) | ((c << 6) & 0x00000700) | // B
      0x000000FF; // A
}

vector<uint32_t> decode_gvp(const string& data) {
  StringReader r(data.data(), data.size());
  auto header = r.get<GVPHeader>();
  if (header.magic != 0x4756504C) {
    throw runtime_error("GVPL signature is missing");
  }

  vector<uint32_t> ret;
  ret.reserve(header.num_entries);
  while (ret.size() < header.num_entries) {
    switch (header.entry_format) {
      case 0: {
        uint8_t a = r.get_u8();
        ret.emplace_back((a << 24) | (a << 16) | (a << 8) | a);
        break;
      }
      case 1:
        ret.emplace_back(decode_rgb565(r.get_u16b()));
        break;
      case 2:
        ret.emplace_back(decode_rgb5a3(r.get_u16b()));
        break;
      default:
        throw runtime_error("unknown color table entry format");
    }
  }

  return ret;
}

Image decode_gvr(const string& data, const vector<uint32_t>* clut = nullptr) {
  if (data.size() < sizeof(GVRHeader)) {
    throw runtime_error("data too small for header");
  }

  StringReader r(data.data(), data.size());
  GVRHeader header = r.get<GVRHeader>();
  if (header.magic != 0x47565254) {
    throw runtime_error("GVRT signature is missing");
  }
  if (data.size() < header.data_size + 8) {
    throw runtime_error("data size is too small");
  }

  // TODO: deal with GBIX if needed

  // TODO: deal with color table if needed. If present, the color table
  // immediately follows the header and precedes the data
  if ((header.data_format == GVRDataFormat::INDEXED_4) ||
      (header.data_format == GVRDataFormat::INDEXED_8)) {
    if (header.format_flags & GVRDataFlag::HAS_EXTERNAL_COLOR_TABLE) {
      if (!clut) {
        throw runtime_error("a color table is required");
      }
    } else if (header.format_flags & GVRDataFlag::HAS_INTERNAL_COLOR_TABLE) {
      throw logic_error("internal color tables not implemented");
    }
  }

  if (header.format_flags & GVRDataFlag::HAS_MIPMAPS) {
    fprintf(stderr, "Note: image has mipmaps; ignoring them\n");

    /* TODO: deal with mipmaps properly
    if (header.width != header.height) {
      throw runtime_error("mipmapped texture is not square");
    }
    if (header.width & (header.width - 1) == 0) {
      throw runtime_error("mipmapped texture has non-power-of-two dimensions")
    }

    vector<size_t> image_offsets;
    image_offsets.emplace_back(sizeof(GVRHeader));

    size_t offset = sizeof(GVMHeader); // this will be wrong if there's a color table
    // the 0 is probably wrong in this loop; figure out the right value
    for (size_t width = header.width; width > 0; width >>= 1) {
      offset += max(size * size * (bpp >> 3), 32);
      image_offsets.emplace_back(offset);
    }
    */
  }

  // For DXT1, w/h must be multiples of 4
  if ((header.data_format == GVRDataFormat::DXT1) &&
      ((header.width & 3) || (header.height & 3))) {
    throw runtime_error("width/height must be multiples of 4 for dxt1 format");
  }

  Image result(header.width, header.height, true);
  switch (header.data_format) {
    case GVRDataFormat::RGB5A3:
      // 4x4 blocks of pixels
      for (size_t y = 0; y < header.height; y += 4) {
        for (size_t x = 0; x < header.width; x += 4) {
          for (size_t yy = 0; yy < 4; yy++) {
            for (size_t xx = 0; xx < 4; xx++) {
              result.write_pixel(x + xx, y + yy, decode_rgb5a3(r.get_u16b()));
            }
          }
        }
      }
      break;

    case GVRDataFormat::INDEXED_4:
      if (!clut) {
        throw runtime_error("a color table is required");
      }
      // 4x4 blocks of pixels
      for (size_t y = 0; y < header.height; y += 8) {
        for (size_t x = 0; x < header.width; x += 8) {
          for (size_t yy = 0; yy < 8; yy++) {
            for (size_t xx = 0; xx < 8; xx += 2) {
              uint8_t indexes = r.get_u8();
              result.write_pixel(x + xx, y + yy, clut->at((indexes >> 4) & 0x0F));
              result.write_pixel(x + xx + 1, y + yy, clut->at(indexes & 0x0F));
            }
          }
        }
      }
      break;

    case GVRDataFormat::INDEXED_8:
      if (!clut) {
        throw runtime_error("a color table is required");
      }
      // 4x4 blocks of pixels
      for (size_t y = 0; y < header.height; y += 4) {
        for (size_t x = 0; x < header.width; x += 8) {
          for (size_t yy = 0; yy < 4; yy++) {
            for (size_t xx = 0; xx < 8; xx++) {
              uint8_t index = r.get_u8();
              result.write_pixel(x + xx, y + yy, clut->at(index));
            }
          }
        }
      }
      break;

    case GVRDataFormat::INTENSITY_4:
      // 4x4 blocks of pixels
      for (size_t y = 0; y < header.height; y += 8) {
        for (size_t x = 0; x < header.width; x += 8) {
          for (size_t yy = 0; yy < 8; yy++) {
            for (size_t xx = 0; xx < 8; xx += 2) {
              uint8_t v = r.get_u8();
              uint8_t v1 = (v & 0xF0) | (v >> 4);
              uint8_t v2 = (v & 0x0F) | (v << 4);
              result.write_pixel(x + xx, y + yy, (v1 << 24) | (v1 << 16) | (v1 << 8) | 0xFF);
              result.write_pixel(x + xx + 1, y + yy, (v2 << 24) | (v2 << 16) | (v2 << 8) | 0xFF);
            }
          }
        }
      }
      break;

    case GVRDataFormat::INTENSITY_8:
      // 4x4 blocks of pixels
      for (size_t y = 0; y < header.height; y += 4) {
        for (size_t x = 0; x < header.width; x += 8) {
          for (size_t yy = 0; yy < 4; yy++) {
            for (size_t xx = 0; xx < 8; xx++) {
              uint8_t v = r.get_u8();
              result.write_pixel(x + xx, y + yy, (v << 24) | (v << 16) | (v << 8) | 0xFF);
            }
          }
        }
      }
      break;

    case GVRDataFormat::DXT1:
      for (size_t y = 0; y < header.height; y += 8) {
        for (size_t x = 0; x < header.width; x += 8) {
          for (size_t yy = 0; yy < 8; yy += 4) {
            for (size_t xx = 0; xx < 8; xx += 4) {
              uint8_t color_table[4][4]; // 4 entries of [r, g, b, a] each
              uint16_t color1 = r.get_u16b(); // RGB565
              uint16_t color2 = r.get_u16b(); // RGB565
              color_table[0][0] = ((color1 >> 8) & 0xF8) | ((color1 >> 13) & 0x07);
              color_table[0][1] = ((color1 >> 3) & 0xFC) | ((color1 >> 9) & 0x03);
              color_table[0][2] = ((color1 << 3) & 0xF8) | ((color1 >> 2) & 0x07);
              color_table[0][3] = 0xFF;
              color_table[1][0] = ((color2 >> 8) & 0xF8) | ((color2 >> 13) & 0x07);
              color_table[1][1] = ((color2 >> 3) & 0xFC) | ((color2 >> 9) & 0x03);
              color_table[1][2] = ((color2 << 3) & 0xF8) | ((color2 >> 2) & 0x07);
              color_table[1][3] = 0xFF;
              if (color1 > color2) {
                color_table[2][0] = (((color_table[0][0] * 2) + color_table[1][0]) / 3);
                color_table[2][1] = (((color_table[0][1] * 2) + color_table[1][1]) / 3);
                color_table[2][2] = (((color_table[0][2] * 2) + color_table[1][2]) / 3);
                color_table[2][3] = 0xFF;
                color_table[3][0] = (((color_table[1][0] * 2) + color_table[0][0]) / 3);
                color_table[3][1] = (((color_table[1][1] * 2) + color_table[0][1]) / 3);
                color_table[3][2] = (((color_table[1][2] * 2) + color_table[0][2]) / 3);
                color_table[3][3] = 0xFF;
              } else {
                color_table[2][0] = ((color_table[0][0] + color_table[1][0]) / 2);
                color_table[2][1] = ((color_table[0][1] + color_table[1][1]) / 2);
                color_table[2][2] = ((color_table[0][2] + color_table[1][2]) / 2);
                color_table[2][3] = 0xFF;
                color_table[3][0] = 0x00;
                color_table[3][1] = 0x00;
                color_table[3][2] = 0x00;
                color_table[3][3] = 0x00;
              }

              for (size_t yyy = 0; yyy < 4; yyy++) {
                uint8_t pixels = r.get_u8();
                for (size_t xxx = 0; xxx < 4; xxx++) {
                  size_t effective_x = x + xx + xxx;
                  size_t effective_y = y + yy + yyy;
                  uint8_t color_index = (pixels >> (6 - (xxx * 2))) & 3;
                  result.write_pixel(effective_x, effective_y,
                      color_table[color_index][0], color_table[color_index][1],
                      color_table[color_index][2], color_table[color_index][3]);
                }
              }
            }
          }
        }
      }
      break;

    default:
      throw logic_error(string_printf(
          "unimplemented data format: %02hhX",
          static_cast<uint8_t>(header.data_format)));
  }

  return result;
}

int main(int argc, char* argv[]) {
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "Usage: %s <filename.gvm|gvr> [color_table.gvp]\n", argv[0]);
    return 1;
  }

  string data = load_file(argv[1]);
  if (data.size() < 8) {
    fprintf(stderr, "file is too small\n");
    return 2;
  }

  vector<uint32_t> clut;
  if (argc == 3) {
    string clut_data = load_file(argv[2]);
    clut = decode_gvp(clut_data);
  }

  uint32_t magic = *reinterpret_cast<const be_uint32_t*>(data.data());
  if ((magic == 0x47565254) || (magic == 0x47424958)) { // GVRT or GBIX
    if (magic == 0x47424958) { // GBIX
      uint32_t gbix_size = *reinterpret_cast<const uint32_t*>(data.data() + 4);
      data = data.substr(gbix_size + 8); // strip off GBIX header
    }
    try {
      Image decoded = decode_gvr(data, clut.empty() ? nullptr : &clut);
      decoded.save(string(argv[1]) + ".bmp", Image::Format::WINDOWS_BITMAP);
    } catch (const exception& e) {
      fprintf(stderr, "failed to decode gvr: %s\n", e.what());
      return 2;
    }

  } else if (magic == 0x47564D48) { // GVMH
    const GVMFileHeader* gvm = reinterpret_cast<const GVMFileHeader*>(data.data());
    if (data.size() < sizeof(GVMFileHeader)) {
      fprintf(stderr, "gvm file is too small\n");
      return 2;
    }
    if (gvm->magic != 0x47564D48) {
      fprintf(stderr, "warning: gvm header may be corrupt\n");
    }

    fprintf(stderr, "%s: %hu files\n", argv[1], gvm->num_files.load());
    size_t offset = gvm->header_size + 8;
    for (size_t x = 0; x < gvm->num_files; x++) {
      string filename = argv[1];
      filename += '_';
      for (const char* ch = gvm->entries[x].name; *ch; ch++) {
        if (*ch < 0x20 || *ch > 0x7E) {
          filename += string_printf("_x%02hhX", *ch);
        } else {
          filename += *ch;
        }
      }
      filename += ".gvr";

      const GVRHeader* gvr = reinterpret_cast<const GVRHeader*>(data.data() + offset);
      if (gvr->magic != 0x47565254) {
        fprintf(stderr, "warning: gvr header may be corrupt\n");
      }

      string gvr_contents = data.substr(offset, gvr->data_size + 8);
      try {
        Image decoded = decode_gvr(gvr_contents, clut.empty() ? nullptr : &clut);
        decoded.save(filename + ".bmp", Image::Format::WINDOWS_BITMAP);
        printf("> %04zu = %08zX:%08X => %s.bmp\n",
            x + 1, offset, gvr->data_size + 8, filename.c_str());
      } catch (const exception& e) {
        fprintf(stderr, "failed to decode gvr: %s\n", e.what());
      }

      printf("> %04zu = %08zX:%08X => %s\n",
          x + 1, offset, gvr->data_size + 8, filename.c_str());
      save_file(filename, gvr_contents);
      offset += (gvr->data_size + 8);
    }

  } else {
    fprintf(stderr, "file signature is incorrect\n");
    return 2;
  }

  return 0;
}
