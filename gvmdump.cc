#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Image.hh>
#include <phosg/Strings.hh>
#include <string>

using namespace std;



struct GVMFileEntry {
  uint16_t file_num;
  char name[28];
  uint32_t unknown[2];

  void byteswap() {
    this->file_num = bswap16(this->file_num);
  }
} __attribute__((packed));

struct GVMFileHeader {
  uint32_t magic; // GVMH
  uint32_t header_size; // add 8 to this value (doesn't include magic/size)
  uint16_t unknown;
  uint16_t num_files;
  GVMFileEntry entries[0];

  void byteswap() {
    this->magic = bswap32(this->magic);
    // note: header_size is apparently little-endian
    this->num_files = bswap16(this->num_files);
    for (size_t x = 0; x < this->num_files; x++) {
      this->entries[x].byteswap();
    }
  }
} __attribute__((packed));



// note: most of these formats are named after those in puyotools but are
// currently unimplemented here
enum GVRColorTablePixelFormat {
  IntensityA8ColorTablePixelFormat = 0x00,
  RGB565ColorTablePixelFormat      = 0x10,
  RGB5A3ColorTablePixelFormat      = 0x20,
  ColorTablePixelFormatMask        = 0xF0,
};

enum GVRDataFlag {
  HasMipmaps            = 0x01,
  HasExternalColorTable = 0x02,
  HasInternalColorTable = 0x08,
  DataFlagMask          = 0x0F,
};

enum GVRDataFormat {
  Intensity4DataFormat  = 0x00,
  Intensity8DataFormat  = 0x01,
  IntensityA4DataFormat = 0x02,
  IntensityA8DataFormat = 0x03,
  RGB565DataFormat      = 0x04,
  RGB5A3DataFormat      = 0x05,
  ARGB8888DataFormat    = 0x06,
  Indexed4DataFormat    = 0x08,
  Indexed8DataFormat    = 0x09,
  DXT1DataFormat        = 0x0E,
};

struct GVRHeader {
  uint32_t magic;
  uint32_t data_size; // add 8 (doesn't include magic and data_size itself)
  uint16_t unknown;
  uint8_t format_flags; // high 4 bits are pixel format, low 4 are data flags
  uint8_t data_format;
  uint16_t width;
  uint16_t height;

  void byteswap() {
    this->magic = bswap32(this->magic);
    // note: data_size is apparently little-endian
    this->width = bswap16(this->width);
    this->height = bswap16(this->height);
  }
} __attribute__((packed));

Image decode_gvr(const string& data) {
  if (data.size() < sizeof(GVRHeader)) {
    throw runtime_error("data too small for header");
  }

  StringReader r(data.data(), data.size());
  GVRHeader header = r.get<GVRHeader>();
  header.byteswap();
  if (header.magic != 0x47565254) {
    throw runtime_error("GVRT signature is missing");
  }
  if (data.size() < header.data_size + 8) {
    throw runtime_error("data size is too small");
  }

  // TODO: deal with GBIX if needed

  // TODO: deal with color table if needed. if present, the color table
  // immediately follows the header and precedes the data
  if ((header.data_format == Indexed4DataFormat) || (header.data_format == Indexed8DataFormat)) {
    if (header.format_flags & HasInternalColorTable) {
      throw logic_error("internal color tables not implemented");
    }
    if (header.format_flags & HasInternalColorTable) {
      throw logic_error("external color tables not implemented");
    }
  }

  // TODO: deal with mipmaps
  if (header.format_flags & HasMipmaps) {
    fprintf(stderr, "note: image has mipmaps; ignoring them\n");

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

  // for DXT1, w/h must be multiples of 4
  if ((header.data_format == DXT1DataFormat) && ((header.width & 3) || (header.height & 3))) {
    throw runtime_error("width/height must be multiples of 4 for dxt1 format");
  }

  Image result(header.width, header.height, true);
  switch (header.data_format) {
    case RGB5A3DataFormat: {
      // 4x4 blocks of rgb555 or argb344
      for (size_t y = 0; y < header.height; y += 4) {
        for (size_t x = 0; x < header.width; x += 4) {
          for (size_t yy = 0; yy < 4; yy++) {
            for (size_t xx = 0; xx < 4; xx++) {
              uint16_t pixel = r.get_u16r();
              if (pixel & 0x8000) { // rgb555
                result.write_pixel(x + xx, y + yy,
                    ((pixel >> 7) & 0xF8) | ((pixel >> 12) & 7),
                    ((pixel >> 2) & 0xF8) | ((pixel >> 7) & 7),
                    ((pixel << 3) & 0xF8) | ((pixel >> 2) & 7), 0xFF);
              } else { // argb3444
                result.write_pixel(x + xx, y + yy,
                    ((pixel >> 4) & 0xF0) | ((pixel >> 8) & 0x0F),
                    ((pixel >> 0) & 0xF0) | ((pixel >> 4) & 0x0F),
                    ((pixel << 4) & 0xF0) | ((pixel >> 0) & 0x0F),
                    ((pixel >> 7) & 0xE0) | ((pixel >> 10) & 0x1C) | ((pixel >> 13) & 0x03));
              }
            }
          }
        }
      }
      break;
    }

    case DXT1DataFormat: {
      for (size_t y = 0; y < header.height; y += 8) {
        for (size_t x = 0; x < header.width; x += 8) {
          for (size_t yy = 0; yy < 8; yy += 4) {
            for (size_t xx = 0; xx < 8; xx += 4) {
              uint8_t color_table[4][4]; // 4 entries of [r, g, b, a] each
              uint16_t color1 = r.get_u16r(); // rgb565
              uint16_t color2 = r.get_u16r(); // rgb565
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
    }
    default:
      throw logic_error(string_printf("unimplemented data format: %02hhX", header.data_format));
  }

  return result;
}



int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <filename>\n", argv[0]);
    return 1;
  }

  string data = load_file(argv[1]);
  if (data.size() < 8) {
    fprintf(stderr, "file is too small\n");
    return 2;
  }

  uint32_t magic = bswap32(*reinterpret_cast<const uint32_t*>(data.data()));
  if ((magic == 0x47565254) || (magic == 0x47424958)) { // GVRT or GBIX
    if (magic == 0x47424958) { // GBIX
      uint32_t gbix_size = *reinterpret_cast<const uint32_t*>(data.data() + 4);
      data = data.substr(gbix_size + 8); // strip off GBIX header
    }
    try {
      Image decoded = decode_gvr(data);
      decoded.save(string(argv[1]) + ".bmp", Image::ImageFormat::WindowsBitmap);
    } catch (const exception& e) {
      fprintf(stderr, "failed to decode gvr: %s\n", e.what());
      return 2;
    }

  } else if (magic == 0x47564D48) { // GVMH
    GVMFileHeader* gvm = reinterpret_cast<GVMFileHeader*>(const_cast<char*>(data.data()));
    if (data.size() < sizeof(GVMFileHeader)) {
      fprintf(stderr, "gvm file is too small\n");
      return 2;
    }
    gvm->byteswap();
    if (gvm->magic != 0x47564D48) {
      fprintf(stderr, "warning: gvm header may be corrupt\n");
    }

    fprintf(stderr, "%s: %hu files\n", argv[1], gvm->num_files);
    size_t offset = gvm->header_size + 8;
    for (size_t x = 0; x < gvm->num_files; x++) {
      string filename = argv[1];
      filename += '_';
      for (char* ch = gvm->entries[x].name; *ch; ch++) {
        if (*ch < 0x20 || *ch > 0x7E) {
          filename += string_printf("_x%02hhX", *ch);
        } else {
          filename += *ch;
        }
      }
      filename += ".gvr";

      struct GVRHeader gvr;
      memcpy(&gvr, data.data() + offset, sizeof(GVRHeader));
      gvr.byteswap();
      if (gvr.magic != 0x47565254) {
        fprintf(stderr, "warning: gvr header may be corrupt\n");
      }

      string gvr_contents = data.substr(offset, gvr.data_size + 8);
      try {
        Image decoded = decode_gvr(gvr_contents);
        decoded.save(filename + ".bmp", Image::ImageFormat::WindowsBitmap);
        printf("> %04zu = %08zX:%08X => %s.bmp\n", x + 1, offset, gvr.data_size + 8, filename.c_str());
      } catch (const exception& e) {
        fprintf(stderr, "failed to decode gvr: %s\n", e.what());
      }

      printf("> %04zu = %08zX:%08X => %s\n", x + 1, offset, gvr.data_size + 8, filename.c_str());
      save_file(filename, gvr_contents);
      offset += (gvr.data_size + 8);
    }

  } else {
    fprintf(stderr, "file signature is incorrect\n");
    return 2;
  }
  
  return 0;
}
