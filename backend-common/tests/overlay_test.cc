// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_overlay.h (API 50): input-region hit testing, window levels, the
// launch activation policy, the speech request and the one-utterance
// tracker. Plain values, no OS.

#include "laufey_overlay.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;

struct Report {
  uint32_t id;
  int32_t status;
  std::string message;
};

static void Record(void* user_data, uint32_t id, int32_t status,
                   const char* message) {
  static_cast<std::vector<Report>*>(user_data)->push_back(
      {id, status, message ? message : "(null)"});
}

static void TestInputRegion() {
  // NULL rects: no region, every point takes input.
  InputRegion none = MakeInputRegion(nullptr, 0);
  EXPECT(!none.active);
  EXPECT(RegionTakesInput(none, -5, -5));
  EXPECT(!ShouldPassThrough(false, none, false, 0, 0));
  EXPECT(ShouldPassThrough(true, none, true, 0, 0));

  // Empty rectangles are dropped; an active region without one lets every
  // click through.
  laufey_rect_t rects[] = {{10, 20, 30, 40}, {0, 0, 0, 5}, {100, 0, 5, -1}};
  InputRegion r = MakeInputRegion(rects, 3);
  EXPECT(r.active && r.rects.size() == 1);
  EXPECT(RegionTakesInput(r, 10, 20));  // top-left corner: inside
  EXPECT(RegionTakesInput(r, 39.5, 59.5));
  EXPECT(!RegionTakesInput(r, 40, 30));  // right edge: outside
  EXPECT(!RegionTakesInput(r, 20, 60));  // bottom edge: outside
  EXPECT(!RegionTakesInput(r, 9.9, 25));
  laufey_rect_t only_empty[] = {{0, 0, 0, 0}};
  InputRegion closed = MakeInputRegion(only_empty, 1);
  EXPECT(closed.active && closed.rects.empty());
  EXPECT(!RegionTakesInput(closed, 0, 0));

  // Explicit passthrough wins; a cursor outside the window passes through
  // when a region is set (the next event over a rectangle is the window's).
  EXPECT(ShouldPassThrough(true, r, true, 15, 25));
  EXPECT(!ShouldPassThrough(false, r, true, 15, 25));
  EXPECT(ShouldPassThrough(false, r, true, 5, 5));
  EXPECT(ShouldPassThrough(false, r, false, 15, 25));
}

static void TestLevelsAndPolicy() {
  EXPECT(IsValidWindowLevel(LAUFEY_WINDOW_LEVEL_NORMAL));
  EXPECT(IsValidWindowLevel(LAUFEY_WINDOW_LEVEL_SCREEN_SAVER));
  EXPECT(!IsValidWindowLevel(-1));
  EXPECT(!IsValidWindowLevel(5));

  // The page's background policy from the creation flags; the other flags
  // don't touch it.
  EXPECT(BackgroundThrottlingFromFlags(0) ==
         LAUFEY_BACKGROUND_THROTTLING_THROTTLE);
  EXPECT(BackgroundThrottlingFromFlags(LAUFEY_WINDOW_FLAG_HIDDEN |
                                       LAUFEY_WINDOW_FLAG_FRAMELESS) ==
         LAUFEY_BACKGROUND_THROTTLING_THROTTLE);
  EXPECT(BackgroundThrottlingFromFlags(
             LAUFEY_WINDOW_FLAG_BACKGROUND_UNTHROTTLED) ==
         LAUFEY_BACKGROUND_THROTTLING_NONE);
  EXPECT(BackgroundThrottlingFromFlags(LAUFEY_WINDOW_FLAG_BACKGROUND_SUSPEND |
                                       LAUFEY_WINDOW_FLAG_HIDDEN) ==
         LAUFEY_BACKGROUND_THROTTLING_SUSPEND);
  EXPECT(
      BackgroundThrottlingFromFlags(LAUFEY_WINDOW_FLAG_BACKGROUND_UNTHROTTLED |
                                    LAUFEY_WINDOW_FLAG_BACKGROUND_SUSPEND) ==
      LAUFEY_BACKGROUND_THROTTLING_NONE);

  EXPECT(ParseActivationPolicy("regular") == LAUFEY_ACTIVATION_POLICY_REGULAR);
  EXPECT(ParseActivationPolicy("accessory") ==
         LAUFEY_ACTIVATION_POLICY_ACCESSORY);
  EXPECT(ParseActivationPolicy("prohibited") ==
         LAUFEY_ACTIVATION_POLICY_PROHIBITED);
  EXPECT(ParseActivationPolicy("Accessory") == -1);
  EXPECT(ParseActivationPolicy("") == -1);
  // The launch key wins over LSUIElement, LSUIElement over the default.
  EXPECT(LaunchActivationPolicy("", false) == LAUFEY_ACTIVATION_POLICY_REGULAR);
  EXPECT(LaunchActivationPolicy("", true) ==
         LAUFEY_ACTIVATION_POLICY_ACCESSORY);
  EXPECT(LaunchActivationPolicy("regular", true) ==
         LAUFEY_ACTIVATION_POLICY_REGULAR);
  EXPECT(LaunchActivationPolicy("accessory", false) ==
         LAUFEY_ACTIVATION_POLICY_ACCESSORY);
  EXPECT(LaunchActivationPolicy("bogus", true) ==
         LAUFEY_ACTIVATION_POLICY_ACCESSORY);
}

// Raw strings stay outside EXPECT: MSVC's preprocessor can't stringize a raw
// string literal that holds a quote.
static bool Parse(const char* json, SpeechRequest* r) {
  return ParseSpeechRequest(json, r);
}

static void TestSpeechRequest() {
  SpeechRequest r;
  const char* full =
      R"({"text":"Hi \"there\"","voice":"v1","lang":"en-US","rate":1.5,)"
      R"("pitch":0.25,"volume":0.5})";
  EXPECT(Parse(full, &r));
  EXPECT(r.text == "Hi \"there\"" && r.voice == "v1" && r.lang == "en-US");
  EXPECT(r.rate == 1.5 && r.pitch == 0.25 && r.volume == 0.5);
  // Defaults and clamping.
  const char* minimal = R"({"text":"x"})";
  EXPECT(Parse(minimal, &r));
  EXPECT(r.rate == 1.0 && r.pitch == 1.0 && r.volume == 1.0 && r.voice.empty());
  const char* out_of_range =
      R"({"text":"x","rate":100,"pitch":-3,"volume":2e9})";
  EXPECT(Parse(out_of_range, &r));
  EXPECT(r.rate == 10.0 && r.pitch == 0.0 && r.volume == 1.0);
  const char* zero_rate = R"({"text":"x","rate":0})";
  EXPECT(Parse(zero_rate, &r));
  EXPECT(r.rate == 0.1);
  // A key nested in an object is not the top-level one.
  const char* nested = R"({"extra":{"rate":5},"text":"x"})";
  EXPECT(Parse(nested, &r));
  EXPECT(r.rate == 1.0);
  // Refused: no text, empty text, not a string, not an object, malformed,
  // too long.
  const char* refused[] = {R"({"voice":"v"})", R"({"text":""})",
                           R"({"text":5})", R"(["text"])", R"({"text":"x")"};
  for (const char* json : refused)
    EXPECT(!Parse(json, &r));
  EXPECT(!Parse(nullptr, &r));
  std::string big = "{\"text\":\"" + std::string(33 * 1024, 'a') + "\"}";
  EXPECT(!Parse(big.c_str(), &r));
}

static void TestSpeechTracker() {
  SpeechTracker t;
  std::vector<Report> reports;
  uint32_t a = t.Begin(Record, &reports);
  EXPECT(a != 0 && t.current() == a);
  // A second utterance cancels the first, once.
  uint32_t b = t.Begin(Record, &reports);
  EXPECT(b != a && t.current() == b);
  EXPECT(reports.size() == 1 && reports[0].id == a &&
         reports[0].status == LAUFEY_SPEECH_CANCELLED);
  // The engine's late report for the superseded one is dropped.
  t.Finish(a, LAUFEY_SPEECH_DONE, "");
  EXPECT(reports.size() == 1);
  t.Finish(b, LAUFEY_SPEECH_FAILED, "why");
  EXPECT(reports.size() == 2 && reports[1].id == b &&
         reports[1].status == LAUFEY_SPEECH_FAILED &&
         reports[1].message == "why");
  // Reported once only; nothing current any more.
  t.Finish(b, LAUFEY_SPEECH_DONE, "");
  t.CancelCurrent();
  EXPECT(reports.size() == 2 && t.current() == 0);
  uint32_t c = t.Begin(Record, &reports);
  t.CancelCurrent();
  EXPECT(reports.size() == 3 && reports[2].id == c &&
         reports[2].status == LAUFEY_SPEECH_CANCELLED &&
         reports[2].message.empty());
  // An invalid request is refused before anything begins.
  const char* empty_text = R"({"text":""})";
  const char* valid = R"({"text":"x"})";
  EXPECT(Speak(empty_text, Record, &reports) == 0);
  EXPECT(Speak(valid, nullptr, nullptr) == 0);
  EXPECT(reports.size() == 3);
}

int main() {
  TestInputRegion();
  TestLevelsAndPolicy();
  TestSpeechRequest();
  TestSpeechTracker();
  std::printf("overlay_test: all passed\n");
  return 0;
}
