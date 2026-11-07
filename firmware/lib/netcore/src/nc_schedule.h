// The display schedule (API.md 7.4): validation of settings.schedule, and when the wall is
// on, given the schedule, the wall's zone, its clock and any wake/sleep override.
//
// Pure functions, so the host tests cover daylight saving on the exact code the device runs.
// The mock finds changes by checking every minute; this jumps from boundary to boundary
// and is tested against that scan.
#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

#include <string>

#include "nc_api.h"

namespace netcore {

constexpr const char* kDayKeys[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
// Long enough to cross a whole week plus a daylight-saving hour. A schedule with no change
// inside it never changes (every day off, or on all day every day).
constexpr int64_t kScheduleHorizonS = 8 * 86400;

struct DayWindows {
  uint8_t count = 0;
  int16_t start[2] = {};  // minutes after local midnight, 0..1439
  int16_t end[2] = {};    // 1..1440 (24:00)
};

struct Schedule {
  bool enabled = false;
  DayWindows days[7];  // Monday first
};

// settings.schedule, cleaned into `out`: windows sorted by start, unknown keys dropped.
// Errors about one day carry ApiError::day.
ApiError validateSchedule(JsonVariantConst in, JsonObject out);
void defaultSchedule(JsonObject out);
// A stored schedule the API would accept; anything else reads as disabled, so a damaged
// file can't leave the wall dark.
Schedule scheduleFromJson(JsonVariantConst in);

bool scheduledOn(const Schedule& s, const char* posix, int64_t unix);
// The first instant >= from where scheduledOn is not `value`, or -1 within the horizon.
int64_t nextFlip(const Schedule& s, const char* posix, int64_t from, bool value, int64_t horizon = kScheduleHorizonS);

// wake / sleep. `until` is the instant the schedule takes over again; < 0 means it lasts
// until the next wake, sleep or schedule save.
struct Override {
  bool active = false;
  bool on = false;
  int64_t until = -1;
};

enum class ScheduleState : uint8_t { kAlwaysOn, kOn, kOff, kOverrideOn, kOverrideOff, kNoClock };
const char* scheduleStateName(ScheduleState s);

struct ScheduleStatus {
  bool displayOn = true;
  ScheduleState state = ScheduleState::kAlwaysOn;
  int64_t nextChange = -1;  // when the display next switches; < 0: null
};

// Where the wall is now. Drops an override whose time has come (so call it with the one
// the caller keeps). A clock that isn't valid keeps the display on: fail open.
ScheduleStatus evaluateSchedule(const Schedule& s, int zone, bool clockValid, int64_t now, Override* ov);

// What a wake (true) or sleep (false) sets: on or off until the next scheduled change.
// Inactive when the schedule already has the wall that way. Without a schedule in force
// the wall is on, so only a sleep is kept, until a wake.
Override overrideFor(const Schedule& s, int zone, bool clockValid, int64_t now, bool wake);

// display_on, schedule_state and next_change for /api/status.
void scheduleStatusJson(const ScheduleStatus& st, int zone, JsonObject out);

}  // namespace netcore
