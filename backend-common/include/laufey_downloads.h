// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Page-triggered downloads (docs/downloads.md): what every web-engine backend
// checks before it asks where to save one, and how it asks.
//
// A download (an `<a download>` click, a `Content-Disposition: attachment`
// response, a blob or data: export) is saved only where the user chose in
// the save dialog, and only:
//
//   - from an origin the window's top-level frame may load (the navigation
//     guard's rules, laufey_navigation_guard.h: the origin the embedder
//     navigated the window to, the embedder's allowed navigation origins, or
//     file: for a window showing a file). A blob: URL counts as the origin
//     that made it. A frame of another origin (a third-party iframe the
//     page's CSP lets in) can't start one, and neither can a link to another
//     origin's file;
//   - when the engine says which frame started it (WebKit on macOS), from a
//     frame of such an origin too. data: URLs have no origin of their own: they
//     need that frame, and are allowed where the engine doesn't say;
//   - while the window has asked fewer than kMaxDownloadPromptsPerWindow
//     times in the last kDownloadPromptWindow (a page looping downloads must
//     not keep the dialog up), and no other file dialog is open (one dialog
//     at a time: the file-dialog slot in laufey_io.h).
//
// The dialog is seeded with SanitizeDownloadFileName(the engine's suggested
// name): a bare file name with no directory part, so the page (its `download`
// attribute) or the server (its Content-Disposition filename) only proposes a
// name; the dialog asks before replacing a file. Dismissing it, or a refusal
// above, cancels the download: nothing is written.
//
// A download from the app's own custom scheme (`myapp://app/report.csv`, an
// attachment or an `<a download>` link) never reaches the WebKit engines'
// download path: WebKit turns a navigation into a download by handing its
// load to the network process, which never had a load the scheme handler
// served, so the download silently never starts. The WebKit backends
// (WKWebView, WebKitGTK) therefore cancel such a navigation and save the URL
// themselves: the same policy and dialog as above, then a GET of the URL
// through the app's scheme handler, written by a SchemeDownloadSink.
// SchemeResponseNotes keeps what each scheme response said (its
// Content-Disposition, the request's method and headers), since the engines
// hand the policy decision a response that may have lost its headers.

#ifndef LAUFEY_DOWNLOADS_H_
#define LAUFEY_DOWNLOADS_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "laufey.h"
#include "laufey_navigation_guard.h"
#include "laufey_scheme_registry.h"

namespace laufey_common {

// At most this many save dialogs per window in any kDownloadPromptWindow.
inline constexpr size_t kMaxDownloadPromptsPerWindow = 3;
inline constexpr std::chrono::seconds kDownloadPromptWindow{10};

// The longest file name proposed (bytes of UTF-8; most file systems allow
// 255).
inline constexpr size_t kMaxDownloadFileNameBytes = 200;

namespace downloads_internal {

// Length of the valid UTF-8 sequence at s[i] (0 if invalid). Overlongs,
// surrogates and code points above U+10FFFF are invalid.
inline size_t Utf8SequenceLength(const std::string& s, size_t i) {
  unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80)
    return 1;
  size_t n;
  uint32_t cp;
  if ((c & 0xE0) == 0xC0) {
    n = 2;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    n = 3;
    cp = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    n = 4;
    cp = c & 0x07;
  } else {
    return 0;
  }
  if (i + n > s.size())
    return 0;
  for (size_t k = 1; k < n; ++k) {
    unsigned char cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80)
      return 0;
    cp = (cp << 6) | (cc & 0x3F);
  }
  if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
      (n == 4 && cp < 0x10000) || cp > 0x10FFFF ||
      (cp >= 0xD800 && cp <= 0xDFFF))
    return 0;
  // Unicode's bidirectional overrides and isolates (U+202A..U+202E,
  // U+2066..U+2069) make a name read as another ("evil\u202Etxt.exe").
  if ((cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069))
    return 0;
  return n;
}

inline bool IsWindowsReservedStem(const std::string& stem) {
  std::string upper;
  for (char c : stem)
    upper.push_back((c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c);
  // Trailing spaces / dots are ignored by Windows ("CON .txt" is CON).
  while (!upper.empty() && (upper.back() == ' ' || upper.back() == '.'))
    upper.pop_back();
  static const char* const kNames[] = {"CON", "PRN",    "AUX",
                                       "NUL", "CONIN$", "CONOUT$"};
  for (const char* name : kNames) {
    if (upper == name)
      return true;
  }
  if (upper.size() == 4 &&
      (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) &&
      upper[3] >= '0' && upper[3] <= '9')
    return true;
  return false;
}

}  // namespace downloads_internal

// The file name a save dialog proposes for a download whose engine suggests
// `suggested`: its last path component (after any '/' or '\\'), without
// control characters, bidirectional overrides, invalid UTF-8 or the
// characters Windows forbids in a name (each replaced by '_'), without
// leading dots or spaces or trailing dots or spaces, not a Windows device
// name (prefixed with '_'), and at most kMaxDownloadFileNameBytes (the
// extension kept when it is short). "download" when nothing is left.
inline std::string SanitizeDownloadFileName(const std::string& suggested) {
  using namespace downloads_internal;
  size_t slash = suggested.find_last_of("/\\");
  std::string base =
      slash == std::string::npos ? suggested : suggested.substr(slash + 1);

  std::string clean;
  for (size_t i = 0; i < base.size();) {
    unsigned char c = static_cast<unsigned char>(base[i]);
    size_t n = Utf8SequenceLength(base, i);
    if (n == 0) {
      clean.push_back('_');
      ++i;
      continue;
    }
    if (n == 1) {
      if (c < 0x20 || c == 0x7F || c == '<' || c == '>' || c == ':' ||
          c == '"' || c == '|' || c == '?' || c == '*')
        clean.push_back('_');
      else
        clean.push_back(static_cast<char>(c));
    } else {
      clean.append(base, i, n);
    }
    i += n;
  }

  size_t start = clean.find_first_not_of(". ");
  if (start == std::string::npos)
    return "download";
  size_t end = clean.find_last_not_of(". ");
  clean = clean.substr(start, end - start + 1);

  // Keep it short, cutting the stem (on a UTF-8 boundary) so a short
  // extension survives.
  if (clean.size() > kMaxDownloadFileNameBytes) {
    size_t dot = clean.rfind('.');
    std::string ext;
    if (dot != std::string::npos && clean.size() - dot <= 16)
      ext = clean.substr(dot);
    size_t keep = kMaxDownloadFileNameBytes - ext.size();
    while (keep > 0 && (static_cast<unsigned char>(clean[keep]) & 0xC0) == 0x80)
      --keep;
    clean = clean.substr(0, keep) + ext;
  }

  size_t dot = clean.find('.');
  if (IsWindowsReservedStem(clean.substr(0, dot)))
    clean = "_" + clean;
  return clean.empty() ? "download" : clean;
}

// What a backend does with a page-triggered download.
enum class DownloadDecision {
  // Ask where to save it.
  kAsk,
  // Cancel it: its URL's origin, or the frame that started it, is not one
  // the window's top-level frame may load.
  kForeignOrigin,
  // Cancel it: the window asked kMaxDownloadPromptsPerWindow times within
  // the last kDownloadPromptWindow.
  kThrottled,
};

// Whether a window whose embedder-navigated key is `window_key`, with the
// embedder's `allowed` origins, may download `url`, started by a frame of
// origin `initiator` ("" when the engine doesn't say). Pure; see the file
// comment.
inline bool DownloadOriginAllowed(const std::string& window_key,
                                  const std::vector<std::string>& allowed,
                                  const std::string& url,
                                  const std::string& initiator) {
  auto ok = [&](const std::string& key) {
    if (key.empty())
      return false;
    if (key == window_key)
      return true;
    for (const std::string& origin : allowed) {
      if (origin == key)
        return true;
    }
    return false;
  };
  if (!initiator.empty() && !ok(NavigationOriginKey(initiator)))
    return false;
  std::string key = NavigationOriginKey(url);
  if (!key.empty())
    return ok(key);
  // No origin of its own (data:, about:, an opaque blob:): the frame that
  // made it decides, where the engine says which one it was.
  return true;
}

// The per-window prompt budget: a sliding kDownloadPromptWindow.
class DownloadPromptBudget {
 public:
  using Clock = std::chrono::steady_clock;

  bool TryTake(Clock::time_point now) {
    while (!prompts_.empty() && now - prompts_.front() >= kDownloadPromptWindow)
      prompts_.pop_front();
    if (prompts_.size() >= kMaxDownloadPromptsPerWindow)
      return false;
    prompts_.push_back(now);
    return true;
  }

 private:
  std::deque<Clock::time_point> prompts_;
};

// The process's download policy: the navigation guard's origins plus each
// window's prompt budget. Thread-safe.
class DownloadPolicy {
 public:
  static DownloadPolicy& Instance() {
    static DownloadPolicy* policy = new DownloadPolicy();
    return *policy;
  }

  // The origin check alone (no budget taken): for an engine's early "may
  // this download start" hook.
  bool OriginAllowed(uint32_t window_id, const std::string& url,
                     const std::string& initiator) {
    std::string key;
    std::vector<std::string> allowed;
    NavigationGuard::Instance().Snapshot(window_id, &key, &allowed);
    return DownloadOriginAllowed(key, allowed, url, initiator);
  }

  // The decision for a download of `url` in `window_id` (see the file
  // comment); kAsk takes one prompt from the window's budget.
  DownloadDecision Decide(uint32_t window_id, const std::string& url,
                          const std::string& initiator,
                          DownloadPromptBudget::Clock::time_point now =
                              DownloadPromptBudget::Clock::now()) {
    if (!OriginAllowed(window_id, url, initiator))
      return DownloadDecision::kForeignOrigin;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!budgets_[window_id].TryTake(now))
      return DownloadDecision::kThrottled;
    return DownloadDecision::kAsk;
  }

  void ForgetWindow(uint32_t window_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    budgets_.erase(window_id);
  }

 private:
  DownloadPolicy() = default;
  std::mutex mutex_;
  std::map<uint32_t, DownloadPromptBudget> budgets_;
};

// The message a refused download is logged with. The URL is cut short: a
// data: URL carries the whole file.
inline void LogRefusedDownload(const std::string& url,
                               DownloadDecision decision) {
  std::string shown = url.size() > 200 ? url.substr(0, 200) + "..." : url;
  if (decision == DownloadDecision::kForeignOrigin) {
    std::cerr << "laufey: not downloading " << shown
              << ": it is not from the app's origin" << std::endl;
  } else if (decision == DownloadDecision::kThrottled) {
    std::cerr << "laufey: not downloading " << shown
              << ": the window asked for more than "
              << kMaxDownloadPromptsPerWindow << " downloads in "
              << kDownloadPromptWindow.count() << " s" << std::endl;
  }
}

// Shows the backend's save dialog (its show_file_dialog: `show(options,
// callback, user_data)`, which answers through `callback` exactly once,
// possibly before it returns) seeded with SanitizeDownloadFileName(
// `suggested`), and calls `done` exactly once with the absolute path the
// user chose, or "" when the dialog was dismissed, another file dialog was
// open, or it could not be shown. `done` runs on whichever thread the
// dialog answers on (the backend hops to the engine's thread itself).
using DownloadDialogShow = std::function<uint32_t(
    const laufey_file_dialog_options_t*, laufey_file_dialog_result_fn, void*)>;

inline void AskDownloadDestination(const std::string& suggested,
                                   const DownloadDialogShow& show,
                                   std::function<void(std::string)> done) {
  struct Pending {
    std::function<void(std::string)> done;
  };
  if (!show) {
    done(std::string());
    return;
  }
  std::string name = SanitizeDownloadFileName(suggested);
  // The dialog opens in the platform's usual directory. Test hook
  // (LAUFEY_TEST_DOWNLOAD_DIR=<dir>): open it in <dir> instead, as an app's
  // own save dialog given a directory does, so the e2e battery's
  // test_file_dialog_respond types a path into the directory already shown
  // (a macOS save panel moved to another directory by the hook isn't
  // accepted in time).
  std::string default_path = name;
  if (const char* dir = std::getenv("LAUFEY_TEST_DOWNLOAD_DIR")) {
    if (*dir) {
#ifdef _WIN32
      default_path = std::string(dir) + "\\" + name;
#else
      default_path = std::string(dir) + "/" + name;
#endif
    }
  }
  laufey_file_dialog_options_t options = {};
  options.kind = LAUFEY_FILE_DIALOG_SAVE;
  options.flags = 0;  // asks before replacing a file
  options.default_path = default_path.c_str();
  auto* pending = new Pending{std::move(done)};
  show(
      &options,
      [](void* user_data, uint32_t /*dialog_id*/, int status,
         const char* const* paths, size_t count) {
        auto* p = static_cast<Pending*>(user_data);
        std::string path;
        if (status == LAUFEY_FILE_DIALOG_ACCEPTED && paths && count > 0 &&
            paths[0])
          path = paths[0];
        if (status == LAUFEY_FILE_DIALOG_BUSY)
          std::cerr << "laufey: not downloading: another file dialog is open"
                    << std::endl;
        p->done(std::move(path));
        delete p;
      },
      pending);
}

// --- Downloads from the app's own scheme -----------------------------------

namespace downloads_internal {

inline std::string TrimAscii(const std::string& s) {
  size_t a = s.find_first_not_of(" \t");
  if (a == std::string::npos)
    return "";
  size_t b = s.find_last_not_of(" \t");
  return s.substr(a, b - a + 1);
}

inline std::string LowerAsciiCopy(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return s;
}

inline int HexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// %XX-decodes `s` (an invalid escape is kept as is).
inline std::string PercentDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && HexValue(s[i + 1]) >= 0 &&
        HexValue(s[i + 2]) >= 0) {
      out.push_back(
          static_cast<char>(HexValue(s[i + 1]) * 16 + HexValue(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

// The `;`-separated parameters of a Content-Disposition value after its
// type, as (lowercased name, raw value with quotes removed) pairs.
inline std::vector<std::pair<std::string, std::string>> DispositionParams(
    const std::string& value) {
  std::vector<std::pair<std::string, std::string>> out;
  size_t i = value.find(';');
  while (i != std::string::npos && i < value.size()) {
    ++i;
    size_t eq = value.find('=', i);
    size_t semi = value.find(';', i);
    if (eq == std::string::npos || (semi != std::string::npos && semi < eq)) {
      i = semi;
      continue;
    }
    std::string name = LowerAsciiCopy(TrimAscii(value.substr(i, eq - i)));
    size_t v = eq + 1;
    while (v < value.size() && (value[v] == ' ' || value[v] == '\t'))
      ++v;
    std::string raw;
    if (v < value.size() && value[v] == '"') {
      // A quoted string: `\x` escapes x.
      ++v;
      while (v < value.size() && value[v] != '"') {
        if (value[v] == '\\' && v + 1 < value.size())
          ++v;
        raw.push_back(value[v]);
        ++v;
      }
      i = value.find(';', v);
    } else {
      size_t end = value.find(';', v);
      raw = TrimAscii(value.substr(
          v, end == std::string::npos ? std::string::npos : end - v));
      i = end;
    }
    out.emplace_back(std::move(name), std::move(raw));
  }
  return out;
}

}  // namespace downloads_internal

// Whether a Content-Disposition header value asks for a download (its type
// is `attachment`, any case).
inline bool IsAttachmentDisposition(const std::string& value) {
  using namespace downloads_internal;
  std::string type = TrimAscii(value.substr(0, value.find(';')));
  return LowerAsciiCopy(type) == "attachment";
}

// The file name a Content-Disposition value suggests (RFC 6266): its
// `filename*` (RFC 8187, UTF-8 only) over its `filename`; "" when it names
// none. Not sanitized: SanitizeDownloadFileName runs on whatever is proposed.
inline std::string DispositionFileName(const std::string& value) {
  using namespace downloads_internal;
  std::string plain;
  for (const auto& [name, raw] : DispositionParams(value)) {
    if (name == "filename*") {
      // charset'language'pct-encoded
      size_t q1 = raw.find('\'');
      size_t q2 = q1 == std::string::npos ? q1 : raw.find('\'', q1 + 1);
      if (q2 != std::string::npos &&
          LowerAsciiCopy(raw.substr(0, q1)) == "utf-8") {
        std::string decoded = PercentDecode(raw.substr(q2 + 1));
        if (!decoded.empty())
          return decoded;
      }
    } else if (name == "filename" && plain.empty()) {
      plain = raw;
    }
  }
  return plain;
}

// The last path segment of `url` (no query or fragment), percent-decoded:
// the name proposed for a download whose response names none.
inline std::string UrlFileName(const std::string& url) {
  std::string path = url.substr(0, url.find_first_of("?#"));
  size_t authority = path.find("://");
  if (authority != std::string::npos) {
    size_t slash = path.find('/', authority + 3);
    path = slash == std::string::npos ? std::string() : path.substr(slash);
  }
  size_t last = path.find_last_of('/');
  std::string segment =
      last == std::string::npos ? path : path.substr(last + 1);
  return downloads_internal::PercentDecode(segment);
}

// Whether `url` is on a scheme an embedder may register (a custom scheme the
// app's scheme handler serves, `myapp://app/x`), not one the engines load
// themselves (http, file, blob, data, ...).
inline bool IsAppSchemeUrl(const std::string& url) {
  std::string scheme = OriginScheme(url);
  return !scheme.empty() && IsValidSchemeName(scheme);
}

// The methods a download from the app's scheme may repeat: the save is a
// second request for the URL, which must not redo what the first one did.
inline bool IsRepeatableDownloadMethod(const std::string& method) {
  return method.empty() || method == "GET" || method == "HEAD";
}

// What a custom-scheme request and its response said, by URL: the request's
// method and headers (flat name\0value\0 pairs) and the response's
// Content-Disposition. Thread-safe; keeps the kMaxNotes most recent URLs.
class SchemeResponseNotes {
 public:
  struct Note {
    std::string method;
    std::string flat_request_headers;
    std::string disposition;
  };

  static constexpr size_t kMaxNotes = 64;

  static SchemeResponseNotes& Instance() {
    static SchemeResponseNotes* notes = new SchemeResponseNotes();
    return *notes;
  }

  // A request for `url` started (its method and headers).
  void NoteRequest(const std::string& url, const std::string& method,
                   const std::string& flat_headers) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = notes_.begin(); it != notes_.end(); ++it) {
      if (it->first == url) {
        notes_.erase(it);
        break;
      }
    }
    notes_.push_back({url, Note{method, flat_headers, ""}});
    while (notes_.size() > kMaxNotes)
      notes_.pop_front();
  }

  // The response for `url` began, with these headers.
  void NoteResponse(
      const std::string& url,
      const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string disposition;
    for (const auto& [name, value] : headers) {
      if (downloads_internal::LowerAsciiCopy(name) == "content-disposition")
        disposition = value;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = notes_.rbegin(); it != notes_.rend(); ++it) {
      if (it->first == url) {
        it->second.disposition = disposition;
        return;
      }
    }
  }

  // The note for `url`, if one is kept.
  bool Find(const std::string& url, Note* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = notes_.rbegin(); it != notes_.rend(); ++it) {
      if (it->first == url) {
        *out = it->second;
        return true;
      }
    }
    return false;
  }

 private:
  SchemeResponseNotes() = default;
  std::mutex mutex_;
  std::deque<std::pair<std::string, Note>> notes_;
};

// Writes one download from the app's scheme to `path`, the path the save
// dialog chose: the bytes go to a sibling temporary file, which replaces
// `path` only once the whole response arrived with a 2xx status. Anything
// else (another status, a write error, a response that ends without a head)
// removes the temporary file and leaves `path` as it was. Not thread-safe:
// one writer (the scheme exchange's runtime thread).
class SchemeDownloadSink {
 public:
  explicit SchemeDownloadSink(std::string url, std::string path)
      : url_(std::move(url)),
        path_(std::move(path)),
        temp_(path_ + ".laufey-download") {}

  ~SchemeDownloadSink() {
    if (file_) {
      std::fclose(file_);
      std::remove(temp_.c_str());
    }
  }

  // The response's status. False when nothing will be written.
  bool Begin(int status) {
    if (began_)
      return file_ != nullptr;
    began_ = true;
    if (status < 200 || status > 299) {
      Fail("the app answered " + std::to_string(status));
      return false;
    }
    file_ = std::fopen(temp_.c_str(), "wb");
    if (!file_) {
      Fail("can't write " + temp_);
      return false;
    }
    return true;
  }

  // Appends response bytes. False once the download failed.
  bool Write(const uint8_t* buf, size_t len) {
    if (!file_)
      return false;
    if (len > 0 && std::fwrite(buf, 1, len, file_) != len) {
      std::fclose(file_);
      file_ = nullptr;
      std::remove(temp_.c_str());
      Fail("can't write " + temp_);
      return false;
    }
    return true;
  }

  // The response ended: true when `path` now holds it.
  bool Finish() {
    if (!began_)
      Fail("the app sent no response");
    if (!file_)
      return false;
    bool ok = std::fclose(file_) == 0;
    file_ = nullptr;
    if (ok) {
#ifdef _WIN32
      // rename() doesn't replace on Windows; the dialog asked already.
      std::remove(path_.c_str());
#endif
      ok = std::rename(temp_.c_str(), path_.c_str()) == 0;
    }
    if (!ok) {
      std::remove(temp_.c_str());
      Fail("can't write " + path_);
    }
    return ok;
  }

  const std::string& temp_path() const {
    return temp_;
  }

 private:
  void Fail(const std::string& why) {
    std::string shown = url_.size() > 200 ? url_.substr(0, 200) + "..." : url_;
    std::cerr << "laufey: not downloading " << shown << ": " << why
              << std::endl;
  }

  std::string url_;
  std::string path_;
  std::string temp_;
  std::FILE* file_ = nullptr;
  bool began_ = false;
};

// The name to propose for a download of `url` whose response said
// `disposition` ("" when unknown), else `engine_suggestion`, else the URL's
// last path segment.
inline std::string SchemeDownloadName(const std::string& url,
                                      const std::string& disposition,
                                      const std::string& engine_suggestion) {
  std::string name = DispositionFileName(disposition);
  if (name.empty())
    name = engine_suggestion;
  if (name.empty())
    name = UrlFileName(url);
  return name;
}

// Saves `url` (on the app's scheme) for window `window_id`: when `method` is
// repeatable (the save is a second request) and the download policy allows
// it (`initiator`: the origin of the frame that started it, "" when the
// engine doesn't say), asks with `show` (seeded with `suggested`, else the
// URL's name) and calls `fetch` with the chosen path, on whichever thread
// the dialog answered on. `fetch` asks the app's scheme handler for the URL
// with a GET and writes the response through a SchemeDownloadSink. A refusal
// is logged; a dismissed dialog fetches nothing. Returns whether it asked.
inline bool StartSchemeDownload(uint32_t window_id, const std::string& url,
                                const std::string& initiator,
                                const std::string& method,
                                const std::string& suggested,
                                const DownloadDialogShow& show,
                                std::function<void(std::string)> fetch) {
  if (!IsRepeatableDownloadMethod(method)) {
    std::string shown = url.size() > 200 ? url.substr(0, 200) + "..." : url;
    std::cerr << "laufey: not downloading " << shown << ": a " << method
              << " response from the app's scheme can't be saved (serve the "
                 "file from a GET)"
              << std::endl;
    return false;
  }
  DownloadDecision decision =
      window_id == 0
          ? DownloadDecision::kForeignOrigin
          : DownloadPolicy::Instance().Decide(window_id, url, initiator);
  if (decision != DownloadDecision::kAsk) {
    LogRefusedDownload(url, decision);
    return false;
  }
  AskDownloadDestination(suggested.empty() ? UrlFileName(url) : suggested, show,
                         [fetch](std::string path) {
                           if (!path.empty())
                             fetch(std::move(path));
                         });
  return true;
}

}  // namespace laufey_common

#endif  // LAUFEY_DOWNLOADS_H_
