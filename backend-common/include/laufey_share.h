// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The share sheet (API 49): macOS NSSharingServicePicker, Windows
// DataTransferManager; none on Linux. See laufey.h `share` and
// docs/share.md.

#ifndef LAUFEY_SHARE_H_
#define LAUFEY_SHARE_H_

#include <string>
#include <vector>

#include "laufey.h"

namespace laufey_common {

struct ShareRequest {
  std::string title;
  std::string text;
  std::string url;
  std::vector<std::string> files;
  bool has_point = false;
  double x = 0;
  double y = 0;
};

// Parses and checks a share request: a JSON object whose "title", "text"
// and "url" are strings, "files" an array of absolute paths, "x"/"y"
// numbers (both or neither); at least one of text, url or files; "url" an
// absolute URL with a scheme. False with `error` otherwise.
bool ParseShareRequest(const std::string& json, ShareRequest* out,
                       std::string* error);

// Reports a refusal synchronously.
void ShareReport(laufey_share_fn callback, void* user_data, int status,
                 const std::string& detail);

#if defined(__APPLE__)
// On the main thread: the picker over `ns_window` (NSWindow*, or nullptr for
// the key / main window).
void ShareShowMac(void* ns_window, const ShareRequest& request,
                  laufey_share_fn callback, void* user_data);
#endif

#if defined(_WIN32)
// On the window's thread: the share UI for `hwnd`.
void ShareShowWin(void* hwnd, const ShareRequest& request,
                  laufey_share_fn callback, void* user_data);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_SHARE_H_
