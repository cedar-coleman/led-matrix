// Text helpers that reproduce what mock_server.py gets from Python for free: code point
// lengths, str.strip(), repr() of a value in an error message, and the API's patterns.
//
// The patterns follow API.md, which is the contract, not Python's `re` quirks: `$` in the
// mock also matches before a trailing newline and `\d` matches non-ASCII digits. Here
// "a1\n" is not an id and "١٢٣٤٥٦" is not a PIN.
#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include <string>

namespace netcore {

bool utf8Valid(const char* s, size_t n);
size_t utf8Length(const std::string& s);  // code points; the string is assumed valid UTF-8

// Python's str.strip(): removes Unicode whitespace from both ends.
std::string pyStrip(const std::string& s);

// Python's repr() for the values that can reach an error message (strings, null, numbers,
// booleans). Anything else is shown as "...".
std::string pyRepr(JsonVariantConst v);

bool isId(const std::string& s);          // ^[A-Za-z0-9_-]{1,32}$
bool isMediaName(const std::string& s);   // ^[a-z0-9][a-z0-9._-]{0,47}\.gif$
bool isPin(const std::string& s);         // ^[0-9]{6,12}$
bool isAccountId(const std::string& s);   // ^[0-9]{0,32}$
bool isAccessToken(const std::string& s); // ^[A-Za-z0-9_\-|.]{0,1024}$
bool isGeneration(const std::string& s);  // ^[0-9]{1,9}$
// A real calendar day written YYYY-MM-DD, year 0001 to 9999 (Python's date.fromisoformat
// after the pattern ^[0-9]{4}-[0-9]{2}-[0-9]{2}$).
bool isCalendarDate(const std::string& s);

// /ig/ paths after percent-decoding: ^(\d{1,9})/((story|post)-\d{1,2}\.bmp)$.
bool parseTilePath(const std::string& rel, std::string* gen, std::string* file);

// JSON numbers as the mock sees them: an int is an integer that is not a boolean.
bool isJsonInt(JsonVariantConst v);
bool isJsonNumber(JsonVariantConst v);  // int or float, never a boolean

// The key is present in the object, whatever its value (null included).
bool hasKey(JsonObjectConst o, const char* key);

std::string hex(const uint8_t* data, size_t n);
bool unhex(const std::string& s, uint8_t* out, size_t n);  // exactly 2n lowercase or uppercase hex digits

// "YYYY-MM-DDTHH:MM:SSZ" and "YYYY-MM-DD" for a Unix time (UTC).
std::string isoUtc(int64_t unix);
std::string civilDate(int64_t days);  // days since 1970-01-01
int64_t daysFromCivil(int y, unsigned m, unsigned d);
void civilFromDays(int64_t z, int* y, unsigned* m, unsigned* d);

}  // namespace netcore
