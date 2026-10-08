#include "log.h"

#include <cstdarg>
#include <cstdio>

namespace ps5cam {

void Log(const char* format, ...)
{
    // Formatted first so that the line reaches the journal in one write, never interleaved.
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    fprintf(stderr, "%s\n", line);
}

}  // namespace ps5cam
