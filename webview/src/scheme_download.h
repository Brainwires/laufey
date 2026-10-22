// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Downloads from the app's own custom scheme on the WebKit backends
// (WKWebView, WebKitGTK; docs/downloads.md, laufey_downloads.h). WebKit makes
// a navigation into a download by handing its load to the network process,
// which never had a load the app's scheme handler served: an attachment from
// `myapp://app` (or an `<a download>` link to one) silently never starts.
// So those backends cancel such a navigation and call StartSchemeDownload:
// the download policy and the save dialog as for any download, then a GET of
// the URL through the app's scheme handler, written by a SchemeDownloadSink.

#ifndef LAUFEY_SCHEME_DOWNLOAD_H_
#define LAUFEY_SCHEME_DOWNLOAD_H_

#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>

#include "laufey_downloads.h"
#include "runtime_loader.h"
#include "scheme_exchange.h"

namespace laufey_webview {

// A scheme exchange whose response goes to a file instead of a web view.
class SchemeDownloadExchange : public SchemeExchangeBase {
 public:
  SchemeDownloadExchange(const std::string& url, const std::string& path)
      : sink_(url, path) {}

  intptr_t ReadRequestBody(uint8_t*, size_t) override {
    return 0;
  }

  void Begin(int status, const char*, size_t) override {
    sink_.Begin(status);
  }

  intptr_t WriteResponse(const uint8_t* buf, size_t len) override {
    return sink_.Write(buf, len) ? static_cast<intptr_t>(len) : -1;
  }

  void Finish() override {
    sink_.Finish();
    delete this;
  }

 private:
  laufey_common::SchemeDownloadSink sink_;
};

// Runs `fn` on the engine's UI thread.
using UiPost = std::function<void(std::function<void()>)>;

// Saves `url` (on the app's scheme) for window `window_id`, after the
// download policy (`initiator`: the origin of the frame that started it, ""
// when the engine doesn't say) and the save dialog (`show`, seeded with
// `suggested`). `method` is the original request's: only a GET (or HEAD) is
// fetched again; `flat_headers` are its headers, sent with the GET. Call on
// the UI thread.
inline void StartSchemeDownload(uint32_t window_id, const std::string& url,
                                const std::string& initiator,
                                const std::string& method,
                                const std::string& flat_headers,
                                const std::string& suggested,
                                laufey_common::DownloadDialogShow show,
                                UiPost post) {
  if (!laufey_common::IsRepeatableDownloadMethod(method)) {
    std::string shown = url.size() > 200 ? url.substr(0, 200) + "..." : url;
    std::cerr << "laufey: not downloading " << shown << ": a " << method
              << " response from the app's scheme can't be saved (serve the "
                 "file from a GET)"
              << std::endl;
    return;
  }
  laufey_common::DownloadDecision decision =
      window_id == 0 ? laufey_common::DownloadDecision::kForeignOrigin
                     : laufey_common::DownloadPolicy::Instance().Decide(
                           window_id, url, initiator);
  if (decision != laufey_common::DownloadDecision::kAsk) {
    laufey_common::LogRefusedDownload(url, decision);
    return;
  }
  laufey_common::AskDownloadDestination(
      suggested.empty() ? laufey_common::UrlFileName(url) : suggested, show,
      [window_id, url, flat_headers, post](std::string path) {
        if (path.empty())
          return;
        post([window_id, url, flat_headers, path] {
          RuntimeLoader::GetInstance()->DispatchSchemeRequest(
              window_id, new SchemeDownloadExchange(url, path), "GET", url,
              flat_headers);
        });
      });
}

// The name to propose for a download of `url` whose response said
// `disposition` ("" when unknown), else `engine_suggestion`, else the URL's
// last path segment.
inline std::string SchemeDownloadName(const std::string& url,
                                      const std::string& disposition,
                                      const std::string& engine_suggestion) {
  std::string name = laufey_common::DispositionFileName(disposition);
  if (name.empty())
    name = engine_suggestion;
  if (name.empty())
    name = laufey_common::UrlFileName(url);
  return name;
}

}  // namespace laufey_webview

#endif  // LAUFEY_SCHEME_DOWNLOAD_H_
