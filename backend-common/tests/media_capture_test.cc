// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_media_capture.h (API 53): the launch file's "mediaCapture" key, who
// may capture, and the requests waiting for the runtime (each answered
// exactly once). Plain strings, no engine. Plain asserts, no framework.

#include "laufey_media_capture.h"

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

namespace {

constexpr uint32_t kMic = LAUFEY_MEDIA_CAPTURE_MICROPHONE;
constexpr uint32_t kCam = LAUFEY_MEDIA_CAPTURE_CAMERA;

MediaCapturePolicy PolicyOf(const std::string& json,
                            std::vector<std::string>* warnings = nullptr) {
  return MediaCapturePolicyFrom(ParseLaunchConfig(json, warnings));
}

struct Asked {
  uint32_t window_id;
  uint64_t request_id;
  std::string origin;
  uint32_t kinds;
};

std::vector<Asked>& AskedLog() {
  static std::vector<Asked> log;
  return log;
}

void RecordAsk(uint32_t window_id, uint64_t request_id, const char* origin,
               uint32_t kinds, void* user_data) {
  EXPECT(user_data == &AskedLog());
  AskedLog().push_back({window_id, request_id, origin ? origin : "", kinds});
}

}  // namespace

int main() {
  const MediaCaptureDecision kAllow = MediaCaptureDecision::kAllow;
  const MediaCaptureDecision kDeny = MediaCaptureDecision::kDeny;
  const MediaCaptureDecision kAsk = MediaCaptureDecision::kAsk;

  // --- The launch file's key -------------------------------------------------
  {
    std::vector<std::string> w;
    LaunchConfig c = ParseLaunchConfig(
        R"({"mediaCapture":{"microphone":["MyApp://App/","https://a.example:443"],)"
        R"("camera":["myapp://app"]}})",
        &w);
    EXPECT(w.empty());
    EXPECT(c.has_media_capture);
    EXPECT(c.media_capture_microphone.size() == 2);
    EXPECT(c.media_capture_microphone[0] == "myapp://app");
    EXPECT(c.media_capture_microphone[1] == "https://a.example");
    EXPECT(c.media_capture_camera.size() == 1);
  }
  {
    // Wildcards, paths, opaque origins, non-strings and unknown kinds are
    // reported and skipped; the valid entries are kept.
    std::vector<std::string> w;
    LaunchConfig c = ParseLaunchConfig(
        R"({"mediaCapture":{"microphone":["*","myapp://*","https://a.example/x",)"
        R"("data:text/html,x","",7,"myapp://app"],"screen":["myapp://app"],)"
        R"("camera":"myapp://app"}})",
        &w);
    EXPECT(c.has_media_capture);
    EXPECT(c.media_capture_microphone.size() == 1);
    EXPECT(c.media_capture_microphone[0] == "myapp://app");
    EXPECT(c.media_capture_camera.empty());
    EXPECT(w.size() == 8);
  }
  {
    std::vector<std::string> w;
    LaunchConfig c =
        ParseLaunchConfig(R"({"mediaCapture":["myapp://app"]})", &w);
    EXPECT(!c.has_media_capture && w.size() == 1);
    EXPECT(!ParseLaunchConfig("{}", nullptr).has_media_capture);
  }

  // --- Who may capture -------------------------------------------------------
  {
    const MediaCapturePolicy none = PolicyOf("{}");
    const MediaCapturePolicy p =
        PolicyOf(R"({"mediaCapture":{"microphone":["myapp://app"],)"
                 R"("camera":["myapp://app","http://127.0.0.1:5173"]}})");
    const std::string app = "myapp://app";

    // Default deny: no launch file grant, no handler.
    EXPECT(DecideMediaCapture(none, app, app, kMic, false) == kDeny);
    EXPECT(DecideMediaCapture(none, app, app, kMic | kCam, false) == kDeny);
    // ... and with a handler, it is asked.
    EXPECT(DecideMediaCapture(none, app, app, kMic, true) == kAsk);

    // The granted origin gets what it was granted, without asking.
    EXPECT(DecideMediaCapture(p, app, app, kMic, false) == kAllow);
    EXPECT(DecideMediaCapture(p, app, app, kCam, false) == kAllow);
    EXPECT(DecideMediaCapture(p, app, app, kMic | kCam, true) == kAllow);
    // A kind it was not granted is not given with the other.
    EXPECT(DecideMediaCapture(p, "http://127.0.0.1:5173",
                              "http://127.0.0.1:5173", kCam, false) == kAllow);
    EXPECT(DecideMediaCapture(p, "http://127.0.0.1:5173",
                              "http://127.0.0.1:5173", kMic | kCam,
                              false) == kDeny);
    EXPECT(DecideMediaCapture(p, "http://127.0.0.1:5173",
                              "http://127.0.0.1:5173", kMic | kCam,
                              true) == kAsk);

    // Exact origins: another host, port or scheme is not the granted one.
    for (const char* other :
         {"myapp://app2", "myapp://ap", "otherapp://app", "myapp://app:8080",
          "https://app", "http://127.0.0.1:5174"}) {
      EXPECT(DecideMediaCapture(p, other, other, kMic, false) == kDeny);
    }

    // A sub-frame of another origin is refused, even with a handler, and
    // even when its own origin is granted (the top document's is not).
    EXPECT(DecideMediaCapture(p, app, "https://evil.example", kMic, true) ==
           kDeny);
    EXPECT(DecideMediaCapture(p, "https://evil.example", app, kMic, true) ==
           kDeny);
    // Opaque and empty origins are refused.
    EXPECT(DecideMediaCapture(p, kOpaqueOrigin, kOpaqueOrigin, kMic, true) ==
           kDeny);
    EXPECT(DecideMediaCapture(p, "", "", kMic, true) == kDeny);

    // Nothing asked for, screen capture, or an unknown bit: refused.
    EXPECT(DecideMediaCapture(p, app, app, 0, true) == kDeny);
    EXPECT(DecideMediaCapture(p, app, app, kMic | kMediaCaptureOther, true) ==
           kDeny);
    EXPECT(DecideMediaCapture(p, app, app, 1u << 2, true) == kDeny);
  }

  // --- The requests waiting for the runtime ----------------------------------
  {
    MediaCaptureRequests requests;
    const MediaCapturePolicy p =
        PolicyOf(R"({"mediaCapture":{"microphone":["myapp://app"]}})");
    std::vector<std::pair<int, bool>> replies;
    auto reply_for = [&](int tag) {
      return [&replies, tag](bool allow) { replies.push_back({tag, allow}); };
    };

    // No handler: answered at once, refused unless granted.
    EXPECT(requests.Request(p, 1, "myapp://app", "myapp://app", kMic,
                            reply_for(1)) == kAllow);
    EXPECT(requests.Request(p, 1, "myapp://app", "myapp://app", kCam,
                            reply_for(2)) == kDeny);
    EXPECT(replies.size() == 2 && replies[0] == std::make_pair(1, true) &&
           replies[1] == std::make_pair(2, false));
    EXPECT(requests.PendingCount() == 0);

    // With a handler: the rest is asked, with the origin and kinds.
    requests.SetHandler(RecordAsk, &AskedLog());
    EXPECT(requests.HandlerSet());
    EXPECT(requests.Request(p, 7, "myapp://app", "myapp://app", kCam,
                            reply_for(3)) == kAsk);
    EXPECT(requests.Request(p, 8, "myapp://app", "myapp://app", kMic | kCam,
                            reply_for(4)) == kAsk);
    EXPECT(requests.Request(p, 8, "myapp://app", "myapp://app", kMic,
                            reply_for(5)) == kAllow);
    EXPECT(requests.Request(p, 8, "myapp://app", "https://evil.example", kMic,
                            reply_for(6)) == kDeny);
    EXPECT(AskedLog().size() == 2);
    EXPECT(AskedLog()[0].window_id == 7 && AskedLog()[0].kinds == kCam &&
           AskedLog()[0].origin == "myapp://app");
    EXPECT(AskedLog()[1].window_id == 8 &&
           AskedLog()[1].kinds == (kMic | kCam));
    EXPECT(AskedLog()[0].request_id != AskedLog()[1].request_id);
    EXPECT(requests.PendingCount() == 2);
    replies.clear();

    // An answer settles its request once; a second answer finds nothing.
    EXPECT(requests.Answer(AskedLog()[0].request_id, true));
    EXPECT(!requests.Answer(AskedLog()[0].request_id, false));
    EXPECT(!requests.Answer(9999, true));
    EXPECT(replies.size() == 1 && replies[0] == std::make_pair(3, true));

    // Closing a window refuses what it waits on, and only that.
    EXPECT(requests.Request(p, 9, "myapp://app", "myapp://app", kCam,
                            reply_for(7)) == kAsk);
    requests.ForgetWindow(8);
    EXPECT(replies.size() == 2 && replies[1] == std::make_pair(4, false));
    EXPECT(!requests.Answer(AskedLog()[1].request_id, true));
    EXPECT(requests.PendingCount() == 1);

    // Clearing the handler refuses the rest, and later requests are refused
    // without asking.
    requests.SetHandler(nullptr, nullptr);
    EXPECT(!requests.HandlerSet());
    EXPECT(replies.size() == 3 && replies[2] == std::make_pair(7, false));
    EXPECT(requests.PendingCount() == 0);
    EXPECT(requests.Request(p, 9, "myapp://app", "myapp://app", kCam,
                            reply_for(8)) == kDeny);
    EXPECT(AskedLog().size() == 3);
    EXPECT(replies.size() == 4 && replies[3] == std::make_pair(8, false));

    // A reply may itself start another request (an engine that asks again).
    requests.SetHandler(RecordAsk, &AskedLog());
    bool nested = false;
    EXPECT(
        requests.Request(p, 10, "myapp://app", "myapp://app", kCam, [&](bool) {
          nested = requests.Request(p, 10, "myapp://app", "myapp://app", kMic,
                                    nullptr) == kAllow;
        }) == kAsk);
    EXPECT(requests.Answer(AskedLog().back().request_id, false));
    EXPECT(nested);
  }

  // --- A window's top-level document is replaced ----------------------------
  {
    MediaCaptureRequests requests;
    const MediaCapturePolicy p = PolicyOf(R"({})");
    std::vector<std::pair<int, bool>> replies;
    auto reply_for = [&](int tag) {
      return [&replies, tag](bool allow) { replies.push_back({tag, allow}); };
    };
    AskedLog().clear();
    requests.SetHandler(RecordAsk, &AskedLog());
    EXPECT(requests.Request(p, 20, "https://a.example", "https://a.example",
                            kMic, reply_for(1)) == kAsk);
    EXPECT(requests.Request(p, 20, "https://a.example", "https://a.example",
                            kCam, reply_for(2)) == kAsk);
    EXPECT(requests.Request(p, 21, "https://a.example", "https://a.example",
                            kMic, reply_for(3)) == kAsk);
    EXPECT(AskedLog().size() == 3);

    // Window 20's main frame commits another document: what it waits on is
    // refused at once, and a late yes reaches nothing (the new page included).
    requests.ForgetDocument(20);
    EXPECT(replies.size() == 2 && replies[0] == std::make_pair(1, false) &&
           replies[1] == std::make_pair(2, false));
    EXPECT(!requests.Answer(AskedLog()[0].request_id, true));
    EXPECT(!requests.Answer(AskedLog()[1].request_id, true));
    EXPECT(replies.size() == 2);
    // Another window's request still waits, and its answer still lands.
    EXPECT(requests.PendingCount() == 1);
    EXPECT(requests.Answer(AskedLog()[2].request_id, true));
    EXPECT(replies.size() == 3 && replies[2] == std::make_pair(3, true));

    // The new document's own requests are decided as usual.
    EXPECT(requests.Request(p, 20, "https://b.example", "https://b.example",
                            kMic, reply_for(4)) == kAsk);
    EXPECT(requests.Answer(AskedLog().back().request_id, true));
    EXPECT(replies.size() == 4 && replies[3] == std::make_pair(4, true));
    // A window with nothing waiting: nothing happens.
    requests.ForgetDocument(20);
    requests.ForgetDocument(99);
    EXPECT(replies.size() == 4 && requests.PendingCount() == 0);
    requests.SetHandler(nullptr, nullptr);
  }

  // --- At answer time: the window's top-level origin is re-checked ----------
  {
    const std::string a = "https://a.example";
    EXPECT(MediaCaptureGrantStands(true, a, a));
    // The top-level document moved to another origin, or to an opaque one.
    EXPECT(!MediaCaptureGrantStands(true, a, "https://b.example"));
    EXPECT(!MediaCaptureGrantStands(true, a, kOpaqueOrigin));
    EXPECT(!MediaCaptureGrantStands(true, a, ""));
    EXPECT(!MediaCaptureGrantStands(true, a, "https://a.example:8443"));
    // A request made for no origin never stands.
    EXPECT(!MediaCaptureGrantStands(true, kOpaqueOrigin, kOpaqueOrigin));
    EXPECT(!MediaCaptureGrantStands(true, "", ""));
    // A refusal stays one.
    EXPECT(!MediaCaptureGrantStands(false, a, a));
  }

  // --- A sub-frame's origin, from its own navigations (WebView2) ------------
  {
    const std::string a = "https://a.example";
    const std::string b = "https://b.example";
    SubFrameOrigin f;
    // Nothing committed yet (the initial about:blank): unknown.
    EXPECT(f.RequestingOrigin() == kOpaqueOrigin);
    f.NavigationStarting(1, b);
    EXPECT(f.RequestingOrigin() == kOpaqueOrigin);
    // A redirect to the top-level document's origin: the commit is a's.
    f.NavigationStarting(1, a);
    f.Committed(1);
    EXPECT(f.RequestingOrigin() == a);
    // A navigation to another origin that never commits (a 204) leaves a's
    // document, still a's; one that commits is b's.
    f.NavigationStarting(2, b);
    EXPECT(f.RequestingOrigin() == a);
    f.NavigationStarting(3, b);
    f.Committed(3);
    EXPECT(f.RequestingOrigin() == b);
    // Starting a's URL and not committing it never makes the frame a's.
    f.NavigationStarting(4, a);
    EXPECT(f.RequestingOrigin() == b);
    // A commit the frame never saw start: unknown.
    f.Committed(5);
    EXPECT(f.RequestingOrigin() == kOpaqueOrigin);
    // Once the frame has a child frame, its requests can't be told apart.
    SubFrameOrigin g;
    g.NavigationStarting(1, a);
    g.Committed(1);
    EXPECT(g.RequestingOrigin() == a);
    g.ChildFrameCreated();
    EXPECT(g.RequestingOrigin() == kOpaqueOrigin);
    g.NavigationStarting(2, a);
    g.Committed(2);
    EXPECT(g.RequestingOrigin() == kOpaqueOrigin);
    // Decided as any request: a's frame in a's window is asked about, b's
    // refused.
    EXPECT(DecideMediaCapture(MediaCapturePolicy{}, a, f.RequestingOrigin(),
                              kMic, true) == kDeny);
  }

  std::printf("media_capture_test: ok\n");
  return 0;
}
