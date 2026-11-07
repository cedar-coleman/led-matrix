#include "nc_tz.h"

#include <ctype.h>
#include <stdio.h>

#include "nc_text.h"

namespace netcore {

const Zone kZones[kZoneCount] = {
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix", "MST7"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu", "HST10"},
    {"UTC", "UTC0"},
};

int zoneIndex(const std::string& iana) {
  for (int i = 0; i < kZoneCount; i++) {
    if (iana == kZones[i].iana) return i;
  }
  return -1;
}

namespace {

struct Rule {
  int month = 0, week = 0, wday = 0;
  int32_t time = 7200;  // seconds after local midnight; POSIX default 02:00
};

struct Tz {
  int32_t stdOff = 0;  // POSIX sign: seconds WEST of UTC
  int32_t dstOff = 0;
  bool hasDst = false;
  Rule start, end;
};

void skipName(const char*& p) {
  if (*p == '<') {
    while (*p && *p != '>') p++;
    if (*p) p++;
    return;
  }
  while (isalpha(static_cast<unsigned char>(*p))) p++;
}

bool number(const char*& p, int* out) {
  if (!isdigit(static_cast<unsigned char>(*p))) return false;
  int v = 0;
  while (isdigit(static_cast<unsigned char>(*p))) v = v * 10 + (*p++ - '0');
  *out = v;
  return true;
}

// [+-]hh[:mm[:ss]] in seconds.
bool offset(const char*& p, int32_t* out) {
  int sign = 1;
  if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
  int h, m = 0, s = 0;
  if (!number(p, &h)) return false;
  if (*p == ':') {
    p++;
    if (!number(p, &m)) return false;
    if (*p == ':') {
      p++;
      if (!number(p, &s)) return false;
    }
  }
  *out = sign * (h * 3600 + m * 60 + s);
  return true;
}

bool rule(const char*& p, Rule* r) {
  if (*p++ != 'M') return false;  // the eight zones only use the Mm.w.d form
  if (!number(p, &r->month) || *p++ != '.' || !number(p, &r->week) || *p++ != '.' || !number(p, &r->wday)) return false;
  if (*p == '/') {
    p++;
    if (!offset(p, &r->time)) return false;
  }
  return r->month >= 1 && r->month <= 12 && r->week >= 1 && r->week <= 5 && r->wday >= 0 && r->wday <= 6;
}

bool parse(const char* p, Tz* tz) {
  skipName(p);
  if (!offset(p, &tz->stdOff)) return false;
  if (!*p) return true;
  skipName(p);
  tz->hasDst = true;
  tz->dstOff = tz->stdOff - 3600;
  if (*p && *p != ',' && !offset(p, &tz->dstOff)) return false;
  if (*p++ != ',' || !rule(p, &tz->start) || *p++ != ',' || !rule(p, &tz->end)) return false;
  return *p == 0;
}

// Local midnight (as days since the epoch) of the rule's day in `year`.
int64_t ruleDay(int year, const Rule& r) {
  const int64_t first = daysFromCivil(year, r.month, 1);
  const int firstWday = static_cast<int>(((first % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday
  int64_t day = first + ((r.wday - firstWday + 7) % 7) + 7 * (r.week - 1);
  const int nextMonthYear = r.month == 12 ? year + 1 : year;
  const unsigned nextMonth = r.month == 12 ? 1 : r.month + 1;
  const int64_t limit = daysFromCivil(nextMonthYear, nextMonth, 1);
  while (day >= limit) day -= 7;  // week 5 means "the last one"
  return day;
}

}  // namespace

int32_t utcOffsetAt(const char* posix, int64_t unix) {
  Tz tz;
  if (!posix || !parse(posix, &tz)) return 0;
  if (!tz.hasDst) return -tz.stdOff;
  int y;
  unsigned m, d;
  const int64_t stdLocal = unix - tz.stdOff;
  civilFromDays(stdLocal >= 0 ? stdLocal / 86400 : -((-stdLocal + 86399) / 86400), &y, &m, &d);
  // Transition instants in UTC: the start rule is read in standard time, the end in DST.
  const int64_t start = ruleDay(y, tz.start) * 86400 + tz.start.time + tz.stdOff;
  const int64_t end = ruleDay(y, tz.end) * 86400 + tz.end.time + tz.dstOff;
  const bool dst = start < end ? (unix >= start && unix < end) : !(unix >= end && unix < start);
  return -(dst ? tz.dstOff : tz.stdOff);
}

int64_t nextOffsetChange(const char* posix, int64_t unix) {
  Tz tz;
  if (!posix || !parse(posix, &tz) || !tz.hasDst) return -1;
  int y;
  unsigned m, d;
  const int64_t stdLocal = unix - tz.stdOff;
  civilFromDays(stdLocal >= 0 ? stdLocal / 86400 : -((-stdLocal + 86399) / 86400), &y, &m, &d);
  // Both switches of last year, this year and next: whatever `unix` is, the next one is
  // among them.
  int64_t best = -1;
  for (int year = y - 1; year <= y + 1; year++) {
    for (const int64_t t : {ruleDay(year, tz.start) * 86400 + tz.start.time + tz.stdOff,
                            ruleDay(year, tz.end) * 86400 + tz.end.time + tz.dstOff}) {
      if (t > unix && (best < 0 || t < best)) best = t;
    }
  }
  return best;
}

std::string localIso(int zone, int64_t unix) {
  if (zone < 0 || zone >= kZoneCount) zone = kDefaultZone;
  const int32_t off = utcOffsetAt(kZones[zone].posix, unix);
  const int64_t local = unix + off;
  const int64_t days = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);
  const int64_t sod = local - days * 86400;
  const int32_t a = off < 0 ? -off : off;
  char buf[24];
  snprintf(buf, sizeof buf, "T%02d:%02d:%02d%c%02d:%02d", static_cast<int>(sod / 3600), static_cast<int>(sod / 60 % 60),
           static_cast<int>(sod % 60), off < 0 ? '-' : '+', static_cast<int>(a / 3600), static_cast<int>(a / 60 % 60));
  return civilDate(days) + buf;
}

std::string localDate(int zone, int64_t unix) {
  if (zone < 0 || zone >= kZoneCount) zone = kDefaultZone;
  const int64_t local = unix + utcOffsetAt(kZones[zone].posix, unix);
  return civilDate(local >= 0 ? local / 86400 : -((-local + 86399) / 86400));
}

}  // namespace netcore
