// What the generator links against from the rest of the emulator, as far as a host build needs.
#include <cstdio>
#include <cstdlib>
#include "common/logging/log.h"
namespace Common::Log {
void FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view,
                       const fmt::format_args&) {}
void Stop() {}
} // namespace Common::Log
