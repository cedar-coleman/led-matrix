// The handful of JavaScript semantics the renderers depend on, so the C++ port of
// web/layout.js, web/igslides.js and web/power.js produces the same pixels.
//
// JS strings are UTF-16: `s.length` and `s.slice()` count code units, while
// `for (const ch of s)` walks code points. Names and titles reach the board as UTF-8,
// so the port converts them to std::u16string and keeps both rules, which is what makes
// truncation and '?' glyphs land where the browser preview puts them.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace wall {
namespace render {

using JsString = std::u16string;

// Invalid UTF-8 becomes U+FFFD, as TextDecoder does; it renders as '?' either way.
JsString fromUtf8(const std::string& s);
std::string toUtf8(const JsString& s);

// String.prototype.slice, including negative indices.
JsString jsSlice(const JsString& s, long from, long to);
inline JsString jsSlice(const JsString& s, long from) { return jsSlice(s, from, static_cast<long>(s.size())); }

// `for (const ch of s)`: f(firstUnit, unitCount) once per code point. A lone surrogate
// is its own character, as in JS.
template <class F>
void forEachChar(const JsString& s, F f) {
  for (size_t i = 0; i < s.size();) {
    const char16_t u = s[i];
    const bool pair = u >= 0xD800 && u <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF;
    f(u, pair ? 2 : 1);
    i += pair ? 2 : 1;
  }
}

// Math.floor(a / b) for ints, where C++ division truncates toward zero instead.
inline int floorDiv(int a, int b) {
  const int q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Math.round: halves go toward +infinity. floor(x + 0.5) is wrong for 0.49999999999999994.
double jsRound(double x);

// Number.prototype.toFixed(1) for finite x >= 0: the exact binary value is rounded, and
// an exact tie goes up (1.25 -> "1.3"), where printf would round it to even.
std::string toFixed1(double x);

// String(v) for an integer-valued double, e.g. 136 -> "136".
std::string intString(double v);

// Date.parse for the ISO 8601 shapes this project sees (V8 rules): YYYY, YYYY-MM or
// YYYY-MM-DD, optionally followed by 'T' or ' ' and HH:MM[:SS[.fff]], optionally Z,
// +HH:MM or +HHMM. Day 1..31 is accepted in any month and rolls over, as V8 does.
// A date-time with no offset is local time in JS; the wall treats it as UTC, and the
// parity fixtures are generated with TZ=UTC so the two agree.
// Returns false where Date.parse returns NaN.
bool parseIsoMs(const std::string& s, int64_t* ms);

// Days since 1970-01-01 for a proleptic Gregorian date (month 1..12, any day count).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d);

}  // namespace render
}  // namespace wall
