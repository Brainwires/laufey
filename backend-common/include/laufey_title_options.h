// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Title visibility and toolbar style (API 57): the rules every backend
// shares, with no OS API, so they are tested once (title_options_test.cc).
//
// A window's title bar is described by three choices:
//   - its style (LAUFEY_TITLEBAR_*: set_window_titlebar_style),
//   - whether the title text shows (LAUFEY_TITLE_*:
//     set_window_title_visibility),
//   - the toolbar style (LAUFEY_TOOLBAR_STYLE_*: set_window_toolbar_style,
//     macOS 11+).
// TitleOptions keeps them per window, TitleOptionsCompatible says whether
// they can be shown together (the HIDDEN style has no toolbar, so a toolbar
// style other than AUTOMATIC is refused there: HIDDEN_INSET is that look),
// and MacTitleChromeFor turns them into the NSWindow settings the macOS
// backends apply (window_mac.mm). The title visibility is remembered under
// HIDDEN / HIDDEN_INSET, which never draw the title, and applies again when
// the window goes back to DEFAULT.
//
// LinuxShouldTryHideTitle is the Linux rule: only a title bar GTK draws
// itself (client-side decorations) can hide its title; under a window
// manager's server-side decorations the request is refused, never faked by
// switching the window to client-side decorations.
//
// Header-only.

#ifndef LAUFEY_TITLE_OPTIONS_H_
#define LAUFEY_TITLE_OPTIONS_H_

#include "laufey.h"

namespace laufey_common {

struct TitleOptions {
  int titlebar_style = LAUFEY_TITLEBAR_DEFAULT;
  int title_visibility = LAUFEY_TITLE_VISIBLE;
  int toolbar_style = LAUFEY_TOOLBAR_STYLE_AUTOMATIC;
};

inline bool IsTitlebarStyle(int style) {
  return style == LAUFEY_TITLEBAR_DEFAULT || style == LAUFEY_TITLEBAR_HIDDEN ||
         style == LAUFEY_TITLEBAR_HIDDEN_INSET;
}

inline bool IsTitleVisibility(int visibility) {
  return visibility == LAUFEY_TITLE_VISIBLE ||
         visibility == LAUFEY_TITLE_HIDDEN;
}

inline bool IsToolbarStyle(int style) {
  return style >= LAUFEY_TOOLBAR_STYLE_AUTOMATIC &&
         style <= LAUFEY_TOOLBAR_STYLE_EXPANDED;
}

// Whether the three choices can be shown together: every value known, and no
// toolbar style but AUTOMATIC under the HIDDEN title bar style.
inline bool TitleOptionsCompatible(const TitleOptions& o) {
  if (!IsTitlebarStyle(o.titlebar_style) ||
      !IsTitleVisibility(o.title_visibility) ||
      !IsToolbarStyle(o.toolbar_style))
    return false;
  return !(o.titlebar_style == LAUFEY_TITLEBAR_HIDDEN &&
           o.toolbar_style != LAUFEY_TOOLBAR_STYLE_AUTOMATIC);
}

// The options with one choice changed, or false (and `out` untouched) when
// the result is not TitleOptionsCompatible: what each setter checks before
// it changes the window.
inline bool WithTitlebarStyle(const TitleOptions& o, int style,
                              TitleOptions* out) {
  TitleOptions next = o;
  next.titlebar_style = style;
  if (!TitleOptionsCompatible(next))
    return false;
  *out = next;
  return true;
}

inline bool WithTitleVisibility(const TitleOptions& o, int visibility,
                                TitleOptions* out) {
  TitleOptions next = o;
  next.title_visibility = visibility;
  if (!TitleOptionsCompatible(next))
    return false;
  *out = next;
  return true;
}

inline bool WithToolbarStyle(const TitleOptions& o, int style,
                             TitleOptions* out) {
  TitleOptions next = o;
  next.toolbar_style = style;
  if (!TitleOptionsCompatible(next))
    return false;
  *out = next;
  return true;
}

// NSWindowToolbarStyle's values (AppKit, macOS 11), so this header needs no
// AppKit.
constexpr int kNSToolbarStyleAutomatic = 0;
constexpr int kNSToolbarStyleExpanded = 1;
constexpr int kNSToolbarStyleUnified = 3;
constexpr int kNSToolbarStyleUnifiedCompact = 4;

inline int NSToolbarStyleFor(int toolbar_style) {
  switch (toolbar_style) {
    case LAUFEY_TOOLBAR_STYLE_UNIFIED:
      return kNSToolbarStyleUnified;
    case LAUFEY_TOOLBAR_STYLE_UNIFIED_COMPACT:
      return kNSToolbarStyleUnifiedCompact;
    case LAUFEY_TOOLBAR_STYLE_EXPANDED:
      return kNSToolbarStyleExpanded;
    default:
      return kNSToolbarStyleAutomatic;
  }
}

// What an NSWindow shows for compatible options.
struct MacTitleChrome {
  bool full_size_content = false;     // NSWindowStyleMaskFullSizeContentView
  bool transparent_titlebar = false;  // titlebarAppearsTransparent
  bool title_hidden = false;          // titleVisibility = NSWindowTitleHidden
  bool has_toolbar = false;           // an empty NSToolbar is attached
  int ns_toolbar_style = kNSToolbarStyleAutomatic;  // NSWindow.toolbarStyle
};

inline MacTitleChrome MacTitleChromeFor(const TitleOptions& o) {
  MacTitleChrome c;
  switch (o.titlebar_style) {
    case LAUFEY_TITLEBAR_HIDDEN:
    case LAUFEY_TITLEBAR_HIDDEN_INSET:
      c.full_size_content = true;
      c.transparent_titlebar = true;
      // A transparent title bar over the page never draws the title.
      c.title_hidden = true;
      // HIDDEN_INSET's empty toolbar makes the title bar taller and insets
      // the traffic lights (Electron's hiddenInset); its style follows the
      // window's toolbar style.
      c.has_toolbar = o.titlebar_style == LAUFEY_TITLEBAR_HIDDEN_INSET;
      c.ns_toolbar_style =
          c.has_toolbar ? NSToolbarStyleFor(o.toolbar_style)
                        : kNSToolbarStyleAutomatic;
      break;
    default:
      c.title_hidden = o.title_visibility == LAUFEY_TITLE_HIDDEN;
      c.has_toolbar = o.toolbar_style != LAUFEY_TOOLBAR_STYLE_AUTOMATIC;
      c.ns_toolbar_style = NSToolbarStyleFor(o.toolbar_style);
      break;
  }
  return c;
}

// Linux: whether hiding the title can be attempted on a window, given whether
// GTK draws its title bar (client-side decorations) and whether it is
// frameless. Showing the title again is always accepted where nothing was
// hidden.
inline bool LinuxShouldTryHideTitle(bool client_side_decorated,
                                    bool frameless) {
  return client_side_decorated && !frameless;
}

}  // namespace laufey_common

#endif  // LAUFEY_TITLE_OPTIONS_H_
