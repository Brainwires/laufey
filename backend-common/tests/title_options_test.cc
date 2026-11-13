// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_title_options.h (API 57): which title bar style / title visibility
// / toolbar style combinations are accepted, and the NSWindow chrome each
// one gives. No OS.

#include "laufey_title_options.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;

static void TestValues() {
  EXPECT(IsTitleVisibility(LAUFEY_TITLE_VISIBLE));
  EXPECT(IsTitleVisibility(LAUFEY_TITLE_HIDDEN));
  EXPECT(!IsTitleVisibility(2) && !IsTitleVisibility(-1));
  for (int s = LAUFEY_TOOLBAR_STYLE_AUTOMATIC;
       s <= LAUFEY_TOOLBAR_STYLE_EXPANDED; ++s)
    EXPECT(IsToolbarStyle(s));
  EXPECT(!IsToolbarStyle(4) && !IsToolbarStyle(-1));
  EXPECT(!IsTitlebarStyle(3));
  // The header's numbers are the ABI: never renumber them.
  EXPECT(LAUFEY_TOOLBAR_STYLE_UNIFIED == 1);
  EXPECT(LAUFEY_TOOLBAR_STYLE_UNIFIED_COMPACT == 2);
  EXPECT(LAUFEY_TOOLBAR_STYLE_EXPANDED == 3);
  EXPECT(LAUFEY_WINDOW_CAP_TITLE_HIDDEN == (1u << 23));
  EXPECT(LAUFEY_WINDOW_CAP_TOOLBAR_STYLE == (1u << 24));
}

static void TestCompatibility() {
  TitleOptions o;
  EXPECT(TitleOptionsCompatible(o));
  // Every toolbar style works under DEFAULT and HIDDEN_INSET.
  for (int style : {LAUFEY_TITLEBAR_DEFAULT, LAUFEY_TITLEBAR_HIDDEN_INSET}) {
    for (int t = LAUFEY_TOOLBAR_STYLE_AUTOMATIC;
         t <= LAUFEY_TOOLBAR_STYLE_EXPANDED; ++t) {
      TitleOptions c{style, LAUFEY_TITLE_HIDDEN, t};
      EXPECT(TitleOptionsCompatible(c));
    }
  }
  // HIDDEN has no toolbar: only AUTOMATIC.
  EXPECT(TitleOptionsCompatible(
      {LAUFEY_TITLEBAR_HIDDEN, LAUFEY_TITLE_VISIBLE,
       LAUFEY_TOOLBAR_STYLE_AUTOMATIC}));
  EXPECT(!TitleOptionsCompatible(
      {LAUFEY_TITLEBAR_HIDDEN, LAUFEY_TITLE_VISIBLE,
       LAUFEY_TOOLBAR_STYLE_UNIFIED}));
  // Unknown values.
  EXPECT(!TitleOptionsCompatible({7, 0, 0}));
  EXPECT(!TitleOptionsCompatible({0, 5, 0}));
  EXPECT(!TitleOptionsCompatible({0, 0, 9}));
}

static void TestSetters() {
  TitleOptions o;
  TitleOptions next;
  // A toolbar style, then HIDDEN is refused and nothing changes.
  EXPECT(WithToolbarStyle(o, LAUFEY_TOOLBAR_STYLE_UNIFIED, &next));
  o = next;
  TitleOptions untouched = o;
  EXPECT(!WithTitlebarStyle(o, LAUFEY_TITLEBAR_HIDDEN, &untouched));
  EXPECT(untouched.titlebar_style == LAUFEY_TITLEBAR_DEFAULT);
  // HIDDEN_INSET is fine with it.
  EXPECT(WithTitlebarStyle(o, LAUFEY_TITLEBAR_HIDDEN_INSET, &next));
  EXPECT(next.toolbar_style == LAUFEY_TOOLBAR_STYLE_UNIFIED);
  // Under HIDDEN a toolbar style is refused; AUTOMATIC is accepted.
  TitleOptions hidden{LAUFEY_TITLEBAR_HIDDEN, LAUFEY_TITLE_VISIBLE,
                      LAUFEY_TOOLBAR_STYLE_AUTOMATIC};
  EXPECT(!WithToolbarStyle(hidden, LAUFEY_TOOLBAR_STYLE_EXPANDED, &next));
  EXPECT(WithToolbarStyle(hidden, LAUFEY_TOOLBAR_STYLE_AUTOMATIC, &next));
  // The title visibility is accepted (and kept) under every style.
  EXPECT(WithTitleVisibility(hidden, LAUFEY_TITLE_HIDDEN, &next));
  EXPECT(next.title_visibility == LAUFEY_TITLE_HIDDEN);
  EXPECT(!WithTitleVisibility(hidden, 3, &next));
}

static void TestMacChrome() {
  // DEFAULT, title shown, no toolbar: the window as AppKit makes it.
  MacTitleChrome c = MacTitleChromeFor({});
  EXPECT(!c.full_size_content && !c.transparent_titlebar && !c.title_hidden);
  EXPECT(!c.has_toolbar && c.ns_toolbar_style == kNSToolbarStyleAutomatic);

  // DEFAULT, title hidden: the native bar stays, only the text goes.
  c = MacTitleChromeFor(
      {LAUFEY_TITLEBAR_DEFAULT, LAUFEY_TITLE_HIDDEN,
       LAUFEY_TOOLBAR_STYLE_AUTOMATIC});
  EXPECT(c.title_hidden && !c.full_size_content && !c.transparent_titlebar);
  EXPECT(!c.has_toolbar);

  // DEFAULT + UNIFIED: an empty toolbar in the unified style, title shown.
  c = MacTitleChromeFor(
      {LAUFEY_TITLEBAR_DEFAULT, LAUFEY_TITLE_VISIBLE,
       LAUFEY_TOOLBAR_STYLE_UNIFIED});
  EXPECT(c.has_toolbar && c.ns_toolbar_style == kNSToolbarStyleUnified);
  EXPECT(!c.title_hidden && !c.full_size_content);
  c = MacTitleChromeFor(
      {LAUFEY_TITLEBAR_DEFAULT, LAUFEY_TITLE_VISIBLE,
       LAUFEY_TOOLBAR_STYLE_UNIFIED_COMPACT});
  EXPECT(c.ns_toolbar_style == kNSToolbarStyleUnifiedCompact);
  c = MacTitleChromeFor(
      {LAUFEY_TITLEBAR_DEFAULT, LAUFEY_TITLE_VISIBLE,
       LAUFEY_TOOLBAR_STYLE_EXPANDED});
  EXPECT(c.ns_toolbar_style == kNSToolbarStyleExpanded);

  // HIDDEN: transparent, content under it, no toolbar, title never drawn,
  // whatever the visibility says.
  c = MacTitleChromeFor(
      {LAUFEY_TITLEBAR_HIDDEN, LAUFEY_TITLE_VISIBLE,
       LAUFEY_TOOLBAR_STYLE_AUTOMATIC});
  EXPECT(c.full_size_content && c.transparent_titlebar && c.title_hidden);
  EXPECT(!c.has_toolbar);

  // HIDDEN_INSET: its toolbar, in AppKit's automatic style by default (the
  // look before API 57), else the window's toolbar style.
  c = MacTitleChromeFor(
      {LAUFEY_TITLEBAR_HIDDEN_INSET, LAUFEY_TITLE_VISIBLE,
       LAUFEY_TOOLBAR_STYLE_AUTOMATIC});
  EXPECT(c.has_toolbar && c.ns_toolbar_style == kNSToolbarStyleAutomatic);
  EXPECT(c.title_hidden && c.full_size_content);
  c = MacTitleChromeFor(
      {LAUFEY_TITLEBAR_HIDDEN_INSET, LAUFEY_TITLE_VISIBLE,
       LAUFEY_TOOLBAR_STYLE_UNIFIED_COMPACT});
  EXPECT(c.has_toolbar && c.ns_toolbar_style == kNSToolbarStyleUnifiedCompact);
}

static void TestLinux() {
  EXPECT(LinuxShouldTryHideTitle(true, false));
  // A window manager's decorations: refused, never faked.
  EXPECT(!LinuxShouldTryHideTitle(false, false));
  EXPECT(!LinuxShouldTryHideTitle(true, true));
}

int main() {
  TestValues();
  TestCompatibility();
  TestSetters();
  TestMacChrome();
  TestLinux();
  std::printf("title_options_test: ok\n");
  return 0;
}
