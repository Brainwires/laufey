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

// Reserved bridge method: `window[ns].__laufeyDownload(url, name)` saves `url`
// (on the app's scheme, the calling document's origin) as StartSchemeDownload
// does, proposing `name`. RuntimeLoader handles it before the runtime sees
// it; the download policy and the dialog decide as for any download.
#define LAUFEY_SCHEME_DOWNLOAD_METHOD "__laufeyDownload"

// WebView2: a page's `<a download>` link to the app's own scheme never starts
// (Chromium fetches a download link with a URL loader of its own, which has
// no way to reach the scheme). The Navigation API reports the link first, as
// a cancelable navigation with a `downloadRequest` (the link's `download`
// name): this script cancels it and hands the URL to
// LAUFEY_SCHEME_DOWNLOAD_METHOD instead. Only a same-origin link on a scheme
// the engine doesn't load itself; everything else keeps the engine's path.
inline std::string BuildSchemeDownloadLinkScript(const std::string& ns) {
  return R"JS(
(function() {
  var ns = ")JS" +
         ns +
         R"JS(";
  var engineSchemes = ['http:', 'https:', 'blob:', 'data:', 'file:',
                       'about:', 'javascript:'];
  if (!window.navigation ||
      typeof window.navigation.addEventListener !== 'function') return;
  window.navigation.addEventListener('navigate', function(e) {
    try {
      if (typeof e.downloadRequest !== 'string' || !e.cancelable) return;
      var dest = new URL(e.destination.url);
      if (engineSchemes.indexOf(dest.protocol) !== -1) return;
      if (dest.origin !== location.origin) return;
      var bridge = window[ns];
      if (!bridge) return;
      e.preventDefault();
      bridge.)JS" LAUFEY_SCHEME_DOWNLOAD_METHOD
         R"JS((dest.href, e.downloadRequest);
    } catch (err) {
      // Never let this break in-page navigation.
    }
  });
})();
)JS";
}

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
