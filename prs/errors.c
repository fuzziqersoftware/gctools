#include "errors.h"

const char* name_for_error_code(int error) {
  switch (error) {
    case ERROR_UNSUPPORTED:
      return "ERROR_UNSUPPORTED";
    case ERROR_CORRUPTED:
      return "ERROR_CORRUPTED";
    case ERROR_UNRECOGNIZED:
      return "ERROR_UNRECOGNIZED";
    case ERROR_MEMORY:
      return "ERROR_MEMORY";
    case ERROR_BACKREFERENCE_TOO_DISTANT:
      return "ERROR_BACKREFERENCE_TOO_DISTANT";
    case ERROR_OVERFLOW:
      return "ERROR_OVERFLOW";
    default:
      return "ERROR_UNKNOWN";
  }
}
