#pragma once
// One line per event on stderr: under systemd that is the journal, which adds the time and the unit.
namespace ps5cam {

[[gnu::format(printf, 1, 2)]] void Log(const char* format, ...);

}  // namespace ps5cam
