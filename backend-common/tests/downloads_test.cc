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

  // --- Downloads from the app's own scheme -----------------------------------
  EXPECT(IsAttachmentDisposition("attachment"));
  EXPECT(IsAttachmentDisposition(" Attachment ; filename=\"a.txt\""));
  EXPECT(!IsAttachmentDisposition("inline; filename=\"a.txt\""));
  EXPECT(!IsAttachmentDisposition(""));
  EXPECT(!IsAttachmentDisposition("attachments"));
  EXPECT(DispositionFileName("attachment; filename=\"a b.txt\"") == "a b.txt");
  EXPECT(DispositionFileName("attachment; filename=plain.csv") == "plain.csv");
  EXPECT(DispositionFileName("attachment; filename=\"q\\\"x.txt\"") ==
         "q\"x.txt");
  // filename* (RFC 8187) wins over filename, wherever it comes.
  EXPECT(DispositionFileName("attachment; filename*=UTF-8''r%C3%A9sum%C3%A9."
                             "pdf; filename=\"resume.pdf\"") ==
         "r\xC3\xA9sum\xC3\xA9.pdf");
  EXPECT(DispositionFileName("attachment; filename=\"resume.pdf\"; "
                             "filename*=utf-8'en'r%C3%A9.pdf") ==
         "r\xC3\xA9.pdf");
  // Another charset is not decoded; the plain name stands.
  EXPECT(DispositionFileName("attachment; filename*=ISO-8859-1''r%E9.pdf; "
                             "filename=r.pdf") == "r.pdf");
  EXPECT(DispositionFileName("attachment") == "");
  EXPECT(DispositionFileName("attachment; size=3") == "");
  // A traversing name is only proposed; the dialog's name is sanitized.
  EXPECT(SanitizeDownloadFileName(DispositionFileName(
             "attachment; filename=\"../../evil.txt\"")) == "evil.txt");
  EXPECT(UrlFileName("myapp://app/exports/report%20q3.csv?x=1#y") ==
         "report q3.csv");
  EXPECT(UrlFileName("myapp://app/") == "");
  EXPECT(UrlFileName("myapp://app") == "");
  EXPECT(UrlFileName("myapp://app/a%2") == "a%2");
  EXPECT(IsAppSchemeUrl("myapp://app/x"));
  EXPECT(IsAppSchemeUrl("App://e2e/x"));
  EXPECT(!IsAppSchemeUrl("http://127.0.0.1:3000/x"));
  EXPECT(!IsAppSchemeUrl("https://example.com/x"));
  EXPECT(!IsAppSchemeUrl("file:///tmp/x"));
  EXPECT(!IsAppSchemeUrl("blob:myapp://app/1234"));
  EXPECT(!IsAppSchemeUrl("data:text/plain,x"));
  EXPECT(!IsAppSchemeUrl("about:blank"));
  EXPECT(IsRepeatableDownloadMethod("GET"));
  EXPECT(IsRepeatableDownloadMethod(""));
  EXPECT(!IsRepeatableDownloadMethod("POST"));
  EXPECT(!IsRepeatableDownloadMethod("PUT"));
  {
    SchemeResponseNotes& notes = SchemeResponseNotes::Instance();
    SchemeResponseNotes::Note note;
    EXPECT(!notes.Find("myapp://app/none", &note));
    notes.NoteRequest("myapp://app/a.txt", "GET",
                      std::string("accept\0*/*\0", 11));
    notes.NoteResponse("myapp://app/a.txt",
                       {{"Content-Type", "text/plain"},
                        {"Content-Disposition", "attachment; filename=a.txt"}});
    EXPECT(notes.Find("myapp://app/a.txt", &note));
    EXPECT(note.method == "GET");
    EXPECT(note.flat_request_headers == std::string("accept\0*/*\0", 11));
    EXPECT(note.disposition == "attachment; filename=a.txt");
    // A later request for the URL replaces the note.
    notes.NoteRequest("myapp://app/a.txt", "POST", "");
    EXPECT(notes.Find("myapp://app/a.txt", &note));
    EXPECT(note.method == "POST" && note.disposition.empty());
    // A response with no request noted is dropped.
    notes.NoteResponse("myapp://app/unseen", {{"content-disposition", "x"}});
    EXPECT(!notes.Find("myapp://app/unseen", &note));
    // Bounded: the oldest notes go.
    for (size_t i = 0; i < SchemeResponseNotes::kMaxNotes; ++i)
      notes.NoteRequest("myapp://app/n" + std::to_string(i), "GET", "");
    EXPECT(!notes.Find("myapp://app/a.txt", &note));
    EXPECT(notes.Find("myapp://app/n0", &note));
  }
  {
    // The sink writes only a whole 2xx response, to the chosen path.
    std::string dir;
    if (const char* tmp = std::getenv("TMPDIR"))
      dir = tmp;
    if (dir.empty())
      dir = "/tmp";
    std::string path = dir + "/laufey-dl-sink-" + std::to_string(std::rand());
    auto read_file = [](const std::string& p, std::string* out) {
      std::FILE* f = std::fopen(p.c_str(), "rb");
      if (!f)
        return false;
      char buf[256];
      out->clear();
      size_t n;
      while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out->append(buf, n);
      std::fclose(f);
      return true;
    };
    std::string got;
    {
      SchemeDownloadSink sink("myapp://app/a", path);
      EXPECT(sink.Begin(200));
      const uint8_t a[] = {'h', 'e', 'l'};
      const uint8_t b[] = {'l', 'o'};
      EXPECT(sink.Write(a, 3));
      // Nothing at the chosen path until the response ends.
      EXPECT(!read_file(path, &got));
      EXPECT(sink.Write(b, 2));
      EXPECT(sink.Finish());
      EXPECT(!read_file(sink.temp_path(), &got));
    }
    EXPECT(read_file(path, &got) && got == "hello");
    // A replaced file: the dialog asked first.
    {
      SchemeDownloadSink sink("myapp://app/a", path);
      EXPECT(sink.Begin(201));
      const uint8_t c[] = {'x'};
      EXPECT(sink.Write(c, 1));
      EXPECT(sink.Finish());
    }
    EXPECT(read_file(path, &got) && got == "x");
    // Another status: nothing written, the file kept.
    {
      SchemeDownloadSink sink("myapp://app/a", path);
      EXPECT(!sink.Begin(404));
      const uint8_t d[] = {'n', 'o'};
      EXPECT(!sink.Write(d, 2));
      EXPECT(!sink.Finish());
      EXPECT(!read_file(sink.temp_path(), &got));
    }
    EXPECT(read_file(path, &got) && got == "x");
    // No head at all, or a sink dropped mid-response: no file.
    std::remove(path.c_str());
    {
      SchemeDownloadSink sink("myapp://app/a", path);
      EXPECT(!sink.Finish());
    }
    {
      SchemeDownloadSink sink("myapp://app/a", path);
      EXPECT(sink.Begin(200));
      const uint8_t e[] = {'p'};
      EXPECT(sink.Write(e, 1));
    }
    EXPECT(!read_file(path, &got));
    EXPECT(!read_file(path + ".laufey-download", &got));
  }

  {
    // The name a custom-scheme save proposes.
    EXPECT(SchemeDownloadName("myapp://app/x/r.csv",
                              "attachment; filename=\"q.csv\"",
                              "engine.csv") == "q.csv");
    EXPECT(SchemeDownloadName("myapp://app/x/r.csv", "attachment",
                              "engine.csv") == "engine.csv");
    EXPECT(SchemeDownloadName("myapp://app/x/r.csv", "", "") == "r.csv");
    // A save from the app's scheme: the policy, the dialog, then the fetch.
    const uint32_t wid = 9001;
    NavigationGuard::Instance().SetWindowUrl(wid, "myapp://app/");
    std::string seen_default;
    std::string fetched = "unset";
    auto show = [&](const laufey_file_dialog_options_t* o,
                    laufey_file_dialog_result_fn cb, void* ud) -> uint32_t {
      seen_default = o->default_path ? o->default_path : "";
      const char* paths[] = {"/home/u/saved.csv"};
      cb(ud, 1, LAUFEY_FILE_DIALOG_ACCEPTED, paths, 1);
      return 1;
    };
    auto fetch = [&](std::string path) { fetched = path; };
    EXPECT(StartSchemeDownload(wid, "myapp://app/exports/r%201.csv", "", "GET",
                               "", show, fetch));
    EXPECT(seen_default == "r 1.csv");
    EXPECT(fetched == "/home/u/saved.csv");
    // A suggested name wins over the URL's.
    fetched = "unset";
    EXPECT(StartSchemeDownload(wid, "myapp://app/r.csv", "myapp://app", "GET",
                               "named.csv", show, fetch));
    EXPECT(seen_default == "named.csv" && fetched == "/home/u/saved.csv");
    // A POST is never sent again; another origin, another frame's origin or
    // no window is refused; none of them asks or fetches.
    seen_default = "none";
    fetched = "unset";
    EXPECT(!StartSchemeDownload(wid, "myapp://app/r.csv", "", "POST", "", show,
                                fetch));
    EXPECT(!StartSchemeDownload(wid, "other://app/r.csv", "", "GET", "", show,
                                fetch));
    EXPECT(!StartSchemeDownload(wid, "myapp://app/r.csv", "https://ads.example",
                                "GET", "", show, fetch));
    EXPECT(!StartSchemeDownload(0, "myapp://app/r.csv", "", "GET", "", show,
                                fetch));
    EXPECT(seen_default == "none" && fetched == "unset");
    // The window's prompt budget applies (2 taken above).
    EXPECT(
        StartSchemeDownload(wid, "myapp://app/r.csv", "", "", "", show, fetch));
    fetched = "unset";
    EXPECT(!StartSchemeDownload(wid, "myapp://app/r.csv", "", "GET", "", show,
                                fetch));
    EXPECT(fetched == "unset");
    // A dismissed dialog fetches nothing.
    DownloadPolicy::Instance().ForgetWindow(wid);
    EXPECT(StartSchemeDownload(
        wid, "myapp://app/r.csv", "", "GET", "",
        [](const laufey_file_dialog_options_t*, laufey_file_dialog_result_fn cb,
           void* ud) -> uint32_t {
          cb(ud, 0, LAUFEY_FILE_DIALOG_CANCELLED, nullptr, 0);
          return 0;
        },
        fetch));
    EXPECT(fetched == "unset");
    NavigationGuard::Instance().ForgetWindow(wid);
    DownloadPolicy::Instance().ForgetWindow(wid);
  }

  std::printf("downloads_test: ok\n");
  return 0;
}
