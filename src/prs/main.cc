#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <phosg/Filesystem.hh>
#include <string>

#include "PRS.hh"
#include "Yaz0.hh"
#include "Yay0.hh"

using namespace std;


enum class Format {
  PRS,
  YAZ0,
  YAY0,
};


void print_help() {
  fprintf(stderr, "\
Usage: prs [options] < input_file > output_file\n\
  (or use unix pipes appropriately)\n\
\n\
Options:\n\
  -h, --help: Show this message.\n\
  -d, --decompress: Decompress the input instead of compressing it.\n\
  --start-offset=N: Before decompressing, ignore this many bytes from the input\n\
      stream. Useful if the input data has an uncompressed header.\n\
  --raw-bytes=N: After ignoring any bytes requested via --start-offset but\n\
      before decompressing, copy this many bytes directly to the output stream\n\
      without compressing or decompressing.\n\
  --prs: Use Sega\'s press format (default).\n\
  --yaz0: Use Nintendo\'s Yaz0 format.\n\
  --yay0: Use Nintendo\'s Yay0 format.\n\
\n");
}


int main(int argc, char* argv[]) {
  FILE* in = stdin;
  FILE* out = stdout;
  bool decompress = false;
  size_t start_offset = 0;
  size_t raw_bytes = 0;
  Format format = Format::PRS;
  for (int x = 1; x < argc; x++) {
    if (!strcmp(argv[x], "-h") || !strcmp(argv[x], "--help")) {
      print_help();
      return 0;
    } else if (!strcmp(argv[x], "-d") || !strcmp(argv[x], "--decompress")) {
      decompress = true;
    } else if (!strncmp(argv[x], "--start-offset=", 15)) {
      start_offset = strtoull(&argv[x][15], nullptr, 0);
    } else if (!strcmp(argv[x], "--yaz0")) {
      format = Format::YAZ0;
    } else if (!strcmp(argv[x], "--yay0")) {
      format = Format::YAY0;
    } else if (!strcmp(argv[x], "--prs")) {
      format = Format::PRS;
    } else if (!strncmp(argv[x], "--raw-bytes=", 12)) {
      raw_bytes = strtoull(&argv[x][12], nullptr, 0);
    } else {
      fprintf(stderr, "prs: unknown command line option: %s\n", argv[x]);
      return 1;
    }
  }

  size_t bytes_written = 0;
  if (decompress) {
    // Skip start_offset bytes, then copy raw_bytes bytes to the output. Note
    // that we can't use fseek here because in could be a pipe.
    while (start_offset > 0) {
      size_t bytes_to_read = (start_offset > 0x4000) ? 0x4000 : start_offset;
      freadx(in, bytes_to_read);
      start_offset -= bytes_to_read;
    }
    while (raw_bytes > 0) {
      string data = freadx(in, (raw_bytes > 0x4000) ? 0x4000 : raw_bytes);
      fwritex(out, data);
      raw_bytes -= data.size();
    }

    if (format == Format::PRS) {
      bytes_written = prs_decompress_stream(in, out, 0);
    } else if (format == Format::YAZ0) {
      bytes_written = yaz0_decompress_stream(in, out, 0);
    } else if (format == Format::YAY0) {
      string in_data = read_all(in);
      string out_data = yay0_decompress(in_data.data(), in_data.size());
      fwritex(out, out_data);
      bytes_written = out_data.size();
    }

  } else {
    if (format == Format::PRS) {
      bytes_written = prs_compress_stream(in, out, -1);
    } else if (format == Format::YAZ0) {
      throw invalid_argument("yaz0 compression not supported");
    } else if (format == Format::YAY0) {
      throw invalid_argument("yay0 compression not supported");
    }
  }

  fprintf(stderr, "%zu (0x%zX) bytes written\n", bytes_written, bytes_written);
  return 0;
}
