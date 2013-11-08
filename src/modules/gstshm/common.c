#include "common.h"

#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#define DEBUG 1

int write_log(int thread, const char *format, ...)
{

#if DEBUG
  va_list args;
  va_start(args, format);

  if (thread) {
    printf("\n" ANSI_COLOR_GREEN "THREAD %d: " ANSI_COLOR_RESET, thread);
  } else {
    printf("\n" ANSI_COLOR_BLUE "ROOT: " ANSI_COLOR_RESET);
  }

  vprintf(format, args);

  va_end(args);
#endif

  return 0;
}
