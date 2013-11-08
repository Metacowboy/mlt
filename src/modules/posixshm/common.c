#include "common.h"

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#define GET_COLOR(x) (x == 0 ? ANSI_COLOR_BLUE : (x == 1 ? ANSI_COLOR_GREEN : (x == 2 ? ANSI_COLOR_MAGENTA : ANSI_COLOR_YELLOW)))
#define GET_DESC(x) (x == 0 ? "  ROOT" : "THREAD")
#define DEBUG 1

int write_log(int thread, const char *format, ...)
{

#if DEBUG
  va_list args;
  va_start(args, format);

  printf("\n%s%s: " ANSI_COLOR_RESET, GET_COLOR(thread), GET_DESC(thread));
  vprintf(format, args);

  va_end(args);
#endif

  return 0;
}
