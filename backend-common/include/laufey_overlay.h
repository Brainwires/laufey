// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Overlay windows, the activation policy and text to speech (API 50). See
// docs/window-management.md ("Overlay windows"), docs/dock-taskbar.md and
// docs/platform-features.md.
//
// The portable part (overlay.cc) is what every backend shares: input-region
// hit testing, window-level validation, the launch activation policy, the
// speech request parser and the one-utterance-at-a-time bookkeeping that
// guarantees each speak callback fires exactly once. The per-OS parts work on
// native handles so the WebView and CEF backends of one OS share them:
// overlay_mac.mm (NSWindow), overlay_win.cc (HWND), overlay_linux.cc
// (GtkWindow / X11 Window), speech_{mac,win,linux}.

#ifndef LAUFEY_OVERLAY_H_
#define LAUFEY_OVERLAY_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "laufey.h"

namespace laufey_common {

// --- Input regions ----------------------------------------------------------

// A window's input region: whether one is set, and its rectangles (content
// DIPs). An active region with no rectangles lets every click through.
struct InputRegion {
  bool active = false;
  std::vector<laufey_rect_t> rects;
};

// The region set_input_region describes: `rects` NULL is "no region"; empty
// rectangles (width or height <= 0) are dropped.
InputRegion MakeInputRegion(const laufey_rect_t* rects, size_t count);

// Whether the point (content DIPs from the top-left) takes input under
// `region`: always without a region, else inside one of its rectangles.
bool RegionTakesInput(const InputRegion& region, double x, double y);

// Whether a window passes mouse input through right now: explicit
// passthrough (set_click_passthrough) wins; else the region decides for the
// cursor at (x, y) in content DIPs, and a cursor outside the window
// (`inside_window` false) passes through when a region is set, so the first
// event that reaches the window over a rectangle is already its own.
bool ShouldPassThrough(bool explicit_passthrough, const InputRegion& region,
                       bool inside_window, double x, double y);

// --- Levels and the activation policy --------------------------------------

bool IsValidWindowLevel(int32_t level);

// The LAUFEY_BACKGROUND_THROTTLING_* policy that create_window_ex `flags`
// ask for (API 51): BACKGROUND_UNTHROTTLED wins over BACKGROUND_SUSPEND, and
// neither is THROTTLE.
int32_t BackgroundThrottlingFromFlags(uint32_t flags);

// The policy named by the launch configuration's "activationPolicy"
// ("regular", "accessory", "prohibited"), or -1 when it names none.
int32_t ParseActivationPolicy(const std::string& value);

// The launch policy: the launch config's "activationPolicy" when it names
// one, else LAUFEY_ACTIVATION_POLICY_ACCESSORY when `ls_ui_element` (the
// bundle's LSUIElement), else REGULAR.
int32_t LaunchActivationPolicy(const std::string& launch_value,
                               bool ls_ui_element);

// --- Text to speech ---------------------------------------------------------

struct SpeechRequest {
  std::string text;
  std::string voice;    // empty: the default voice
  std::string lang;     // empty: the voice's own
  double rate = 1.0;    // 0.1 .. 10
  double pitch = 1.0;   // 0 .. 2
  double volume = 1.0;  // 0 .. 1
};

// Parses speak's request JSON; false without a non-empty "text" (or for
// text over 32 KiB). Numbers out of range are clamped.
bool ParseSpeechRequest(const char* json, SpeechRequest* out);

// One utterance at a time: Begin returns a new id and cancels the one
// before it (its callback reports CANCELLED); Finish reports an utterance
// once (later reports, and reports for a superseded id, are dropped).
// Thread-safe. A callback runs outside the lock.
class SpeechTracker {
 public:
  uint32_t Begin(laufey_speech_fn callback, void* user_data);
  // Reports `id` with `status` / `message` if it is still pending.
  void Finish(uint32_t id, int32_t status, const std::string& message);
  // Cancels the current utterance, if any.
  void CancelCurrent();
  uint32_t current() const;

 private:
  struct Pending {
    uint32_t id = 0;
    laufey_speech_fn callback = nullptr;
    void* user_data = nullptr;
  };
  mutable std::mutex mutex_;
  uint32_t next_id_ = 1;
  Pending current_;
};

SpeechTracker& Speech();

// The platform engines (speech_mac.mm, speech_win.cc, speech_linux.cc;
// speech.cc where there is none). SpeakNative must eventually call
// Speech().Finish(id, ...) for every id it is given; StopSpeakingNative
// stops the engine (Finish then reports CANCELLED for the id it was given).
bool SpeechAvailableNative();
void SpeakNative(uint32_t id, const SpeechRequest& request);
void StopSpeakingNative();
std::string SpeechVoicesJsonNative();

// The speak / stop_speaking / speech_voices ABI entry points, shared by
// every backend. Speak validates the request, begins it in Speech() and
// hands it to the engine (or reports UNSUPPORTED).
uint32_t Speak(const char* request_json, laufey_speech_fn callback,
               void* user_data);
void StopSpeaking();
// Freed by the caller with free() (the backends' string_free).
char* SpeechVoicesJson();

#if defined(__APPLE__)
// --- macOS (overlay_mac.mm). Main thread unless noted. ---------------------
// `nswindow` is an NSWindow* (bridged, unretained).

// Explicit click passthrough (set_click_passthrough): recorded here so the
// input region's toggling never mistakes its own state for it.
void MacSetClickPassthrough(void* nswindow, bool enabled);
bool MacIsClickPassthrough(void* nswindow);
// Sets or clears (region.active false) the input region.
void MacSetInputRegion(void* nswindow, const InputRegion& region);
// Drops the window's region state (call when it closes).
void MacForgetOverlayWindow(void* nswindow);
bool MacSetVisibleOnAllWorkspaces(void* nswindow, bool visible,
                                  bool visible_on_full_screen);
bool MacSetWindowLevel(void* nswindow, int32_t level);
// Any thread.
bool MacSetActivationPolicy(int32_t policy);
int32_t MacGetActivationPolicy();
// The policy a host starts in (LaunchActivationPolicy over the launch config
// and the main bundle's LSUIElement). Call it where the host used to force
// NSApplicationActivationPolicyRegular.
void MacApplyLaunchActivationPolicy();
#endif

#if defined(_WIN32)
// --- Windows (overlay_win.cc). UI thread. ----------------------------------
// `hwnd` is the top-level HWND.

// Explicit click passthrough, as on macOS: WS_EX_TRANSPARENT | WS_EX_LAYERED,
// kept apart from the region's own toggling.
void WinSetClickPassthrough(void* hwnd, bool enabled);
bool WinIsClickPassthrough(void* hwnd);
void WinSetInputRegion(void* hwnd, const InputRegion& region);
void WinForgetOverlayWindow(void* hwnd);
bool WinSetSkipTaskbar(void* hwnd, bool skip);
bool WinSetWindowLevel(void* hwnd, int32_t level);
// A transparent top-level window for WebView2 (DWM blur-behind with an
// empty region: per-pixel alpha from the child's composition).
void WinMakeWindowTransparent(void* hwnd);
#endif

#if defined(__linux__)
// --- Linux (overlay_linux.cc). UI thread. ----------------------------------
// Each call takes the top-level as a GtkWindow* (`gtk_window`, the WebView
// backend; it keeps the setting across realize / unmap) or, when that is
// NULL, a GdkWindow* (`gdk_window`, a foreign wrapper of CEF's X11 window);
// `key` identifies the window across calls (its window id or XID). `scale`
// converts content DIPs to the GdkWindow's coordinates (1 where GDK already
// scales). X11 and Wayland through GDK.

// Explicit click passthrough and the input region, kept apart: passthrough
// wins (an empty input shape); else the region's rectangles; else the
// default shape (the whole window).
// LinuxSetClickPassthrough's `scale` <= 0 reuses the region's.
void LinuxSetClickPassthrough(uint64_t key, void* gtk_window, void* gdk_window,
                              bool enabled, double scale);
bool LinuxIsClickPassthrough(uint64_t key);
void LinuxSetInputRegion(uint64_t key, void* gtk_window, void* gdk_window,
                         const InputRegion& region, double scale);
void LinuxForgetOverlayWindow(uint64_t key);
// GtkWindow* only (GDK's toplevel hints crash on a foreign window: CEF
// sends the EWMH client messages itself, main_linux.cc).
// The skip-taskbar and skip-pager hints; false on Wayland (no protocol).
bool LinuxSetSkipTaskbar(void* gtk_window, bool skip);
// Sticky (every workspace); false on Wayland.
bool LinuxSetVisibleOnAllWorkspaces(void* gtk_window, bool visible);
// keep-above for every level over NORMAL.
bool LinuxSetWindowLevel(void* gtk_window, int32_t level);
// Whether the default display is Wayland.
bool LinuxIsWayland();

// wlr-layer-shell through gtk-layer-shell (dlopen'd, optional).
// Whether the session is Wayland with a compositor that has
// zwlr_layer_shell_v1 and libgtk-layer-shell loads (cached).
bool LayerShellAvailable();
// Makes an unrealized GtkWindow* an overlay layer surface (anchored top-left,
// exclusive zone 0). The keyboard: none for a non-activating window (it never
// takes focus), else on demand (focus when clicked, as a popover panel needs
// for its fields and for a blur when the user clicks elsewhere; gtk-layer-shell
// 0.6+, none before). False when unavailable.
bool LayerShellInitOverlay(void* gtk_window, bool non_activating);
// Whether the GtkWindow* is a layer surface.
bool LayerShellIsLayerWindow(void* gtk_window);
// Moves a layer surface (its margins from the top-left anchor). False if
// the window is not one.
bool LayerShellMove(void* gtk_window, int x, int y);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_OVERLAY_H_
