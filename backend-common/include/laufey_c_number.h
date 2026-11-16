// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Numbers in JSON's syntax (a '.' decimal point) whatever the process
// locale says. GTK calls setlocale(LC_ALL, ""), and strtod / snprintf follow
// LC_NUMERIC's decimal point: ',' in German or Swedish, the two-byte U+066B
// in Persian. Parsing "1.5" there stops at the '.', and "%g" writes "1,5".
// Header-only.

#ifndef LAUFEY_C_NUMBER_H_
#define LAUFEY_C_NUMBER_H_

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace laufey_common {

// The current LC_NUMERIC decimal point, or "." when it can't be read.
inline std::string LocaleDecimalPoint() {
  const std::lconv* conv = std::localeconv();
  const char* point = conv ? conv->decimal_point : nullptr;
  return point && point[0] ? std::string(point) : std::string(".");
}

// strtod for a number written with a '.' decimal point (leading white
// space skipped, as strtod does). `end` (optional) gets the first character
// of `s` not used, as strtod's does: `s` itself when there is no number.
inline double StrtodC(const char* s, const char** end) {
  if (end)
    *end = s;
  if (!s)
    return 0;
  const char* begin = s;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' ||
         *s == '\v')
    ++s;
  // The longest run of characters a decimal number can use; strtod decides
  // how much of it is the number.
  size_t n = 0;
  while (s[n] && (std::strchr("0123456789+-.eE", s[n]) != nullptr))
    ++n;
  const std::string point = LocaleDecimalPoint();
  std::string token;
  token.reserve(n + 8);
  for (size_t i = 0; i < n; ++i) {
    if (s[i] == '.')
      token += point;
    else
      token += s[i];
  }
  char* tend = nullptr;
  double d = std::strtod(token.c_str(), &tend);
  if (end) {
    // Map the characters strtod used back onto `s`.
    size_t used = static_cast<size_t>(tend - token.c_str());
    size_t i = 0;
    size_t t = 0;
    while (i < n && t < used) {
      t += s[i] == '.' ? point.size() : 1;
      ++i;
    }
    *end = used ? s + i : begin;
  }
  return d;
}

// `d` as JSON writes it: "%.17g" with a '.' decimal point, and "null" for
// a NaN or an infinity (as JSON.stringify writes them).
inline std::string FormatDoubleC(double d) {
  if (!std::isfinite(d))
    return "null";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  std::string s = buf;
  const std::string point = LocaleDecimalPoint();
  if (point != ".") {
    size_t at = s.find(point);
    if (at != std::string::npos)
      s.replace(at, point.size(), ".");
  }
  return s;
}

}  // namespace laufey_common

#endif  // LAUFEY_C_NUMBER_H_
