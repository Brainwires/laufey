// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The count a streamed scheme response's reader acknowledges (WebView2's
// stream shim posts {"__laufeyStream":{"t":"ack","id":…,"n":…}}). The message
// comes from the page, so `n` is whatever a script there wrote: only 1 to 11
// ASCII digits are a count; anything else (empty, a sign, an exponent, a
// letter, too many digits) is ignored rather than parsed. Header-only.

#ifndef LAUFEY_STREAM_ACK_H_
#define LAUFEY_STREAM_ACK_H_

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>

namespace laufey_common {

// Longest count accepted, in digits.
constexpr size_t kMaxStreamAckDigits = 11;

// Parses an acknowledged count. Returns false (leaving *out as it was) unless
// `n` is 1 to kMaxStreamAckDigits ASCII digits whose value fits in size_t.
inline bool ParseStreamAckCount(const std::string& n, size_t* out) {
  if (n.empty() || n.size() > kMaxStreamAckDigits)
    return false;
  for (char c : n) {
    if (c < '0' || c > '9')
      return false;
  }
  uint64_t value = 0;
  const char* end = n.data() + n.size();
  auto result = std::from_chars(n.data(), end, value);
  if (result.ec != std::errc() || result.ptr != end ||
      // Parenthesized: <windows.h> defines a max() macro.
      value > (std::numeric_limits<size_t>::max)())
    return false;
  *out = static_cast<size_t>(value);
  return true;
}

}  // namespace laufey_common

#endif  // LAUFEY_STREAM_ACK_H_
