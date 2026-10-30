// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Overlay windows on Windows (API 50); see laufey_overlay.h.
//
// Click passthrough is WS_EX_TRANSPARENT on a layered top-level window (it
// takes the children, the web view's host windows, with it). An input region
// toggles the same style from a 60 Hz cursor timer on the window's own thread:
// the window is transparent to the mouse while the cursor is outside the
// region's rectangles, so the next click over one lands in the window. The
// region is input-only: nothing is clipped. (SetWindowRgn would clip the
// painting too, and WM_NCHITTEST's HTTRANSPARENT passes clicks only to
// windows of the same thread.)

#include "laufey_overlay.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>

#include <map>

namespace laufey_common {

namespace {

constexpr UINT_PTR kRegionTimerId = 0x4C4F5652;  // 'LOVR'
constexpr UINT kRegionTimerMs = 16;
constexpr UINT_PTR kSkipTaskbarSubclassId = 0x4C534B54;  // 'LSKT'

struct WinOverlayState {
  bool explicit_passthrough = false;
  InputRegion region;
  bool timer = false;
  bool skip_taskbar = false;
};

// UI thread only.
std::map<HWND, WinOverlayState>& States() {
  static auto* map = new std::map<HWND, WinOverlayState>();
  return *map;
}

void ApplyPassthrough(HWND hwnd, bool pass) {
  LONG ex = GetWindowLong(hwnd, GWL_EXSTYLE);
  if (pass) {
    if ((ex & (WS_EX_TRANSPARENT | WS_EX_LAYERED)) ==
        (WS_EX_TRANSPARENT | WS_EX_LAYERED))
      return;
    bool newly_layered = !(ex & WS_EX_LAYERED);
    SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_LAYERED);
    if (newly_layered) {
      // A window that just became layered renders nothing until its
      // attributes are set; fully opaque keeps it as it was.
      SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    }
    return;
  }
  if (!(ex & WS_EX_TRANSPARENT))
    return;
  LONG new_ex = ex & ~WS_EX_TRANSPARENT;
  // Keep the layered style while a window opacity below 1 needs it.
  BYTE alpha = 255;
  DWORD flags = 0;
  bool has_alpha = (ex & WS_EX_LAYERED) &&
                   GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) &&
                   (flags & LWA_ALPHA) && alpha < 255;
  if (!has_alpha)
    new_ex &= ~WS_EX_LAYERED;
  SetWindowLong(hwnd, GWL_EXSTYLE, new_ex);
  if ((ex & WS_EX_LAYERED) && !(new_ex & WS_EX_LAYERED)) {
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
  }
}

void Recompute(HWND hwnd, const WinOverlayState& st) {
  bool pass = st.explicit_passthrough;
  if (!pass && st.region.active) {
    POINT cursor;
    RECT client;
    bool inside = false;
    double x = 0, y = 0;
    if (IsWindowVisible(hwnd) && GetCursorPos(&cursor) &&
        GetClientRect(hwnd, &client)) {
      POINT origin = {0, 0};
      ClientToScreen(hwnd, &origin);
      UINT dpi = GetDpiForWindow(hwnd);
      double scale = dpi ? dpi / 96.0 : 1.0;
      x = (cursor.x - origin.x) / scale;
      y = (cursor.y - origin.y) / scale;
      inside = cursor.x >= origin.x && cursor.y >= origin.y &&
               cursor.x < origin.x + client.right &&
               cursor.y < origin.y + client.bottom;
    }
    pass = ShouldPassThrough(false, st.region, inside, x, y);
  }
  ApplyPassthrough(hwnd, pass);
}

void CALLBACK RegionTimer(HWND hwnd, UINT, UINT_PTR, DWORD) {
  auto it = States().find(hwnd);
  if (it == States().end() || !IsWindow(hwnd)) {
    KillTimer(hwnd, kRegionTimerId);
    if (it != States().end())
      States().erase(it);
    return;
  }
  Recompute(hwnd, it->second);
}

void UpdateTimer(HWND hwnd, WinOverlayState& st) {
  bool want = st.region.active && !st.explicit_passthrough;
  if (want && !st.timer) {
    st.timer = SetTimer(hwnd, kRegionTimerId, kRegionTimerMs, RegionTimer) != 0;
  } else if (!want && st.timer) {
    KillTimer(hwnd, kRegionTimerId);
    st.timer = false;
  }
}

ITaskbarList* Taskbar() {
  static ITaskbarList* list = [] {
    ITaskbarList* l = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l))) ||
        !l)
      return (ITaskbarList*)nullptr;
    if (FAILED(l->HrInit())) {
      l->Release();
      return (ITaskbarList*)nullptr;
    }
    return l;
  }();
  return list;
}

// Windows adds a taskbar button again whenever a window is shown: a window
// kept off the taskbar deletes it again after each show.
LRESULT CALLBACK SkipTaskbarSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR, DWORD_PTR) {
  static const UINT kDeleteTab = RegisterWindowMessageW(L"LaufeySkipTaskbar");
  if (msg == WM_SHOWWINDOW && wp) {
    // The shell adds the button after the show: delete it once the show's
    // messages are through.
    PostMessage(hwnd, kDeleteTab, 0, 0);
  } else if (msg == kDeleteTab && kDeleteTab) {
    if (ITaskbarList* list = Taskbar())
      list->DeleteTab(hwnd);
    return 0;
  }
  if (msg == WM_NCDESTROY) {
    RemoveWindowSubclass(hwnd, SkipTaskbarSubclass, kSkipTaskbarSubclassId);
    States().erase(hwnd);
  }
  return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

void WinSetClickPassthrough(void* hwnd_ptr, bool enabled) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd)
    return;
  WinOverlayState& st = States()[hwnd];
  st.explicit_passthrough = enabled;
  UpdateTimer(hwnd, st);
  Recompute(hwnd, st);
}

bool WinIsClickPassthrough(void* hwnd_ptr) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  auto it = States().find(hwnd);
  if (it != States().end())
    return it->second.explicit_passthrough;
  return hwnd && (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
}

void WinSetInputRegion(void* hwnd_ptr, const InputRegion& region) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd)
    return;
  WinOverlayState& st = States()[hwnd];
  st.region = region;
  UpdateTimer(hwnd, st);
  Recompute(hwnd, st);
}

void WinForgetOverlayWindow(void* hwnd_ptr) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  auto it = States().find(hwnd);
  if (it == States().end())
    return;
  if (it->second.timer)
    KillTimer(hwnd, kRegionTimerId);
  States().erase(it);
}

bool WinSetSkipTaskbar(void* hwnd_ptr, bool skip) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  ITaskbarList* list = Taskbar();
  if (!hwnd || !list)
    return false;
  WinOverlayState& st = States()[hwnd];
  if (skip && !st.skip_taskbar) {
    SetWindowSubclass(hwnd, SkipTaskbarSubclass, kSkipTaskbarSubclassId, 0);
  } else if (!skip && st.skip_taskbar) {
    RemoveWindowSubclass(hwnd, SkipTaskbarSubclass, kSkipTaskbarSubclassId);
  }
  st.skip_taskbar = skip;
  HRESULT hr = skip ? list->DeleteTab(hwnd) : list->AddTab(hwnd);
  return SUCCEEDED(hr);
}

bool WinSetWindowLevel(void* hwnd_ptr, int32_t level) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd || !IsValidWindowLevel(level))
    return false;
  HWND after =
      level == LAUFEY_WINDOW_LEVEL_NORMAL ? HWND_NOTOPMOST : HWND_TOPMOST;
  return SetWindowPos(hwnd, after, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
}

void WinMakeWindowTransparent(void* hwnd_ptr) {
  HWND hwnd = static_cast<HWND>(hwnd_ptr);
  if (!hwnd)
    return;
  // Blur-behind over an empty region turns on per-pixel alpha for the
  // window without blurring anything: what the window (and the web view's
  // composition over it) leaves at alpha 0 shows the desktop.
  HRGN region = CreateRectRgn(0, 0, -1, -1);
  DWM_BLURBEHIND bb = {};
  bb.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
  bb.fEnable = TRUE;
  bb.hRgnBlur = region;
  DwmEnableBlurBehindWindow(hwnd, &bb);
  if (region)
    DeleteObject(region);
}

}  // namespace laufey_common
