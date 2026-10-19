// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// CEF: a document served over a custom scheme gets its body only once the
// browser process has taken in the document's commit.
//
// A page's localStorage is a StorageArea the renderer opens through the
// process-wide DomStorage interface, which is not ordered with the frame's
// commit messages. The browser checks the request against the frame's storage
// key (DOMStorageContextWrapper::IsRequestValid), and the frame takes the new
// document's key only when the browser handles its DidCommitNavigation
// (RenderFrameHostImpl::TakeNewDocumentPropertiesFromNavigation). A script
// that touches localStorage as the page loads can get its open there first:
// the key doesn't match, the browser drops the StorageArea without a word
// ("a race condition between committing the new document and binding the DOM
// Storage. For now, we'll just fail to bind."), and the page reads an empty
// localStorage and every write it makes is lost, for that document's life.
// The next launch reads the value from the launch before (the lost launch's
// writes never reached the storage service: it opened the database but never
// loaded the page's area). IndexedDB is bound per document and doesn't race.
// On macOS arm64 CI an app that stores at startup over a custom scheme, and
// quits at once, lost about one launch in 40 (17 in 720 quick launches,
// scripts/storage-stress-e2e-run.sh); over loopback HTTP, whose response
// comes later, none in 500.
//
// A document can't run a script before its body arrives, and its commit needs
// only the response head. So a frame's document over a laufey custom scheme
// (HTML, XHTML, SVG, XML: what runs scripts) holds its body until CEF reports
// the load start of that frame at that URL (OnLoadStart, from
// DidFinishNavigation, after the commit was taken in; the window's initial
// about:blank commits first and releases nothing), at most
// kCommitGateMaxWaitMs. With the hold, none of 200 such launches lost anything.
// DocumentCommitGates keeps the held responses; the CEF host releases them.
//
// Header-only.

#ifndef LAUFEY_CEF_COMMIT_GATE_H_
#define LAUFEY_CEF_COMMIT_GATE_H_

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace laufey_common {

// The longest a document's body is held for its commit. A navigation that
// never commits (a download) or a commit CEF doesn't report goes on after
// this.
inline constexpr int kCommitGateMaxWaitMs = 1000;

// Whether a response's body waits for its document's commit: the response to
// a frame's navigation (`frame_document`), with a body that becomes a
// document that can run scripts. A redirect or an empty status has no such
// body, and a MIME type the browser would sniff (none) needs body bytes
// before it commits, so it never waits.
inline bool DocumentBodyNeedsCommitGate(bool frame_document, int status,
                                        const std::string& mime_type) {
  if (!frame_document || status < 200 || status > 299 || status == 204 ||
      status == 205)
    return false;
  std::string mime = mime_type;
  std::transform(mime.begin(), mime.end(), mime.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return mime == "text/html" || mime == "application/xhtml+xml" ||
         mime == "image/svg+xml" || mime == "application/xml" ||
         mime == "text/xml";
}

// `url` without its fragment: the request's URL and the committed frame's URL
// are compared without it.
inline std::string CommitGateUrlKey(const std::string& url) {
  return url.substr(0, url.find('#'));
}

// The responses held for their documents' commits. Thread-safe; a release
// runs outside the lock, at most once per gate.
class DocumentCommitGates {
 public:
  using Release = std::function<void()>;

  // Holds a response for `browser_id`'s main frame (or a sub-frame) loading
  // `url`. Returns the gate's id (never 0).
  uint64_t Add(int browser_id, bool main_frame, const std::string& url,
               Release release) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = ++last_id_;
    gates_[id] =
        Gate{browser_id, main_frame, CommitGateUrlKey(url), std::move(release)};
    return id;
  }

  // A document of `browser_id` committed at `url`: releases the gates of
  // that browser's main frame (or of its sub-frames) for that URL. Another
  // document's commit (the window's initial about:blank) releases nothing; a
  // navigation that was replaced is cancelled (Forget) or runs into its
  // deadline. Returns how many it released.
  int Committed(int browser_id, bool main_frame, const std::string& url) {
    const std::string key = CommitGateUrlKey(url);
    return ReleaseWhere([&](const Gate& g) {
      return g.browser_id == browser_id && g.main_frame == main_frame &&
             g.url_key == key;
    });
  }

  // Releases one gate (its deadline passed). Returns whether it was held.
  bool ReleaseOne(uint64_t id) {
    return ReleaseWhere([](const Gate&) { return true; }, id) > 0;
  }

  // Drops a gate without running its release (the request was cancelled).
  void Forget(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    gates_.erase(id);
  }

  // Releases every gate of a browser that is going away.
  int ReleaseBrowser(int browser_id) {
    return ReleaseWhere(
        [&](const Gate& g) { return g.browser_id == browser_id; });
  }

  size_t HeldForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    return gates_.size();
  }

 private:
  struct Gate {
    int browser_id;
    bool main_frame;
    std::string url_key;
    Release release;
  };

  template <typename Pred>
  int ReleaseWhere(Pred pred, uint64_t only_id = 0) {
    std::vector<Release> released;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = gates_.begin(); it != gates_.end();) {
        if ((only_id == 0 || it->first == only_id) && pred(it->second)) {
          released.push_back(std::move(it->second.release));
          it = gates_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (Release& r : released) {
      if (r)
        r();
    }
    return static_cast<int>(released.size());
  }

  std::mutex mutex_;
  uint64_t last_id_ = 0;
  std::map<uint64_t, Gate> gates_;
};

// The process's gates (the CEF host's scheme handlers and load handler).
inline DocumentCommitGates& CommitGates() {
  static DocumentCommitGates* gates = new DocumentCommitGates();
  return *gates;
}

}  // namespace laufey_common

#endif  // LAUFEY_CEF_COMMIT_GATE_H_
