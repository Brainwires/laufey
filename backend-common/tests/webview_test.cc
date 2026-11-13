// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_webview.h (API 58): create_webview's options, the URL and partition
// rules, the event / state JSON, the evaluate envelope and the registry. No
// OS.

#include "laufey_webview.h"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <set>
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

static int32_t Parse(const std::string& json, WebViewOptions* o,
                     std::string* error = nullptr) {
  std::string e;
  int32_t status = ParseWebViewOptions(json, o, &e);
  if (error)
    *error = e;
  return status;
}

static void TestAbiNumbers() {
  // The header's numbers are the ABI: never renumber them.
  EXPECT(LAUFEY_WEBVIEW_OK == 0 && LAUFEY_WEBVIEW_NOT_SERIALIZABLE == 9);
  EXPECT(LAUFEY_WEBVIEW_LIMIT == 3 && LAUFEY_WEBVIEW_CLOSED == 6);
  EXPECT(LAUFEY_WEBVIEW_EVENT_TITLE == 1 && LAUFEY_WEBVIEW_EVENT_CLOSED == 7);
  EXPECT(LAUFEY_WEBVIEW_COMMAND_BACK == 1 && LAUFEY_WEBVIEW_COMMAND_FOCUS == 6);
  EXPECT(LAUFEY_WEBVIEW_ENGINE_SHIFT == 24);
  EXPECT(LAUFEY_WEBVIEW_ENGINE_MASK == (0xfu << 24));
  EXPECT(LAUFEY_WEBVIEW_MAX_PER_WINDOW == 32);
}

static void TestOptionsDefaults() {
  WebViewOptions o;
  EXPECT(Parse(R"({"url":"https://github.com/","x":10,"y":20.5,)"
               R"("width":300,"height":200})",
               &o) == LAUFEY_WEBVIEW_OK);
  EXPECT(o.url == "https://github.com/");
  EXPECT(o.bounds.x == 10 && o.bounds.y == 20.5);
  EXPECT(o.bounds.width == 300 && o.bounds.height == 200);
  EXPECT(o.partition == "webview:default");
  EXPECT(o.visible);
  EXPECT(o.popups == WebViewPopups::kSameView);
  EXPECT(o.downloads == WebViewDownloads::kDeny);
  EXPECT(o.permissions == WebViewPermissions::kDeny);
  EXPECT(o.user_agent.empty());
}

static void TestOptionsAll() {
  WebViewOptions o;
  EXPECT(Parse(R"({"url":"http://127.0.0.1:8080/x?y#z","x":0,"y":0,)"
               R"("width":1.5e2,"height":0,"partition":"ephemeral:tile-1",)"
               R"("visible":false,"allowPopups":"deny","downloads":"ask",)"
               R"("permissions":"ask","userAgent":"Bot/1"})",
               &o) == LAUFEY_WEBVIEW_OK);
  EXPECT(o.bounds.width == 150 && o.bounds.height == 0);
  EXPECT(o.partition == "ephemeral:tile-1" && !o.visible);
  EXPECT(o.popups == WebViewPopups::kDeny);
  EXPECT(o.downloads == WebViewDownloads::kAsk);
  EXPECT(o.permissions == WebViewPermissions::kAsk);
  EXPECT(o.user_agent == "Bot/1");
  EXPECT(Parse(R"({"url":"https://a.b/","x":0,"y":0,"width":1,"height":1,)"
               R"("allowPopups":"browser","downloads":"browser",)"
               R"("partition":null})",
               &o) == LAUFEY_WEBVIEW_OK);
  EXPECT(o.popups == WebViewPopups::kBrowser);
  EXPECT(o.downloads == WebViewDownloads::kBrowser);
  EXPECT(o.partition == "webview:default");
}

static void TestOptionsRefused() {
  WebViewOptions o;
  std::string e;
  const char* invalid[] = {
      "",
      "[]",
      R"({"x":0,"y":0,"width":1,"height":1})",
      R"({"url":1,"x":0,"y":0,"width":1,"height":1})",
      R"({"url":"https://a/","y":0,"width":1,"height":1})",
      R"({"url":"https://a/","x":"0","y":0,"width":1,"height":1})",
      R"({"url":"https://a/","x":0,"y":0,"width":-1,"height":1})",
      R"({"url":"https://a/","x":0,"y":0,"width":1,"height":2e6})",
      R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,"visible":1})",
      R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,)"
      R"("allowPopups":"tab"})",
      R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,)"
      R"("downloads":"save"})",
      R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,)"
      R"("permissions":"allow"})",
      R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,)"
      R"("partition":""})",
      R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,)"
      R"("partition":"../app"})",
      R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,)"
      R"("userAgent":"a\nb"})",
      // A bad option is INVALID even with a bad URL.
      R"({"url":"file:///etc","x":0,"y":0,"width":1,"height":1,)"
      R"("downloads":"x"})",
  };
  for (const char* json : invalid) {
    if (Parse(json, &o, &e) != LAUFEY_WEBVIEW_INVALID) {
      std::fprintf(stderr, "not INVALID: %s\n", json);
      std::exit(1);
    }
    EXPECT(!e.empty());
  }
  const char* urls[] = {"file:///etc/hosts", "javascript:alert(1)",
                        "data:text/html,x",  "about:blank",
                        "myapp://app/",      "https:///nohost",
                        "https://a b/",      "ftp://a/",
                        "mailto:a@b.c",      "http://"};
  for (const char* url : urls) {
    std::string json = std::string(R"({"url":")") + url +
                       R"(","x":0,"y":0,"width":1,"height":1})";
    if (Parse(json, &o, &e) != LAUFEY_WEBVIEW_NOT_ALLOWED_URL) {
      std::fprintf(stderr, "not NOT_ALLOWED_URL: %s\n", url);
      std::exit(1);
    }
  }
}

static void TestNumbersIgnoreTheLocale() {
  // A GTK process calls setlocale(LC_ALL, ""); a comma-decimal locale must
  // not change how "2.5" reads or how state JSON is written.
  const char* locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8"};
  for (const char* l : locales) {
    if (std::setlocale(LC_NUMERIC, l))
      break;
  }
  WebViewOptions o;
  EXPECT(Parse(R"({"url":"https://a/","x":2.5,"y":-0.25,"width":1e1,)"
               R"("height":3.125})",
               &o) == LAUFEY_WEBVIEW_OK);
  EXPECT(o.bounds.x == 2.5 && o.bounds.y == -0.25);
  EXPECT(o.bounds.width == 10 && o.bounds.height == 3.125);
  WebViewStateFields s;
  s.bounds = {2.5, 0, 100, 33.3333333};
  std::string json = WebViewStateJson(s);
  EXPECT(
      json.find(R"("bounds":{"x":2.5,"y":0,"width":100,"height":33.3333})") !=
      std::string::npos);
  std::setlocale(LC_NUMERIC, "C");
}

static void TestUrlsAndPartitions() {
  EXPECT(IsAllowedWebViewUrl("https://github.com/denoland"));
  EXPECT(IsAllowedWebViewUrl("HTTP://LOCALHOST:3000/"));
  EXPECT(!IsAllowedWebViewUrl("about:blank"));
  EXPECT(IsAllowedWebViewNavigation("about:blank"));
  EXPECT(IsAllowedWebViewNavigation("About:Srcdoc"));
  EXPECT(!IsAllowedWebViewNavigation("javascript:void(0)"));
  EXPECT(!IsAllowedWebViewNavigation("file:///"));
  EXPECT(!IsAllowedWebViewNavigation("blob:https://a/x"));

  EXPECT(IsValidPartitionName("webview:default"));
  EXPECT(IsValidPartitionName("a"));
  EXPECT(IsValidPartitionName("Fleet.github_1-2"));
  EXPECT(IsValidPartitionName(std::string(64, 'x')));
  EXPECT(!IsValidPartitionName(std::string(65, 'x')));
  EXPECT(!IsValidPartitionName(":a") && !IsValidPartitionName(".a"));
  EXPECT(!IsValidPartitionName("a/b") && !IsValidPartitionName("a b"));
  EXPECT(!IsValidPartitionName("a~b") && !IsValidPartitionName("é"));

  EXPECT(IsEphemeralPartition("ephemeral"));
  EXPECT(IsEphemeralPartition("ephemeral:"));
  EXPECT(IsEphemeralPartition("ephemeral:x"));
  EXPECT(!IsEphemeralPartition("ephemeralx"));
  EXPECT(!IsEphemeralPartition("webview:default"));

  EXPECT(PartitionDirName("webview:default") == "webview~default");
  EXPECT(PartitionDirName("a.b-c_d") == "a.b-c_d");

  // Stable per name, distinct across names, RFC 4122 version / variant.
  EXPECT(PartitionUuidString("webview:default") ==
         PartitionUuidString("webview:default"));
  std::set<std::string> seen;
  for (const char* n : {"a", "b", "webview:default", "webview~default", "A"})
    seen.insert(PartitionUuidString(n));
  EXPECT(seen.size() == 5);
  auto u = PartitionUuid("x");
  EXPECT((u[6] & 0xf0) == 0x50);
  EXPECT((u[8] & 0xc0) == 0x80);
  std::string s = PartitionUuidString("x");
  EXPECT(s.size() == 36 && s[8] == '-' && s[13] == '-' && s[14] == '5');
}

static void TestEventJson() {
  EXPECT(WebViewTitleJson("a \"b\"\n") == R"({"title":"a \"b\"\n"})");
  EXPECT(WebViewUrlJson("https://a/", true, false) ==
         R"({"url":"https://a/","canGoBack":true,"canGoForward":false})");
  EXPECT(WebViewLoadJson("loading", "https://a/") ==
         R"({"state":"loading","url":"https://a/"})");
  EXPECT(WebViewLoadJson("complete", "https://a/", "dns", "x") ==
         R"({"state":"complete","url":"https://a/"})");
  EXPECT(WebViewLoadJson("failed", "https://a/", "dns", "no host") ==
         R"({"state":"failed","url":"https://a/","error":{"code":"dns",)"
         R"("message":"no host"}})");
  EXPECT(WebViewLoadJson("failed", "u") ==
         R"({"state":"failed","url":"u","error":{"code":"failed",)"
         R"("message":""}})");
  EXPECT(
      WebViewConsoleJson("warn", "m", "https://a/x.js", 3) ==
      R"({"level":"warn","message":"m","source":"https://a/x.js","line":3})");
  EXPECT(WebViewConsoleJson("verbose", "m", "", 0) ==
         R"({"level":"log","message":"m"})");
  EXPECT(WebViewFocusJson(true) == R"({"focused":true})");
  EXPECT(WebViewNewWindowJson("https://a/", true) ==
         R"({"url":"https://a/","disposition":"popup"})");
  EXPECT(WebViewNewWindowJson("https://a/", false) ==
         R"({"url":"https://a/","disposition":"tab"})");
  WebViewStateFields s;
  s.url = "https://a/";
  s.title = "T";
  s.load = "interactive";
  s.can_go_back = true;
  s.visible = false;
  s.bounds = {1, 2, 3, 4};
  EXPECT(WebViewStateJson(s) ==
         R"({"url":"https://a/","title":"T","load":"interactive",)"
         R"("canGoBack":true,"canGoForward":false,"visible":false,)"
         R"("bounds":{"x":1,"y":2,"width":3,"height":4},"zOrder":0})");
}

static void TestEvalEnvelope() {
  std::string script = WebViewEvalScript("document.title + \"\\n\"");
  EXPECT(script.find("await (0, eval)(\"document.title + \\\"\\\\n\\\"\")") !=
         std::string::npos);
  EXPECT(script.find("kind: 'not_serializable'") != std::string::npos);
  std::string text;
  EXPECT(ParseWebViewEvalEnvelope(R"({"ok":true,"json":"{\"a\":1}"})", &text) ==
         LAUFEY_WEBVIEW_OK);
  EXPECT(text == R"({"a":1})");
  EXPECT(ParseWebViewEvalEnvelope(
             R"({"ok":false,"kind":"error","message":"Error: x"})", &text) ==
         LAUFEY_WEBVIEW_SCRIPT_ERROR);
  EXPECT(text == "Error: x");
  EXPECT(ParseWebViewEvalEnvelope(
             R"({"ok":false,"kind":"not_serializable","message":"a function"})",
             &text) == LAUFEY_WEBVIEW_NOT_SERIALIZABLE);
  EXPECT(text == "a function");
  EXPECT(ParseWebViewEvalEnvelope("2", &text) == LAUFEY_WEBVIEW_FAILED);
  EXPECT(ParseWebViewEvalEnvelope(R"({"ok":true})", &text) ==
         LAUFEY_WEBVIEW_FAILED);
  EXPECT(std::string(kWebViewSerializeFunction).rfind("(value) =>", 0) == 0);
}

struct Recorded {
  uint32_t id;
  int32_t kind;
  std::string detail;
};
static std::vector<Recorded> g_events;

static void Record(void* user_data, uint32_t id, int32_t kind,
                   const char* detail) {
  EXPECT(user_data == &g_events);
  g_events.push_back({id, kind, detail});
  // A sink may call back into the registry (the lock is recursive).
  (void)WebViewSinks::Instance().Has(id);
}

static void TestRegistry() {
  WebViewSinks& r = WebViewSinks::Instance();
  r.ResetForTest();
  g_events.clear();
  uint32_t a = r.Add(7, Record, &g_events);
  uint32_t b = r.Add(7, Record, &g_events);
  uint32_t c = r.Add(8, Record, &g_events);
  EXPECT(a == 1 && b == 2 && c == 3);
  EXPECT(r.WindowOf(b) == 7 && r.WindowOf(99) == 0);
  EXPECT(r.CountForWindow(7) == 2 && r.CountForWindow(8) == 1);
  r.Emit(a, LAUFEY_WEBVIEW_EVENT_TITLE, R"({"title":"x"})");
  EXPECT(g_events.size() == 1 && g_events[0].id == a);
  EXPECT(g_events[0].kind == LAUFEY_WEBVIEW_EVENT_TITLE);
  // CLOSED exactly once, last.
  EXPECT(r.Close(a));
  EXPECT(!r.Close(a));
  r.Emit(a, LAUFEY_WEBVIEW_EVENT_TITLE, "{}");
  EXPECT(g_events.size() == 2);
  EXPECT(g_events[1].kind == LAUFEY_WEBVIEW_EVENT_CLOSED);
  EXPECT(g_events[1].detail == "{}");
  // Emitting CLOSED closes.
  r.Emit(b, LAUFEY_WEBVIEW_EVENT_CLOSED, "{}");
  EXPECT(!r.Has(b) && g_events.size() == 3);
  // Abandon: no event.
  r.Abandon(c);
  EXPECT(!r.Has(c) && g_events.size() == 3);
  // Ids are never reused; the per-window limit.
  r.ResetForTest();
  for (int i = 0; i < LAUFEY_WEBVIEW_MAX_PER_WINDOW; ++i)
    EXPECT(r.Add(1, Record, &g_events) != 0);
  EXPECT(r.Add(1, Record, &g_events) == 0);
  EXPECT(r.Add(2, Record, &g_events) != 0);
  std::vector<uint32_t> views = r.ViewsOfWindow(1);
  EXPECT(views.size() == LAUFEY_WEBVIEW_MAX_PER_WINDOW);
  EXPECT(r.Close(views[0]));
  EXPECT(r.Add(1, Record, &g_events) == LAUFEY_WEBVIEW_MAX_PER_WINDOW + 2);
}

static void TestStacking() {
  WebViewSinks& r = WebViewSinks::Instance();
  r.ResetForTest();
  uint32_t a = r.Add(1, Record, &g_events);
  uint32_t b = r.Add(1, Record, &g_events);
  uint32_t c = r.Add(1, Record, &g_events);
  uint32_t other = r.Add(2, Record, &g_events);
  // New views start on top, per window.
  EXPECT(r.ZOrderOf(a) == 0 && r.ZOrderOf(b) == 1 && r.ZOrderOf(c) == 2);
  EXPECT(r.ZOrderOf(other) == 0);
  EXPECT((r.StackOrder(1) == std::vector<uint32_t>{a, b, c}));
  EXPECT(r.Raise(a));
  EXPECT(r.ZOrderOf(a) == 3);
  EXPECT((r.StackOrder(1) == std::vector<uint32_t>{b, c, a}));
  EXPECT(r.Lower(c));
  EXPECT(r.ZOrderOf(c) == 0);
  EXPECT((r.StackOrder(1) == std::vector<uint32_t>{c, b, a}));
  // Equal z keeps creation order.
  EXPECT(r.SetZOrder(a, 1) && r.SetZOrder(c, 1));
  EXPECT((r.StackOrder(1) == std::vector<uint32_t>{a, b, c}));
  EXPECT(!r.SetZOrder(999, 1) && !r.Raise(999) && !r.Lower(999));
  // Raising the only view (or the top one) keeps it on top.
  EXPECT(r.Raise(other) && r.ZOrderOf(other) == 0);
  WebViewStateFields s;
  s.z_order = -3;
  EXPECT(WebViewStateJson(s).find(R"("zOrder":-3})") != std::string::npos);
  r.ResetForTest();
}

static void TestHandleAdditions() {
  WebViewOptions o;
  EXPECT(Parse(R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1})",
               &o) == LAUFEY_WEBVIEW_OK);
  EXPECT(!o.app_context_menu && !o.clipboard_read && o.clipboard_write);
  EXPECT(Parse(R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,)"
               R"("contextMenu":"app","clipboardRead":true,)"
               R"("clipboardWrite":false})",
               &o) == LAUFEY_WEBVIEW_OK);
  EXPECT(o.app_context_menu && o.clipboard_read && !o.clipboard_write);
  EXPECT(Parse(R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,)"
               R"("contextMenu":"custom"})",
               &o) == LAUFEY_WEBVIEW_INVALID);
  EXPECT(Parse(R"({"url":"https://a/","x":0,"y":0,"width":1,"height":1,)"
               R"("clipboardRead":"yes"})",
               &o) == LAUFEY_WEBVIEW_INVALID);
  EXPECT(WebViewFindResultJson(3, 2) == R"({"matches":3,"activeMatch":2})");
  EXPECT(WebViewFindResultJson(0, 5) == R"({"matches":0,"activeMatch":0})");
  WebViewContextMenuFields f;
  f.x = 10.5;
  f.y = 20;
  f.link_url = "https://a/";
  f.is_editable = true;
  f.media_type = "image";
  EXPECT(WebViewContextMenuJson(f) ==
         R"({"x":10.5,"y":20,"linkUrl":"https://a/","isEditable":true,)"
         R"("mediaType":"image"})");
  EXPECT(WebViewContextMenuJson(WebViewContextMenuFields{}) ==
         R"({"x":0,"y":0,"isEditable":false})");
  EXPECT(IsValidWebViewZoom(1) && IsValidWebViewZoom(0.25) &&
         IsValidWebViewZoom(5));
  EXPECT(!IsValidWebViewZoom(0.2) && !IsValidWebViewZoom(5.1) &&
         !IsValidWebViewZoom(0.0 / 0.0));
  EXPECT(LAUFEY_WEBVIEW_NOT_ALLOWED == 10);
  EXPECT(LAUFEY_WEBVIEW_EVENT_FIND_RESULT == 8);
  EXPECT(LAUFEY_WEBVIEW_EVENT_CONTEXT_MENU == 9);
}

int main() {
  TestAbiNumbers();
  TestOptionsDefaults();
  TestOptionsAll();
  TestOptionsRefused();
  TestNumbersIgnoreTheLocale();
  TestUrlsAndPartitions();
  TestEventJson();
  TestEvalEnvelope();
  TestRegistry();
  TestStacking();
  TestHandleAdditions();
  std::printf("webview_test: ok\n");
  return 0;
}
