// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the CEF host's network-quiet defaults
// (src/cef_network_quiet.cc). No test framework; exits non-zero if any
// expectation fails.

#include "laufey_cef_network_quiet.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

using namespace laufey_common;

static int g_failures = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

using Names = std::vector<std::string>;

static const char kQuiet[] =
    "NetworkTimeServiceQuerying,AimEnabled,PreconnectToSearch";
static const char kOwn[] =
    "GlicActorUi,AutofillActorMode,LensOverlay,KillOnInvalidNavigationHeaders";
static const std::string kAll = std::string(kQuiet) + "," + kOwn;

static void TestFeatureListNames() {
  EXPECT(FeatureListNames("") == Names{});
  EXPECT(FeatureListNames(",, ,") == Names{});
  EXPECT((FeatureListNames("A,B") == Names{"A", "B"}));
  // Field-trial suffixes, params and the "*" prefix are not part of the name.
  EXPECT((FeatureListNames("*A<Trial,B:x/1/y/2,C<T.G:p/v, D ") ==
          Names{"A", "B", "C", "D"}));
}

static void TestMerge() {
  // Nothing on the command line: every default, in order.
  EXPECT(MergeQuietDisabledFeatures("", "") == kAll);
  EXPECT(CefQuietDisabledFeatures().size() == 3);
  EXPECT(CefOwnDisabledFeatures().size() == 4);
  // The app's own disabled features come first and are kept as written.
  EXPECT(MergeQuietDisabledFeatures("Foo<Trial", "") == "Foo<Trial," + kAll);
  EXPECT(MergeQuietDisabledFeatures("Foo,", "") == "Foo," + kAll);
  // A default already disabled is not repeated.
  EXPECT(MergeQuietDisabledFeatures("AimEnabled", "") ==
         std::string("AimEnabled,NetworkTimeServiceQuerying,"
                     "PreconnectToSearch,") +
             kOwn);
  EXPECT(MergeQuietDisabledFeatures(kAll, "") == kAll);
  EXPECT(MergeQuietDisabledFeatures(kOwn, "") ==
         std::string(kOwn) + "," + kQuiet);
  // An app that enables one of them keeps it: the explicit choice wins.
  EXPECT(MergeQuietDisabledFeatures("", "PreconnectToSearch") ==
         std::string("NetworkTimeServiceQuerying,AimEnabled,") + kOwn);
  EXPECT(MergeQuietDisabledFeatures("", "OverlayScrollbar,*AimEnabled<T") ==
         std::string("NetworkTimeServiceQuerying,PreconnectToSearch,") + kOwn);
  EXPECT(MergeQuietDisabledFeatures("X", kAll) == "X");
}

// CEF's own default --disable-features entries (features that crash Chrome's
// browser initialization or break CEF APIs), spelled out here rather than
// read from CefOwnDisabledFeatures() so a list that loses one fails too.
static const char* const kCefOwn[] = {"GlicActorUi", "AutofillActorMode",
                                      "LensOverlay",
                                      "KillOnInvalidNavigationHeaders"};

static void TestDeepLinkKeepsCefDefaults() {
  // A deep-link launch whose command line carries --disable-features (a
  // relaunch copies Chromium's processed switches next to the link) drops
  // the switch (LaufeyStripDeepLinkSwitches), and with it the entries CEF
  // had merged into it before the command line hook. The hook's merge must
  // put CEF's back, or Chrome crashes at startup
  // (tabs::TabInterface::GetFromContents).
  const std::string after_strip = MergeQuietDisabledFeatures("", "");
  const Names names = FeatureListNames(after_strip);
  for (const char* feature : kCefOwn) {
    if (std::find(names.begin(), names.end(), feature) == names.end()) {
      std::fprintf(stderr, "FAIL: %s missing after a deep-link strip (%s)\n",
                   feature, after_strip.c_str());
      ++g_failures;
    }
  }
  EXPECT(CefOwnDisabledFeatures() ==
         Names(std::begin(kCefOwn), std::end(kCefOwn)));
  // Nothing is lost when the switch survives either (an ordinary launch:
  // CEF's entries are already there and are not repeated).
  EXPECT(FeatureListNames(MergeQuietDisabledFeatures(kOwn, "")).size() ==
         4 + CefQuietDisabledFeatures().size());
}

static void TestGaiaUrl() {
  // An http(s) origin with no path: what Chromium's --gaia-url accepts.
  EXPECT(std::string(kCefQuietGaiaUrl) == "https://localhost:9");
}

static void TestLocalHunspellDictionaries() {
  const Names files = {"en-US-10-1.bdic", "DE-de-3-0.BDIC", "fr-FR.bdic",
                       "es-ES-3-0.bdic.tmp", "Custom Dictionary.txt"};
  // Present (case does not matter) and kept in the profile's order.
  EXPECT((LocalHunspellDictionaries({"de-DE", "en-US"}, files) ==
          Names{"de-DE", "en-US"}));
  // No file, an unversioned name or a partial download: dropped.
  EXPECT(LocalHunspellDictionaries({"fr-FR", "es-ES", "it-IT"}, files) ==
         Names{});
  // A language is not matched by a longer one's file.
  EXPECT(LocalHunspellDictionaries({"en"}, files) == Names{});
  EXPECT(LocalHunspellDictionaries({"en-US"}, {}) == Names{});
  EXPECT(LocalHunspellDictionaries({}, files) == Names{});
  EXPECT(LocalHunspellDictionaries({""}, {"-1-0.bdic"}) == Names{});
}

static void TestDictionariesToSeed() {
  const Names bundled = {"en-US-10-1.bdic", "de-DE-3-0.bdic", "fr-FR-3-0.bdic",
                         "README_en_US.txt", "it-IT.bdic"};
  // Only the languages in use, in the profile's order, and only the
  // missing ones.
  EXPECT((DictionariesToSeed({"de-DE", "en-US"}, bundled, {}) ==
          Names{"de-DE-3-0.bdic", "en-US-10-1.bdic"}));
  EXPECT((DictionariesToSeed({"de-DE", "en-US"}, bundled,
                             {"EN-us-10-1.BDIC", "Custom Dictionary.txt"}) ==
          Names{"de-DE-3-0.bdic"}));
  // Nothing bundled for it, or not a versioned dictionary: not seeded.
  EXPECT(DictionariesToSeed({"es-ES", "it-IT"}, bundled, {}) == Names{});
  // A language is not served by a longer one's file.
  EXPECT(DictionariesToSeed({"en"}, bundled, {}) == Names{});
  // A language listed twice is seeded once.
  EXPECT((DictionariesToSeed({"fr-FR", "fr-FR"}, bundled, {}) ==
          Names{"fr-FR-3-0.bdic"}));
  // The app's folder first, then the runtime's: the app's file wins.
  const Names app = {"en-US-10-1.bdic"};
  Names have = DictionariesToSeed({"en-US", "de-DE"}, app, {});
  EXPECT((have == Names{"en-US-10-1.bdic"}));
  EXPECT((DictionariesToSeed({"en-US", "de-DE"}, bundled, have) ==
          Names{"de-DE-3-0.bdic"}));
  // What gets seeded is then kept by LocalHunspellDictionaries.
  EXPECT((LocalHunspellDictionaries(
              {"de-DE"}, DictionariesToSeed({"de-DE"}, bundled, {})) ==
          Names{"de-DE"}));
}

int main() {
  TestFeatureListNames();
  TestMerge();
  TestDeepLinkKeepsCefDefaults();
  TestGaiaUrl();
  TestLocalHunspellDictionaries();
  TestDictionariesToSeed();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("cef_network_quiet_test: all passed\n");
  return 0;
}
