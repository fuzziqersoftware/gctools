#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "errors.h"
#include "util.h"
#include "prs.h"
#include "yaz0.h"
#include "yay0.h"


#define FORMAT_PRS    0
#define FORMAT_YAZ0   1
#define FORMAT_YAY0   2


void print_help(const char* name) {
  fprintf(stderr, "fuzziqer software prs/yaz0/yay0 (de)compressor\n\n");
  fprintf(stderr, "usage: %s [options] < input_file > output_file\n", name);
  fprintf(stderr, "  or use unix pipes appropriately\n\n");
  fprintf(stderr, "options:\n");  
  fprintf(stderr, "  -h, --help: show this message\n");
  fprintf(stderr, "  -v, --verbose: print more info messages\n");
  fprintf(stderr, "  -d, --decompress: self-explanatory (default behavior is to compress)\n");
  fprintf(stderr, "  --start-offset=N: before decompressing, ignore this many bytes from the input stream\n");
  fprintf(stderr, "  --raw-bytes=N: before decompressing, copy this many bytes directly to the output stream (after start_offset bytes have been discarded)\n");
  fprintf(stderr, "  --pr2-format: expect a .pr2 file header before compressed data, and only decompress as much data as it says is compressed\n");
  fprintf(stderr, "  --prs: use Sega\'s press format (default)\n");
  fprintf(stderr, "  --yaz0: use Nintendo\'s Yaz0 format\n");
  fprintf(stderr, "  --yay0: use Nintendo\'s Yay0 format\n");
}


int main(int argc, char* argv[]) {

  FILE *in = stdin, *out = stdout;

  int x, decompress = 0, verbose = 0, pr2_format = 0;
  int64_t start_offset = 0, raw_bytes = 0;
  int format = FORMAT_PRS;
  for (x = 1; x < argc; x++) {

    if (!strcmp(argv[x], "-h") || !strcmp(argv[x], "--help")) {
      print_help(argv[0]);
      return 0;

    } else if (!strcmp(argv[x], "-d") || !strcmp(argv[x], "--decompress")) {
      decompress = 1;

    } else if (!strcmp(argv[x], "-v") || !strcmp(argv[x], "--verbose")) {
      verbose = 1;

    } else if (!strncmp(argv[x], "--start-offset=", 15)) {
      if (argv[x][15] == '0' && argv[x][16] == 'x') {
        sscanf(&argv[x][17], "%" PRIX64, &start_offset);
      } else {
        start_offset = atoi(&argv[x][15]);
      }

    } else if (!strcmp(argv[x], "--yaz0")) {
      format = FORMAT_YAZ0;

    } else if (!strcmp(argv[x], "--yay0")) {
      format = FORMAT_YAY0;

    } else if (!strcmp(argv[x], "--prs")) {
      format = FORMAT_PRS;

    } else if (!strncmp(argv[x], "--raw-bytes=", 12)) {
      if (argv[x][12] == '0' && argv[x][13] == 'x') {
        sscanf(&argv[x][14], "%" PRIX64, &raw_bytes);
      } else {
        raw_bytes = atoi(&argv[x][12]);
      }

    } else if (!strncmp(argv[x], "--pr2-format", 12))
      pr2_format = 1;

    else {
      fprintf(stderr, "prs: unknown command line option: %s\n", argv[x]);
    }
  }

  if (pr2_format && (format != FORMAT_PRS)) {
    fprintf(stderr, "prs: warning: using pr2 format for non-prs stream\n");
  }

  if (verbose) {
    const char* action_str = (decompress ? "decompress" : "compress");
    const char* format_str;
    if (format == FORMAT_PRS) {
      format_str = "FORMAT_PRS";
    }
    if (format == FORMAT_YAZ0) {
      format_str = "FORMAT_YAZ0";
    }
    if (format == FORMAT_YAY0) {
      format_str = "FORMAT_YAY0";
    }
    fprintf(stderr, "prs: action=%s, format=%s, start_offset=%016" PRIX64 ", "
        "raw_bytes=%016" PRIX64 "\n", action_str, format_str, start_offset, raw_bytes);
  }

  int64_t ret = ERROR_UNSUPPORTED;

  if (decompress) {
    int32_t size;
    if (pr2_format) {
      fread(&size, 4, 1, in);
      size = byteswap32(size);
      prs_decompress_stream(in, out, size);

    } else {
      // skip start_offset bytes, then copy raw_bytes bytes to the output
      for (; start_offset > 0; fgetc(in), start_offset--);
      for (; raw_bytes > 0; fputc(fgetc(in), out), raw_bytes--);
      size = 0;
    }

    if (format == FORMAT_PRS) {
      ret = prs_decompress_stream(in, out, size);
    } else if (format == FORMAT_YAZ0) {
      ret = yaz0_decompress_stream(in, out, size);
    } else if (format == FORMAT_YAY0) {
      ret = yay0_decompress_stream(in, out, size);
    }

  } else {
    if (format == FORMAT_PRS) {
      ret = prs_compress_stream(in, out, -1);
    } else if (format == FORMAT_YAZ0) {
      fprintf(stderr, "prs: yaz0 compression not supported\n");
    } else if (format == FORMAT_YAY0) {
      fprintf(stderr, "prs: yay0 compression not supported\n");
    }
  }

  if (ret < 0) {
    fprintf(stderr, "prs: operation failed with error %" PRId64 "\n", ret);
  } else if (verbose) {
    if (ret > 0) {
      fprintf(stderr, "prs: %" PRId64 " (0x%" PRIX64 ") bytes written\n", ret, ret);
    } else {
      fprintf(stderr, "prs: warning: result was empty\n");
    }
  }

  return 0;
}
