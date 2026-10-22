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

// laufey_common::StartSchemeDownload, fetching through this backend's
// RuntimeLoader on the UI thread (`post`) with the original request's
// `flat_headers`. Call on the UI thread.
inline void StartSchemeDownload(uint32_t window_id, const std::string& url,
                                const std::string& initiator,
                                const std::string& method,
                                const std::string& flat_headers,
                                const std::string& suggested,
                                laufey_common::DownloadDialogShow show,
                                UiPost post) {
  laufey_common::StartSchemeDownload(
      window_id, url, initiator, method, suggested, show,
      [window_id, url, flat_headers, post](std::string path) {
        post([window_id, url, flat_headers, path] {
          RuntimeLoader::GetInstance()->DispatchSchemeRequest(
              window_id, new SchemeDownloadExchange(url, path), "GET", url,
              flat_headers);
        });
      });
}

}  // namespace laufey_webview

#endif  // LAUFEY_SCHEME_DOWNLOAD_H_
