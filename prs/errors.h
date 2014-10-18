#define ERROR_UNSUPPORTED                -1
#define ERROR_CORRUPTED                  -2
#define ERROR_UNRECOGNIZED               -3
#define ERROR_MEMORY                     -4
#define ERROR_BACKREFERENCE_TOO_DISTANT  -5
#define ERROR_OVERFLOW                   -6

const char* name_for_error_code(int error);
