#include "nc_schedule.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "nc_text.h"
#include "nc_tz.h"

namespace netcore {

namespace {

const char* const kDayNames[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

// HH:MM, 00:00 to 23:59, or 24:00 where an end is allowed.
bool hhmm(JsonVariantConst v, bool isEnd, int16_t* minutes) {
  if (!v.is<const char*>()) return false;
  const char* s = v.as<const char*>();
  if (strlen(s) != 5 || s[2] != ':') return false;
  for (int i : {0, 1, 3, 4}) {
    if (!isdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  const int h = (s[0] - '0') * 10 + (s[1] - '0'), m = (s[3] - '0') * 10 + (s[4] - '0');
  if (isEnd && h == 24 && m == 0) {
    *minutes = 1440;
    return true;
  }
  if (h > 23 || m > 59) return false;
  *minutes = static_cast<int16_t>(h * 60 + m);
  return true;
}

ApiError dayError(int i, const std::string& what) {
  ApiError e = ApiError::make(400, std::string(kDayNames[i]) + ": " + what);
  e.day = kDayKeys[i];
  return e;
}

int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

std::string hhmmText(int minutes) {
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
  return buf;
}

}  // namespace

ApiError validateSchedule(JsonVariantConst in, JsonObject out) {
  if (!in.is<JsonObjectConst>()) return ApiError::make(400, "Schedule must be an object with enabled and days");
  if (!in["enabled"].is<bool>()) return ApiError::make(400, "Schedule enabled must be true or false");
  JsonVariantConst days = in["days"];
  bool all = days.is<JsonObjectConst>();
  for (const char* k : kDayKeys) all = all && hasKey(days.as<JsonObjectConst>(), k);
  if (!all) return ApiError::make(400, "Schedule days must list mon, tue, wed, thu, fri, sat and sun");
  DayWindows parsed[7];
  for (int i = 0; i < 7; i++) {
    JsonVariantConst list = days[kDayKeys[i]];
    if (!list.is<JsonArrayConst>() || list.size() > 2) return dayError(i, "give a list of 0 to 2 time ranges");
    DayWindows& d = parsed[i];
    for (JsonVariantConst w : list.as<JsonArrayConst>()) {
      int16_t a = 0, b = 0;
      if (!w.is<JsonObjectConst>() || !hhmm(w["start"], false, &a) || !hhmm(w["end"], true, &b)) {
        return dayError(i, "times must be HH:MM, 00:00 to 23:59 (an end can be 24:00)");
      }
      if (b <= a) return dayError(i, "a time range must end after it starts");
      d.start[d.count] = a;
      d.end[d.count] = b;
      d.count++;
    }
    if (d.count == 2 && d.start[1] < d.start[0]) {  // equal starts can't pass the overlap check, so order is all that matters
      std::swap(d.start[0], d.start[1]);
      std::swap(d.end[0], d.end[1]);
    }
    if (d.count == 2 && d.end[0] > d.start[1]) return dayError(i, "the two time ranges overlap");
  }
  out["enabled"] = in["enabled"].as<bool>();
  JsonObject od = out["days"].to<JsonObject>();
  for (int i = 0; i < 7; i++) {
    JsonArray list = od[kDayKeys[i]].to<JsonArray>();
    for (int k = 0; k < parsed[i].count; k++) {
      JsonObject w = list.add<JsonObject>();
      w["start"] = hhmmText(parsed[i].start[k]);
      w["end"] = hhmmText(parsed[i].end[k]);
    }
  }
  return {};
}

void defaultSchedule(JsonObject out) {
  out["enabled"] = false;
  JsonObject days = out["days"].to<JsonObject>();
  for (const char* k : kDayKeys) {
    JsonObject w = days[k].to<JsonArray>().add<JsonObject>();
    w["start"] = "05:00";
    w["end"] = "21:00";
  }
}

Schedule scheduleFromJson(JsonVariantConst in) {
  Schedule s;
  JsonDocument clean;
  if (!validateSchedule(in, clean.to<JsonObject>()).ok()) return s;
  s.enabled = clean["enabled"].as<bool>();
  for (int i = 0; i < 7; i++) {
    for (JsonVariantConst w : clean["days"][kDayKeys[i]].as<JsonArrayConst>()) {
      DayWindows& d = s.days[i];
      hhmm(w["start"], false, &d.start[d.count]);
      hhmm(w["end"], true, &d.end[d.count]);
      d.count++;
    }
  }
  return s;
}

bool scheduledOn(const Schedule& s, const char* posix, int64_t unix) {
  const int64_t local = unix + utcOffsetAt(posix, unix);
  const int64_t days = floorDiv(local, 86400);
  const int64_t sod = local - days * 86400;
  const DayWindows& d = s.days[((days % 7) + 7 + 3) % 7];  // 1970-01-01 was a Thursday; Monday is 0
  for (int k = 0; k < d.count; k++) {
    if (d.start[k] * 60 <= sod && sod < d.end[k] * 60) return true;
  }
  return false;
}

int64_t nextFlip(const Schedule& s, const char* posix, int64_t from, bool value, int64_t horizon) {
  if (scheduledOn(s, posix, from) != value) return from;
  const int64_t end = from + horizon;
  // The state can only change where the local clock crosses a range boundary or where the
  // offset itself changes (a boundary skipped by the spring jump starts at the jump). So
  // list every boundary under every offset in force before `end`, plus the switches, and
  // take the first that differs. Boundaries under the wrong offset are harmless extras.
  std::vector<int64_t> candidates;
  std::vector<int32_t> offsets = {utcOffsetAt(posix, from)};
  for (int64_t t = from, tc; (tc = nextOffsetChange(posix, t)) >= 0 && tc <= end; t = tc) {
    candidates.push_back(tc);
    offsets.push_back(utcOffsetAt(posix, tc));
  }
  const int32_t lo = *std::min_element(offsets.begin(), offsets.end());
  const int32_t hi = *std::max_element(offsets.begin(), offsets.end());
  for (int64_t day = floorDiv(from + lo, 86400) - 1, last = floorDiv(end + hi, 86400) + 1; day <= last; day++) {
    const DayWindows& d = s.days[((day % 7) + 7 + 3) % 7];
    for (int k = 0; k < d.count; k++) {
      for (const int16_t boundary : {d.start[k], d.end[k]}) {
        for (const int32_t off : offsets) {
          const int64_t t = day * 86400 + boundary * 60 - off;
          if (t > from && t <= end) candidates.push_back(t);
        }
      }
    }
  }
  std::sort(candidates.begin(), candidates.end());
  for (const int64_t t : candidates) {
    if (scheduledOn(s, posix, t) != value) return t;
  }
  return -1;
}

const char* scheduleStateName(ScheduleState s) {
  switch (s) {
    case ScheduleState::kOn:
      return "on";
    case ScheduleState::kOff:
      return "off";
    case ScheduleState::kOverrideOn:
      return "override_on";
    case ScheduleState::kOverrideOff:
      return "override_off";
    case ScheduleState::kNoClock:
      return "no_clock";
    default:
      return "always_on";
  }
}

ScheduleStatus evaluateSchedule(const Schedule& s, int zone, bool clockValid, int64_t now, Override* ov) {
  if (zone < 0 || zone >= kZoneCount) zone = kDefaultZone;
  const char* posix = kZones[zone].posix;
  if (ov->active && ov->until >= 0 && now >= ov->until) *ov = Override();
  const bool active = s.enabled && clockValid;
  ScheduleStatus st;
  if (ov->active) {
    st.displayOn = ov->on;
    st.state = ov->on ? ScheduleState::kOverrideOn : ScheduleState::kOverrideOff;
    st.nextChange = active && ov->until >= 0 ? nextFlip(s, posix, ov->until, ov->on) : -1;
    return st;
  }
  if (!s.enabled) return st;
  if (!clockValid) {
    st.state = ScheduleState::kNoClock;
    return st;
  }
  st.displayOn = scheduledOn(s, posix, now);
  st.state = st.displayOn ? ScheduleState::kOn : ScheduleState::kOff;
  st.nextChange = nextFlip(s, posix, now, st.displayOn);
  return st;
}

Override overrideFor(const Schedule& s, int zone, bool clockValid, int64_t now, bool wake) {
  if (zone < 0 || zone >= kZoneCount) zone = kDefaultZone;
  Override o;
  if (s.enabled && clockValid) {
    const bool on = scheduledOn(s, kZones[zone].posix, now);
    if (on == wake) return o;
    o.active = true;
    o.on = wake;
    o.until = nextFlip(s, kZones[zone].posix, now, on);
    return o;
  }
  if (!wake) {
    o.active = true;
    o.on = false;
  }
  return o;
}

void scheduleStatusJson(const ScheduleStatus& st, int zone, JsonObject out) {
  out["display_on"] = st.displayOn;
  out["schedule_state"] = scheduleStateName(st.state);
  if (st.nextChange < 0) {
    out["next_change"] = nullptr;
  } else {
    out["next_change"] = localIso(zone, st.nextChange);
  }
}

}  // namespace netcore
