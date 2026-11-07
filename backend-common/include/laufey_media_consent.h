// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The operating system's own consent for the microphone and camera
// (docs/media-capture.md#the-operating-systems-consent).
//
// Windows keeps it in the registry, each switch the "Value" string ("Allow" /
// "Deny") of a CapabilityAccessManager ConsentStore key (the camera's key is
// "webcam"):
//
//   HKLM ...\ConsentStore\<device>              "Microphone access" /
//                                               "Camera access": the whole
//                                               device, every app
//   HKCU ...\ConsentStore\<device>              "Let apps access your
//                                               microphone / camera": Store
//                                               (packaged) apps only
//   HKCU ...\ConsentStore\<device>\NonPackaged  "Let desktop apps access your
//                                               microphone / camera"
//
// A laufey app is a desktop (unpackaged) app: the device-wide switch and the
// desktop-apps switch govern it, and the packaged apps' switch does not (a
// machine can have it off while desktop apps capture: GitHub's Windows
// images do). Either of the two set to "Deny" keeps the device from the app:
// the engine would fail to open the device (an error of its own) after laufey
// had granted it. laufey refuses
// such a request itself (NotAllowedError, as macOS's TCC refusal reads), and
// platform_features reports the switches (microphoneAccess / cameraAccess).
// The switches are read at every request, so a change the user makes while
// the app runs counts from the next one.
//
// macOS (TCC) and Linux have no such switches laufey reads: there the probe
// says nothing, and macOS refuses by itself.
//
// Pure, except ProbeOsMediaConsent (Windows only, media_consent_win.cc).

#ifndef LAUFEY_MEDIA_CONSENT_H_
#define LAUFEY_MEDIA_CONSENT_H_

#include <cstdint>
#include <string>

#include "laufey.h"

namespace laufey_common {

// One device's switches as the registry holds them: each a "Value" string,
// empty when the key or the value is missing (or unreadable).
struct ConsentStoreSwitches {
  std::string machine;       // HKLM ...\ConsentStore\<device>
  std::string user;          // HKCU ...\ConsentStore\<device> (packaged
                             // apps': never refuses a desktop app)
  std::string desktop_apps;  // HKCU ...\ConsentStore\<device>\NonPackaged
};

enum class OsMediaConsent {
  kUnknown,  // nothing read (not Windows, or no switch is set)
  kAllowed,  // a switch reads "Allow" and none reads "Deny"
  kDenied,   // a switch reads "Deny"
};

struct OsMediaConsentState {
  OsMediaConsent consent = OsMediaConsent::kUnknown;
  // Which switch refuses, in the user's words (Settings' path); empty unless
  // kDenied.
  std::string reason;
};

namespace media_consent_internal {

inline bool EqualsIgnoringCase(const std::string& a, const char* b) {
  size_t i = 0;
  for (; i < a.size() && b[i]; ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z')
      x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z')
      y = static_cast<char>(y - 'A' + 'a');
    if (x != y)
      return false;
  }
  return i == a.size() && b[i] == '\0';
}

}  // namespace media_consent_internal

// The ConsentStore key name of a LAUFEY_MEDIA_CAPTURE_* kind ("microphone",
// "webcam"), or nullptr for anything else.
inline const char* ConsentStoreDevice(uint32_t kind) {
  if (kind == LAUFEY_MEDIA_CAPTURE_MICROPHONE)
    return "microphone";
  if (kind == LAUFEY_MEDIA_CAPTURE_CAMERA)
    return "webcam";
  return nullptr;
}

// What `switches` say for `kind` (one LAUFEY_MEDIA_CAPTURE_* bit) to a
// desktop app: the device-wide switch, then the desktop-apps one; the first
// "Deny" names the reason. The packaged apps' switch (`user`) is not read.
// Pure.
inline OsMediaConsentState ConsentFromSwitches(
    const ConsentStoreSwitches& switches, uint32_t kind) {
  using media_consent_internal::EqualsIgnoringCase;
  const bool mic = kind == LAUFEY_MEDIA_CAPTURE_MICROPHONE;
  const std::string device = mic ? "microphone" : "camera";
  const std::string page = "Settings > Privacy & security > " +
                           std::string(mic ? "Microphone" : "Camera");
  OsMediaConsentState out;
  if (EqualsIgnoringCase(switches.machine, "deny")) {
    out.consent = OsMediaConsent::kDenied;
    out.reason = "Windows turns off " + device + " access on this device (" +
                 page + " > " + (mic ? "Microphone access" : "Camera access") +
                 ")";
    return out;
  }
  if (EqualsIgnoringCase(switches.desktop_apps, "deny")) {
    out.consent = OsMediaConsent::kDenied;
    out.reason = "Windows keeps the " + device + " from desktop apps (" + page +
                 " > Let desktop apps access your " + device + ")";
    return out;
  }
  if (EqualsIgnoringCase(switches.machine, "allow") ||
      EqualsIgnoringCase(switches.desktop_apps, "allow"))
    out.consent = OsMediaConsent::kAllowed;
  return out;
}

// The kinds the OS refuses, from each device's state. Pure.
inline uint32_t DeniedMediaKinds(const OsMediaConsentState& microphone,
                                 const OsMediaConsentState& camera) {
  uint32_t denied = 0;
  if (microphone.consent == OsMediaConsent::kDenied)
    denied |= LAUFEY_MEDIA_CAPTURE_MICROPHONE;
  if (camera.consent == OsMediaConsent::kDenied)
    denied |= LAUFEY_MEDIA_CAPTURE_CAMERA;
  return denied;
}

// platform_features' value for a state: "allowed", "denied", or nullptr
// (JSON null: unknown).
inline const char* OsMediaConsentName(OsMediaConsent consent) {
  switch (consent) {
    case OsMediaConsent::kAllowed:
      return "allowed";
    case OsMediaConsent::kDenied:
      return "denied";
    case OsMediaConsent::kUnknown:
      return nullptr;
  }
  return nullptr;
}

#ifdef _WIN32
// The registry's switches for `kind`, read now. Windows only
// (media_consent_win.cc). Any thread.
OsMediaConsentState ProbeOsMediaConsent(uint32_t kind);
#endif

// The kinds the OS refuses this process now: Windows' switches; none
// elsewhere. Any thread.
inline uint32_t ProcessOsDeniedMediaKinds() {
#ifdef _WIN32
  return DeniedMediaKinds(ProbeOsMediaConsent(LAUFEY_MEDIA_CAPTURE_MICROPHONE),
                          ProbeOsMediaConsent(LAUFEY_MEDIA_CAPTURE_CAMERA));
#else
  return 0;
#endif
}

}  // namespace laufey_common

#endif  // LAUFEY_MEDIA_CONSENT_H_
