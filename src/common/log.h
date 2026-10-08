#pragma once
#include <string>

namespace ps5cam {

// Appends a timestamped line to %ProgramData%\PS5Camera\<name>.log; echoConsole also prints to stderr.
void LogInit(const wchar_t* name, bool echoConsole = false);
void Log(const wchar_t* fmt, ...);

}  // namespace ps5cam
