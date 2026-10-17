// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_downloads.h: the file name a download's save dialog proposes, which
// downloads a window may ask about (origins, prompt budget), and the dialog
// request AskDownloadDestination makes. Plain strings, no engine. Plain
// asserts, no framework.

#include "laufey_downloads.h"

#include <chrono>
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

int main() {
  // --- The proposed file name ------------------------------------------------
  EXPECT(SanitizeDownloadFileName("report.txt") == "report.txt");
  EXPECT(SanitizeDownloadFileName("r\xC3\xA9sum\xC3\xA9.pdf") ==
         "r\xC3\xA9sum\xC3\xA9.pdf");
  // Only the last path component: no directory from the page or the server.
  EXPECT(SanitizeDownloadFileName("../../etc/passwd") == "passwd");
  EXPECT(SanitizeDownloadFileName("..\\..\\Windows\\System32\\evil.dll") ==
         "evil.dll");
  EXPECT(SanitizeDownloadFileName("/etc/cron.d/x") == "x");
  EXPECT(SanitizeDownloadFileName("C:\\Users\\a\\x.txt") == "x.txt");
  EXPECT(SanitizeDownloadFileName("dir/") == "download");
  // Nothing left, or only dots: the default name.
  EXPECT(SanitizeDownloadFileName("") == "download");
  EXPECT(SanitizeDownloadFileName("..") == "download");
  EXPECT(SanitizeDownloadFileName(". . .") == "download");
  // No hidden file, no trailing dot or space (Windows drops them).
  EXPECT(SanitizeDownloadFileName(".bashrc") == "bashrc");
  EXPECT(SanitizeDownloadFileName("  a.txt.  ") == "a.txt");
  // Control characters and the characters Windows forbids.
  EXPECT(SanitizeDownloadFileName("a\nb\tc\x7f.txt") == "a_b_c_.txt");
  EXPECT(SanitizeDownloadFileName("a<b>c:d\"e|f?g*.txt") ==
         "a_b_c_d_e_f_g_.txt");
  EXPECT(SanitizeDownloadFileName(std::string("a\0b.txt", 7)) == "a_b.txt");
  // Invalid UTF-8, overlongs and surrogates.
  EXPECT(SanitizeDownloadFileName("a\xFF.txt") == "a_.txt");
  EXPECT(SanitizeDownloadFileName("a\xC0\xAF.txt") == "a__.txt");
  EXPECT(SanitizeDownloadFileName("a\xED\xA0\x80.txt") == "a___.txt");
  // A right-to-left override would show "evil\u202Etxt.exe" as "evilexe.txt".
  EXPECT(SanitizeDownloadFileName("evil\xE2\x80\xAEtxt.exe") ==
         "evil___txt.exe");
  // Windows device names.
  EXPECT(SanitizeDownloadFileName("CON") == "_CON");
  EXPECT(SanitizeDownloadFileName("nul.txt") == "_nul.txt");
  EXPECT(SanitizeDownloadFileName("com1.tar.gz") == "_com1.tar.gz");
  EXPECT(SanitizeDownloadFileName("console.txt") == "console.txt");
  EXPECT(SanitizeDownloadFileName("COM10") == "COM10");
  // Length: at most kMaxDownloadFileNameBytes, a short extension kept, never
  // a split UTF-8 sequence.
  {
    std::string long_name(300, 'a');
    std::string out = SanitizeDownloadFileName(long_name + ".csv");
    EXPECT(out.size() == kMaxDownloadFileNameBytes);
    EXPECT(out.substr(out.size() - 4) == ".csv");
    std::string wide;
    for (int i = 0; i < 150; ++i)
      wide += "\xC3\xA9";  // 2 bytes each
    out = SanitizeDownloadFileName(wide);
    EXPECT(out.size() <= kMaxDownloadFileNameBytes);
    EXPECT(out.size() % 2 == 0);
  }

  // --- Origins ---------------------------------------------------------------
  const std::string app = "app://localhost";
  const std::vector<std::string> none;
  const std::vector<std::string> api = {"https://api.example.com"};
  // The window's own origin, and blob: URLs it made.
  EXPECT(DownloadOriginAllowed(app, none, "app://localhost/export.csv", ""));
  EXPECT(DownloadOriginAllowed(app, none, "blob:app://localhost/3f2a-11", ""));
  // Another origin's file, or a blob another origin made: no.
  EXPECT(!DownloadOriginAllowed(app, none, "https://evil.example/x.exe", ""));
  EXPECT(!DownloadOriginAllowed(app, none, "blob:https://evil.example/1", ""));
  // ...unless the embedder allows that origin.
  EXPECT(DownloadOriginAllowed(app, api, "https://api.example.com/r.pdf", ""));
  // The frame that started it, when the engine says.
  EXPECT(
      DownloadOriginAllowed(app, none, "app://localhost/a", "app://localhost"));
  EXPECT(!DownloadOriginAllowed(app, none, "app://localhost/a",
                                "https://iframe.example"));
  EXPECT(!DownloadOriginAllowed(app, none, "data:text/plain,hi",
                                "https://iframe.example"));
  EXPECT(!DownloadOriginAllowed(app, none, "data:text/plain,hi", "null"));
  EXPECT(DownloadOriginAllowed(app, none, "data:text/plain,hi",
                               "app://localhost"));
  // data: with no frame to go by: allowed (the dialog still asks).
  EXPECT(DownloadOriginAllowed(app, none, "data:text/plain,hi", ""));
  // A window never navigated has no origin of its own.
  EXPECT(!DownloadOriginAllowed("", none, "app://localhost/a", ""));
  // A file: window may download its files.
  EXPECT(DownloadOriginAllowed("file:", none, "file:///tmp/a.txt", ""));
  // Loopback servers: the port is part of the origin.
  EXPECT(DownloadOriginAllowed("http://127.0.0.1:5173", none,
                               "http://127.0.0.1:5173/f", ""));
  EXPECT(!DownloadOriginAllowed("http://127.0.0.1:5173", none,
                                "http://127.0.0.1:5174/f", ""));
  EXPECT(!DownloadOriginAllowed("http://127.0.0.1:5173", none,
                                "http://localhost:5173/f", ""));

  // --- The prompt budget -----------------------------------------------------
  {
    DownloadPromptBudget budget;
    auto t0 = DownloadPromptBudget::Clock::now();
    for (size_t i = 0; i < kMaxDownloadPromptsPerWindow; ++i)
      EXPECT(budget.TryTake(t0));
    EXPECT(!budget.TryTake(t0 + std::chrono::seconds(1)));
    EXPECT(!budget.TryTake(t0 + kDownloadPromptWindow -
                           std::chrono::milliseconds(1)));
    EXPECT(budget.TryTake(t0 + kDownloadPromptWindow));
  }

  // --- The policy, through the navigation guard
  // -------------------------------
  {
    NavigationGuard::Instance().SetWindowUrl(7, "http://127.0.0.1:4000/");
    NavigationGuard::Instance().SetAllowedOrigins({});
    DownloadPolicy& policy = DownloadPolicy::Instance();
    auto t0 = DownloadPromptBudget::Clock::now();
    EXPECT(policy.Decide(7, "https://evil.example/x", "", t0) ==
           DownloadDecision::kForeignOrigin);
    // A refusal costs no prompt.
    for (size_t i = 0; i < kMaxDownloadPromptsPerWindow; ++i)
      EXPECT(policy.Decide(7, "http://127.0.0.1:4000/f", "", t0) ==
             DownloadDecision::kAsk);
    EXPECT(policy.Decide(7, "http://127.0.0.1:4000/f", "", t0) ==
           DownloadDecision::kThrottled);
    // Another window has its own budget.
    NavigationGuard::Instance().SetWindowUrl(8, "http://127.0.0.1:4000/");
    EXPECT(policy.Decide(8, "http://127.0.0.1:4000/f", "", t0) ==
           DownloadDecision::kAsk);
    // A closed window's budget is forgotten.
    policy.ForgetWindow(7);
    EXPECT(policy.Decide(7, "http://127.0.0.1:4000/f", "", t0) ==
           DownloadDecision::kAsk);
    EXPECT(policy.OriginAllowed(7, "blob:http://127.0.0.1:4000/x", ""));
    EXPECT(!policy.OriginAllowed(9, "http://127.0.0.1:4000/f", ""));
  }

  // --- The dialog request ----------------------------------------------------
  {
    std::string seen_default;
    int seen_kind = -1;
    uint32_t seen_flags = 99;
    std::string result = "unset";
    // Accepted.
    AskDownloadDestination(
        "../../x/report.csv",
        [&](const laufey_file_dialog_options_t* o,
            laufey_file_dialog_result_fn cb, void* ud) -> uint32_t {
          seen_default = o->default_path ? o->default_path : "";
          seen_kind = o->kind;
          seen_flags = o->flags;
          const char* paths[] = {"/home/u/Documents/report.csv"};
          cb(ud, 1, LAUFEY_FILE_DIALOG_ACCEPTED, paths, 1);
          return 1;
        },
        [&](std::string path) { result = path; });
    EXPECT(seen_default == "report.csv");
    EXPECT(seen_kind == LAUFEY_FILE_DIALOG_SAVE);
    EXPECT((seen_flags & LAUFEY_FILE_DIALOG_NO_OVERWRITE_CONFIRM) == 0);
    EXPECT(result == "/home/u/Documents/report.csv");
    // Cancelled, busy, failed: an empty path, so the download is cancelled.
    for (int status : {LAUFEY_FILE_DIALOG_CANCELLED, LAUFEY_FILE_DIALOG_BUSY,
                       LAUFEY_FILE_DIALOG_FAILED}) {
      result = "unset";
      AskDownloadDestination(
          "a.txt",
          [&](const laufey_file_dialog_options_t*,
              laufey_file_dialog_result_fn cb, void* ud) -> uint32_t {
            cb(ud, 0, status, nullptr, 0);
            return 0;
          },
          [&](std::string path) { result = path; });
      EXPECT(result.empty());
    }
    // No dialog at all.
    result = "unset";
    AskDownloadDestination("a.txt", nullptr,
                           [&](std::string path) { result = path; });
    EXPECT(result.empty());
  }

  std::printf("downloads_test: ok\n");
  return 0;
}
