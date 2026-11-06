// The wall's eight time zones (API.md section 9) and local dates from POSIX TZ rules.
//
// The device does not use setenv("TZ") + localtime_r: the render task on core 1 asks for
// the date while the API task on core 0 may change the zone, and newlib's TZ state is a
// process-wide global. Evaluating the rule here is a pure function, so both are safe.
#pragma once

#include <stdint.h>

#include <string>

namespace netcore {

struct Zone {
  const char* iana;
  const char* posix;
};

constexpr int kZoneCount = 8;
extern const Zone kZones[kZoneCount];
constexpr int kDefaultZone = 0;  // America/Los_Angeles

int zoneIndex(const std::string& iana);  // -1 if not one of the eight

// Seconds east of UTC in effect at `unix` for a POSIX TZ string of the form
// STDoffset[DST[offset][,Mm.w.d[/time],Mm.w.d[/time]]]. 0 for a string it can't parse.
int32_t utcOffsetAt(const char* posix, int64_t unix);

// "YYYY-MM-DD" in the zone at `unix`.
std::string localDate(int zone, int64_t unix);

}  // namespace netcore
