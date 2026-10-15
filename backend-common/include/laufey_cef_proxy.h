// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// How a CEF window on Windows reaches the network ("proxy" in the launch
// file, LAUFEY_PROXY; see LaunchProxyMode).
//
// Under the OS proxy configuration Chromium holds every request, loopback
// ones included, until its proxy resolver has initialized; with automatic
// detection (WPAD) or a PAC URL that can take seconds, and the page's
// WebSocket to the runtime's loopback relay waits for it. So:
//   - every mode warms the resolver up as the browser context is created
//     (CefWarmUpProxyResolution in main_windows.cc), so its initialization
//     overlaps the app's startup instead of following the first request;
//   - "system-resolver" resolves through WinHTTP per request
//     (--use-system-proxy-resolver), with no Chromium-side initialization;
//   - "direct" uses no proxy at all (--no-proxy-server).
// A proxy switch the app's own command line already carries wins.

#ifndef LAUFEY_CEF_PROXY_H_
#define LAUFEY_CEF_PROXY_H_

#include <string>
#include <vector>

namespace laufey_common {

// The Chromium switches whose presence means the command line already
// chose a proxy configuration.
inline const std::vector<std::string>& CefProxySwitchNames() {
  static const std::vector<std::string> names = {"no-proxy-server",
                                                 "proxy-server",
                                                 "proxy-pac-url",
                                                 "proxy-auto-detect",
                                                 "use-system-proxy-resolver",
                                                 "winhttp-proxy-resolver"};
  return names;
}

// The switch to append for `mode`, or "" for none ("system", or a command
// line that already chose: `has_proxy_switch`).
inline std::string CefProxySwitchFor(const std::string& mode,
                                     bool has_proxy_switch) {
  if (has_proxy_switch)
    return "";
  if (mode == "direct")
    return "no-proxy-server";
  if (mode == "system-resolver")
    return "use-system-proxy-resolver";
  return "";
}

}  // namespace laufey_common

#endif  // LAUFEY_CEF_PROXY_H_
