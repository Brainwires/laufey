// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Wayland clipboard through ext-data-control-v1; see the header. One
// private connection, opened on the first use, served by a thread of its own:
// requests from any thread run there as tasks, the selection's data is read
// from its pipe without blocking that thread (the owner may be this very
// process, whose source answers on the same thread), and our own source
// writes each transfer from a short-lived writer thread (at most 8 at once,
// each with a deadline).

#include "clipboard_data_control_linux.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>

#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "wayland/ext-data-control-v1-client-protocol.h"

namespace laufey_common {
namespace data_control {

namespace {

using Clock = std::chrono::steady_clock;

// How long the selection's owner gets to hand over its data.
constexpr auto kReadTimeout = std::chrono::seconds(3);
// How long a caller waits for the connection's thread at all.
constexpr auto kCallTimeout = std::chrono::seconds(5);
// How long a reader of our own selection may take nothing before its
// transfer is dropped, and how long one transfer may take in all.
constexpr auto kWriteIdleTimeout = kReadTimeout;
constexpr auto kWriteTimeout = std::chrono::seconds(30);
// Transfers of our selection in flight at once; a request beyond these is
// refused (its descriptor closed at once: the reader sees an empty answer).
constexpr int kMaxWriters = 8;

std::atomic<int> g_writers{0};

// Whether the compositor advertised gtk_shell1, which only GNOME's mutter
// does (recorded while connecting, whether or not data-control is there).
std::atomic<bool> g_mutter{false};

struct Offer {
  ext_data_control_offer_v1* proxy = nullptr;
  std::vector<std::string> types;
};

struct Source {
  ext_data_control_source_v1* proxy = nullptr;
  Entries entries;
};

struct PendingRead {
  int fd = -1;
  size_t max_bytes = 0;
  std::string data;
  std::string mime;
  Clock::time_point deadline;
  std::shared_ptr<std::promise<std::pair<bool, std::string>>> done;
};

// Writes `data` to `fd` and closes it, giving up on a reader that takes
// nothing for kWriteIdleTimeout or hasn't taken it all after kWriteTimeout
// (the descriptor is non-blocking, so a peer that never reads can't pin the
// thread). SIGPIPE (a reader that went away) is blocked on this thread and
// consumed, so it can't end the process. Ends one of the g_writers.
void WriteAndClose(int fd, std::shared_ptr<const std::string> data) {
  sigset_t pipe_set, old_set;
  sigemptyset(&pipe_set);
  sigaddset(&pipe_set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);
  int flags = fcntl(fd, F_GETFL);
  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  auto now = Clock::now();
  const auto deadline = now + kWriteTimeout;
  auto idle_deadline = now + kWriteIdleTimeout;
  size_t off = 0;
  while (data && off < data->size()) {
    ssize_t n = write(fd, data->data() + off, data->size() - off);
    if (n > 0) {
      off += static_cast<size_t>(n);
      idle_deadline = Clock::now() + kWriteIdleTimeout;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      break;
    // The pipe is full: wait for the reader to take some.
    auto until = std::min(deadline, idle_deadline);
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    until - Clock::now())
                    .count();
    if (left <= 0)
      break;
    pollfd p = {fd, POLLOUT, 0};
    int r = poll(&p, 1, static_cast<int>(left));
    if (r < 0 && errno != EINTR)
      break;
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)))
      break;
  }
  close(fd);
  struct timespec zero = {0, 0};
  while (sigtimedwait(&pipe_set, nullptr, &zero) > 0) {
  }
  pthread_sigmask(SIG_SETMASK, &old_set, nullptr);
  g_writers--;
}

class Client {
 public:
  static Client* Get() {
    static Client* client = [] {
      auto* c = new Client();
      if (!c->Connect()) {
        delete c;
        return static_cast<Client*>(nullptr);
      }
      c->Start();
      return c;
    }();
    return client;
  }

  bool ok() const { return ok_.load(); }

  // Runs `fn` on the connection's thread; false when it didn't run in time
  // (or the connection is gone).
  bool Run(std::function<void()> fn) {
    if (!ok())
      return false;
    auto ran = std::make_shared<std::promise<void>>();
    std::future<void> done = ran->get_future();
    Post([fn = std::move(fn), ran] {
      fn();
      ran->set_value();
    });
    return done.wait_for(kCallTimeout) == std::future_status::ready;
  }

  // (Each task's state is shared with it: a task that misses the caller's
  // deadline still runs later.)
  std::vector<std::string> SelectionTypes() {
    auto types = std::make_shared<std::vector<std::string>>();
    if (!Run([this, types] {
          auto it = offers_.find(selection_);
          if (selection_ && it != offers_.end())
            *types = it->second.types;
        }))
      return {};
    return *types;
  }

  bool Read(const std::vector<std::string>& mimes, size_t max_bytes,
            std::string* out, std::string* mime_out) {
    auto done =
        std::make_shared<std::promise<std::pair<bool, std::string>>>();
    std::future<std::pair<bool, std::string>> result = done->get_future();
    auto chosen = std::make_shared<std::string>();
    bool started = Run([this, mimes, max_bytes, done, chosen] {
      auto it = offers_.find(selection_);
      if (!selection_ || it == offers_.end()) {
        done->set_value({false, ""});
        return;
      }
      for (const auto& m : mimes) {
        if (std::find(it->second.types.begin(), it->second.types.end(), m) !=
            it->second.types.end()) {
          *chosen = m;
          break;
        }
      }
      int fds[2];
      if (chosen->empty() || pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        done->set_value({false, ""});
        return;
      }
      // libwayland duplicates the descriptor when it marshals the request,
      // so our copy of the write end can go at once: EOF then means the
      // owner is done writing.
      ext_data_control_offer_v1_receive(selection_, chosen->c_str(), fds[1]);
      close(fds[1]);
      wl_display_flush(display_);
      PendingRead read;
      read.fd = fds[0];
      read.max_bytes = max_bytes;
      read.mime = *chosen;
      read.deadline = Clock::now() + kReadTimeout;
      read.done = done;
      reads_.push_back(std::move(read));
    });
    if (!started)
      return false;
    if (result.wait_for(kReadTimeout + std::chrono::seconds(1)) !=
        std::future_status::ready)
      return false;
    auto [found, data] = result.get();
    if (!found)
      return false;
    *out = std::move(data);
    if (mime_out)
      *mime_out = *chosen;
    return true;
  }

  bool Write(Entries entries) {
    // Done once the compositor has handled set_selection: a sync issued
    // after it is answered after the selection events it caused, so a read
    // right after this one sees our own offer.
    auto synced = std::make_shared<std::promise<void>>();
    std::future<void> done = synced->get_future();
    auto shared = std::make_shared<Entries>(std::move(entries));
    bool started = Run([this, shared, synced] {
      auto* source = new Source();
      source->proxy =
          ext_data_control_manager_v1_create_data_source(manager_);
      source->entries = std::move(*shared);
      ext_data_control_source_v1_add_listener(source->proxy, &kSourceListener,
                                              this);
      for (const auto& [mime, data] : source->entries)
        ext_data_control_source_v1_offer(source->proxy, mime.c_str());
      sources_[source->proxy] = source;
      ext_data_control_device_v1_set_selection(device_, source->proxy);
      wl_callback* cb = wl_display_sync(display_);
      sync_waiters_[cb] = synced;
      wl_callback_add_listener(cb, &kSyncListener, this);
    });
    if (!started)
      return false;
    done.wait_for(kCallTimeout);
    return true;
  }

  void SetWatching(bool on) { watching_ = on; }

 private:
  Client() = default;

  bool Connect() {
    // Only in a Wayland session: without WAYLAND_DISPLAY libwayland would try
    // "wayland-0" anyway.
    const char* name = getenv("WAYLAND_DISPLAY");
    if (!name || !*name)
      return false;
    display_ = wl_display_connect(nullptr);
    if (!display_)
      return false;
    wl_registry* registry = wl_display_get_registry(display_);
    wl_registry_add_listener(registry, &kRegistryListener, this);
    wl_display_roundtrip(display_);
    if (!manager_ || !seat_) {
      wl_registry_destroy(registry);
      wl_display_disconnect(display_);
      display_ = nullptr;
      return false;
    }
    device_ = ext_data_control_manager_v1_get_data_device(manager_, seat_);
    ext_data_control_device_v1_add_listener(device_, &kDeviceListener, this);
    // The current selection (data_offer, offer..., selection).
    wl_display_roundtrip(display_);
    wake_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_ < 0 || wl_display_get_error(display_) != 0) {
      if (wake_ >= 0)
        close(wake_);
      wl_display_disconnect(display_);
      display_ = nullptr;
      return false;
    }
    ok_ = true;
    return true;
  }

  void Start() {
    std::thread([this] { Loop(); }).detach();
  }

  void Post(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      tasks_.push_back(std::move(task));
    }
    uint64_t one = 1;
    ssize_t n = write(wake_, &one, sizeof(one));
    (void)n;
  }

  void Loop() {
    while (true) {
      while (wl_display_prepare_read(display_) != 0)
        wl_display_dispatch_pending(display_);
      wl_display_flush(display_);
      std::vector<pollfd> fds;
      fds.push_back({wl_display_get_fd(display_), POLLIN, 0});
      fds.push_back({wake_, POLLIN, 0});
      int timeout = -1;
      auto now = Clock::now();
      for (const auto& r : reads_) {
        fds.push_back({r.fd, POLLIN, 0});
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        r.deadline - now)
                        .count();
        int ms = static_cast<int>(std::max<int64_t>(0, left));
        timeout = timeout < 0 ? ms : std::min(timeout, ms);
      }
      int n = poll(fds.data(), fds.size(), timeout);
      if (n > 0 && (fds[0].revents & POLLIN))
        wl_display_read_events(display_);
      else
        wl_display_cancel_read(display_);
      wl_display_dispatch_pending(display_);
      if (wl_display_get_error(display_) != 0 ||
          (n > 0 && (fds[0].revents & (POLLERR | POLLHUP)))) {
        Fail();
        return;
      }
      if (n > 0 && (fds[1].revents & POLLIN)) {
        uint64_t count;
        ssize_t r = read(wake_, &count, sizeof(count));
        (void)r;
      }
      RunTasks();
      PumpReads();
    }
  }

  void RunTasks() {
    std::deque<std::function<void()>> tasks;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      tasks.swap(tasks_);
    }
    for (auto& t : tasks)
      t();
  }

  void PumpReads() {
    auto now = Clock::now();
    for (auto it = reads_.begin(); it != reads_.end();) {
      bool finished = false, found = false;
      char buf[65536];
      while (true) {
        ssize_t n = read(it->fd, buf, sizeof(buf));
        if (n > 0) {
          it->data.append(buf, static_cast<size_t>(n));
          if (it->data.size() > it->max_bytes) {
            finished = true;  // too large: not delivered
            break;
          }
          continue;
        }
        if (n == 0) {
          finished = found = true;
        } else if (errno == EINTR) {
          continue;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          finished = true;
        }
        break;
      }
      if (!finished && now >= it->deadline)
        finished = true;
      if (!finished) {
        ++it;
        continue;
      }
      close(it->fd);
      it->done->set_value({found, found ? std::move(it->data) : ""});
      it = reads_.erase(it);
    }
  }

  // The connection broke: answer everything still waiting, refuse the rest.
  void Fail() {
    ok_ = false;
    for (auto& r : reads_) {
      close(r.fd);
      r.done->set_value({false, ""});
    }
    reads_.clear();
    for (auto& [cb, p] : sync_waiters_)
      p->set_value();
    sync_waiters_.clear();
    RunTasks();
  }

  void DestroyOffer(ext_data_control_offer_v1* proxy) {
    if (!proxy)
      return;
    offers_.erase(proxy);
    ext_data_control_offer_v1_destroy(proxy);
  }

  // --- Listeners --------------------------------------------------------

  static void OnGlobal(void* data, wl_registry* registry, uint32_t name,
                       const char* interface, uint32_t version) {
    auto* self = static_cast<Client*>(data);
    if (strcmp(interface, "gtk_shell1") == 0)
      g_mutter = true;
    if (strcmp(interface, ext_data_control_manager_v1_interface.name) == 0 &&
        !self->manager_) {
      self->manager_ = static_cast<ext_data_control_manager_v1*>(
          wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface,
                           1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0 &&
               !self->seat_) {
      self->seat_ = static_cast<wl_seat*>(
          wl_registry_bind(registry, name, &wl_seat_interface, 1));
    }
    (void)version;
  }
  static void OnGlobalRemove(void*, wl_registry*, uint32_t) {}

  static void OnDataOffer(void* data, ext_data_control_device_v1*,
                          ext_data_control_offer_v1* id) {
    auto* self = static_cast<Client*>(data);
    Offer& offer = self->offers_[id];
    offer.proxy = id;
    ext_data_control_offer_v1_add_listener(id, &kOfferListener, self);
  }
  static void OnSelection(void* data, ext_data_control_device_v1*,
                          ext_data_control_offer_v1* id) {
    auto* self = static_cast<Client*>(data);
    if (self->selection_ && self->selection_ != id)
      self->DestroyOffer(self->selection_);
    self->selection_ = id;
    if (self->watching_)
      GtkRunAsync([] { FireClipboardChange(); });
  }
  static void OnFinished(void* data, ext_data_control_device_v1*) {
    // The device is gone (the seat went away): stop using the protocol.
    static_cast<Client*>(data)->ok_ = false;
  }
  static void OnPrimarySelection(void* data, ext_data_control_device_v1*,
                                 ext_data_control_offer_v1* id) {
    auto* self = static_cast<Client*>(data);
    if (id && id != self->selection_)
      self->DestroyOffer(id);
  }
  static void OnOffer(void* data, ext_data_control_offer_v1* offer,
                      const char* mime) {
    auto* self = static_cast<Client*>(data);
    auto it = self->offers_.find(offer);
    if (it != self->offers_.end() && mime)
      it->second.types.push_back(mime);
  }
  static void OnSend(void* data, ext_data_control_source_v1* proxy,
                     const char* mime, int32_t fd) {
    auto* self = static_cast<Client*>(data);
    auto it = self->sources_.find(proxy);
    std::shared_ptr<const std::string> payload;
    if (it != self->sources_.end() && mime) {
      for (const auto& [m, d] : it->second->entries) {
        if (m == mime) {
          payload = d;
          break;
        }
      }
    }
    if (!payload) {
      close(fd);
      return;
    }
    // At most kMaxWriters transfers at once: refuse the rest, so a peer
    // that asks again and again without reading can't pile up threads.
    if (g_writers.fetch_add(1) >= kMaxWriters) {
      g_writers--;
      close(fd);
      return;
    }
    // Off this thread: the reader may be this process, waiting on it.
    std::thread(WriteAndClose, fd, payload).detach();
  }
  static void OnCancelled(void* data, ext_data_control_source_v1* proxy) {
    auto* self = static_cast<Client*>(data);
    auto it = self->sources_.find(proxy);
    if (it != self->sources_.end()) {
      delete it->second;
      self->sources_.erase(it);
    }
    ext_data_control_source_v1_destroy(proxy);
  }
  static void OnSyncDone(void* data, wl_callback* cb, uint32_t) {
    auto* self = static_cast<Client*>(data);
    auto it = self->sync_waiters_.find(cb);
    if (it != self->sync_waiters_.end()) {
      it->second->set_value();
      self->sync_waiters_.erase(it);
    }
    wl_callback_destroy(cb);
  }

  static constexpr wl_registry_listener kRegistryListener = {OnGlobal,
                                                             OnGlobalRemove};
  static constexpr ext_data_control_device_v1_listener kDeviceListener = {
      OnDataOffer, OnSelection, OnFinished, OnPrimarySelection};
  static constexpr ext_data_control_offer_v1_listener kOfferListener = {
      OnOffer};
  static constexpr ext_data_control_source_v1_listener kSourceListener = {
      OnSend, OnCancelled};
  static constexpr wl_callback_listener kSyncListener = {OnSyncDone};

  std::atomic<bool> ok_{false};
  std::atomic<bool> watching_{false};
  wl_display* display_ = nullptr;
  ext_data_control_manager_v1* manager_ = nullptr;
  wl_seat* seat_ = nullptr;
  ext_data_control_device_v1* device_ = nullptr;
  int wake_ = -1;

  std::mutex tasks_mutex_;
  std::deque<std::function<void()>> tasks_;

  // The connection's thread only, from here on.
  std::map<ext_data_control_offer_v1*, Offer> offers_;
  ext_data_control_offer_v1* selection_ = nullptr;
  std::map<ext_data_control_source_v1*, Source*> sources_;
  std::vector<PendingRead> reads_;
  std::map<wl_callback*, std::shared_ptr<std::promise<void>>> sync_waiters_;
};

Client* Usable() {
  static const bool disabled = [] {
    const char* v = getenv("LAUFEY_CLIPBOARD");
    return v && strcmp(v, "gtk") == 0;
  }();
  if (disabled)
    return nullptr;
  Client* c = Client::Get();
  return c && c->ok() ? c : nullptr;
}

}  // namespace

int ActiveWriters() {
  return g_writers.load();
}

bool Available() {
  return Usable() != nullptr;
}

bool CompositorIsMutter() {
  Client::Get();  // connects once, recording the compositor's globals
  return g_mutter.load();
}

bool Types(std::vector<std::string>* out) {
  Client* c = Usable();
  if (!c)
    return false;
  *out = c->SelectionTypes();
  return true;
}

bool Read(const std::vector<std::string>& mimes, size_t max_bytes,
          std::string* out, std::string* mime_out, bool* found) {
  *found = false;
  Client* c = Usable();
  if (!c)
    return false;
  *found = c->Read(mimes, max_bytes, out, mime_out);
  return true;
}

bool Write(Entries entries) {
  Client* c = Usable();
  if (!c)
    return false;
  return c->Write(std::move(entries));
}

void Watch(bool on) {
  if (Client* c = Usable())
    c->SetWatching(on);
}

}  // namespace data_control
}  // namespace laufey_common
