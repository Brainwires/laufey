// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The X11 CLIPBOARD selection over a private XCB connection; see the header.
// One connection, opened on the first use by a thread of its own, which then
// serves it: requests from any thread run there as tasks, conversions and
// outgoing INCR transfers are state machines driven by the connection's
// events, so neither ever blocks the thread (the owner of the selection may
// be this very bridge, answering on the same thread), and every one has a
// deadline.

#include "clipboard_x11_linux.h"

#include <errno.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/uio.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "laufey_io.h"

namespace laufey_common {
namespace x11_clipboard {

namespace {

using Clock = std::chrono::steady_clock;
using data_control::Entries;

// How long connecting may take (Xwayland may be starting on demand).
constexpr auto kConnectTimeout = std::chrono::seconds(8);
// How long a caller waits for the bridge's thread at all.
constexpr auto kCallTimeout = std::chrono::seconds(5);
// How long the owner gets for each step of a conversion (the answer, each
// INCR chunk), and for the whole of it.
constexpr auto kStepTimeout = std::chrono::seconds(3);
constexpr auto kConversionTimeout = std::chrono::seconds(10);
// How long a requestor of our selection may take nothing before its INCR
// transfer is dropped, and how long a transfer may take in all.
constexpr auto kTransferIdleTimeout = std::chrono::seconds(3);
constexpr auto kTransferTimeout = std::chrono::seconds(30);
// Data larger than this goes through INCR, in chunks this size (well under
// the core protocol's 256 KiB request limit).
constexpr size_t kChunk = 64 * 1024;
// Outgoing INCR transfers in flight at once; requests beyond are refused.
constexpr int kMaxTransfers = 8;

std::atomic<bool> g_forced{false};
std::atomic<int> g_transfers{0};

// XFIXES, spoken with raw requests (no libxcb-xfixes dependency): selection
// owner changes, for change events.
xcb_extension_t g_xfixes = {"XFIXES", 0};
constexpr uint8_t kXfixesQueryVersion = 0;
constexpr uint8_t kXfixesSelectSelectionInput = 2;
constexpr uint32_t kXfixesSelectionMask = 1 | 2 | 4;  // owner, destroy, close

bool XfixesSetUp(xcb_connection_t* conn, xcb_window_t window,
                 xcb_atom_t selection, uint8_t* first_event) {
  const xcb_query_extension_reply_t* ext =
      xcb_get_extension_data(conn, &g_xfixes);
  if (!ext || !ext->present)
    return false;
  struct {
    uint8_t major_opcode;
    uint8_t minor_opcode;
    uint16_t length;
    uint32_t client_major;
    uint32_t client_minor;
  } version = {0, 0, 0, 5, 0};
  struct iovec parts[4];
  parts[2].iov_base = &version;
  parts[2].iov_len = sizeof(version);
  parts[3].iov_base = nullptr;
  parts[3].iov_len = 0;
  xcb_protocol_request_t version_req = {2, &g_xfixes, kXfixesQueryVersion, 0};
  unsigned int seq =
      xcb_send_request(conn, XCB_REQUEST_CHECKED, parts + 2, &version_req);
  xcb_generic_error_t* error = nullptr;
  void* reply = xcb_wait_for_reply(conn, seq, &error);
  free(error);
  if (!reply)
    return false;
  free(reply);
  struct {
    uint8_t major_opcode;
    uint8_t minor_opcode;
    uint16_t length;
    uint32_t window;
    uint32_t selection;
    uint32_t event_mask;
  } select = {0, 0, 0, window, selection, kXfixesSelectionMask};
  parts[2].iov_base = &select;
  parts[2].iov_len = sizeof(select);
  xcb_protocol_request_t select_req = {2, &g_xfixes,
                                       kXfixesSelectSelectionInput, 1};
  xcb_send_request(conn, 0, parts + 2, &select_req);
  *first_event = ext->first_event;
  return true;
}

// A conversion of the CLIPBOARD selection to one target, into our window's
// transfer property.
struct Conversion {
  xcb_atom_t target = XCB_NONE;
  size_t max_bytes = 0;
  bool incr = false;
  bool started = false;
  std::string data;
  xcb_atom_t type = XCB_NONE;
  Clock::time_point step_deadline;
  Clock::time_point deadline;
  // ok, data, the property type.
  std::function<void(bool, std::string, xcb_atom_t)> done;
};

// An outgoing INCR transfer to a requestor of our selection.
struct Transfer {
  xcb_window_t requestor = XCB_NONE;
  xcb_atom_t property = XCB_NONE;
  xcb_atom_t type = XCB_NONE;
  std::shared_ptr<const std::string> data;
  size_t offset = 0;
  bool finished = false;  // the zero-length chunk went out
  Clock::time_point idle_deadline;
  Clock::time_point deadline;
};

class Bridge {
 public:
  static Bridge* Get() {
    static Bridge* bridge = [] {
      auto* b = new Bridge();  // never freed: its thread may outlive us
      std::thread([b] { b->Main(); }).detach();
      std::unique_lock<std::mutex> lock(b->connect_mutex_);
      if (!b->connect_cv_.wait_for(lock,
                                   kConnectTimeout + std::chrono::seconds(1),
                                   [b] { return b->connect_done_; })) {
        b->abandoned_ = true;
        return static_cast<Bridge*>(nullptr);
      }
      return b->connected_ ? b : nullptr;
    }();
    return bridge;
  }

  bool ok() const {
    return ok_.load();
  }

  // Runs `fn(live)` on the bridge's thread; see data-control's Client::Run.
  bool Run(std::function<void(bool)> fn) {
    if (!ok())
      return false;
    auto ran = std::make_shared<std::promise<bool>>();
    std::future<bool> done = ran->get_future();
    Post([fn = std::move(fn), ran](bool live) {
      fn(live);
      ran->set_value(live);
    });
    return done.wait_for(kCallTimeout) == std::future_status::ready &&
           done.get();
  }

  // The selection's targets; false when the bridge is gone. An empty list
  // is an empty clipboard (no owner) or an owner that didn't answer.
  bool Targets(std::vector<std::string>* out) {
    auto result = std::make_shared<std::promise<std::vector<xcb_atom_t>>>();
    std::future<std::vector<xcb_atom_t>> f = result->get_future();
    if (!Run([this, result](bool live) {
          if (!live) {
            result->set_value({});
            return;
          }
          ConvertTargets([result](std::vector<xcb_atom_t> atoms) {
            result->set_value(std::move(atoms));
          });
        }))
      return false;
    if (f.wait_for(kConversionTimeout + std::chrono::seconds(1)) !=
        std::future_status::ready)
      return true;
    std::vector<xcb_atom_t> atoms = f.get();
    // Names, on the bridge's thread (it owns the connection).
    auto names = std::make_shared<std::vector<std::string>>();
    if (!Run([this, atoms, names](bool live) {
          if (!live)
            return;
          for (xcb_atom_t a : atoms) {
            std::string name = AtomName(a);
            if (!name.empty() && name != "TARGETS" && name != "TIMESTAMP" &&
                name != "MULTIPLE" && name != "SAVE_TARGETS")
              names->push_back(name);
          }
        }))
      return false;
    *out = *names;
    return true;
  }

  // The data of the first of `mimes` the selection offers. False when the
  // bridge is gone; *found false when nothing wanted is offered, the owner
  // didn't answer in time, or the data exceeds `max_bytes`.
  bool Read(const std::vector<std::string>& mimes, size_t max_bytes,
            std::string* out, std::string* mime_out, bool* found,
            std::string* type_out) {
    *found = false;
    struct Result {
      bool ok = false;
      std::string data, mime, type;
    };
    auto result = std::make_shared<std::promise<Result>>();
    std::future<Result> f = result->get_future();
    if (!Run([this, mimes, max_bytes, result](bool live) {
          if (!live) {
            result->set_value({});
            return;
          }
          ConvertTargets([this, mimes, max_bytes,
                          result](std::vector<xcb_atom_t> offered) {
            xcb_atom_t target = XCB_NONE;
            std::string mime;
            for (const auto& m : mimes) {
              xcb_atom_t a = Atom(m);
              if (a != XCB_NONE && std::find(offered.begin(), offered.end(),
                                             a) != offered.end()) {
                target = a;
                mime = m;
                break;
              }
            }
            if (target == XCB_NONE) {
              result->set_value({});
              return;
            }
            Convert(target, max_bytes,
                    [this, mime, result](bool ok, std::string data,
                                         xcb_atom_t type) {
                      Result r;
                      r.ok = ok;
                      r.data = std::move(data);
                      r.mime = mime;
                      r.type = ok ? AtomName(type) : "";
                      result->set_value(std::move(r));
                    });
          });
        }))
      return false;
    // Two conversions (TARGETS, then the data), each with its deadline.
    if (f.wait_for(2 * kConversionTimeout + std::chrono::seconds(1)) !=
        std::future_status::ready)
      return true;
    Result r = f.get();
    if (!r.ok)
      return true;
    *found = true;
    *out = std::move(r.data);
    if (mime_out)
      *mime_out = r.mime;
    if (type_out)
      *type_out = r.type;
    return true;
  }

  // Takes the CLIPBOARD selection, offering `entries` (target name, data).
  // True once the server confirmed us as the owner.
  bool Write(Entries entries) {
    auto owned = std::make_shared<std::promise<bool>>();
    std::future<bool> f = owned->get_future();
    auto shared = std::make_shared<Entries>(std::move(entries));
    auto start = Clock::now();
    if (!Run([this, shared, owned](bool live) {
          if (!live) {
            owned->set_value(false);
            return;
          }
          Own(shared, [owned](bool ok) { owned->set_value(ok); });
        }))
      return false;
    return f.wait_until(start + kCallTimeout) == std::future_status::ready &&
           f.get();
  }

  void SetWatching(bool on) {
    watching_ = on;
  }

 private:
  Bridge() = default;

  void Main() {
    bool connected = Connect();
    {
      std::lock_guard<std::mutex> lock(connect_mutex_);
      if (abandoned_ && connected) {
        Teardown();
        connected = false;
      }
      connected_ = connected;
      ok_ = connected;
      connect_done_ = true;
    }
    connect_cv_.notify_all();
    if (connected)
      Loop();
  }

  bool Connect() {
    int screen_num = 0;
    conn_ = xcb_connect(nullptr, &screen_num);  // DISPLAY
    if (!conn_ || xcb_connection_has_error(conn_)) {
      Teardown();
      return false;
    }
    const xcb_setup_t* setup = xcb_get_setup(conn_);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num && it.rem; i++)
      xcb_screen_next(&it);
    if (!it.rem) {
      Teardown();
      return false;
    }
    root_ = it.data->root;
    window_ = xcb_generate_id(conn_);
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, window_, root_, -1, -1, 1, 1,
                      0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                      XCB_CW_EVENT_MASK, &mask);
    clipboard_ = Atom("CLIPBOARD");
    targets_ = Atom("TARGETS");
    timestamp_ = Atom("TIMESTAMP");
    multiple_ = Atom("MULTIPLE");
    incr_ = Atom("INCR");
    transfer_prop_ = Atom("LAUFEY_CLIPBOARD");
    time_prop_ = Atom("LAUFEY_CLIPBOARD_TIME");
    if (clipboard_ == XCB_NONE || targets_ == XCB_NONE || incr_ == XCB_NONE ||
        transfer_prop_ == XCB_NONE || time_prop_ == XCB_NONE) {
      Teardown();
      return false;
    }
    xfixes_ = XfixesSetUp(conn_, window_, clipboard_, &xfixes_event_);
    wake_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    xcb_flush(conn_);
    if (wake_ < 0 || xcb_connection_has_error(conn_)) {
      Teardown();
      return false;
    }
    return true;
  }

  void Teardown() {
    if (conn_)
      xcb_disconnect(conn_);
    conn_ = nullptr;
  }

  // --- Atoms ------------------------------------------------------------

  xcb_atom_t Atom(const std::string& name) {
    auto it = atoms_.find(name);
    if (it != atoms_.end())
      return it->second;
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(
        conn_,
        xcb_intern_atom(conn_, 0, static_cast<uint16_t>(name.size()),
                        name.c_str()),
        nullptr);
    if (!reply)
      return XCB_NONE;
    xcb_atom_t atom = reply->atom;
    free(reply);
    atoms_[name] = atom;
    names_[atom] = name;
    return atom;
  }

  std::string AtomName(xcb_atom_t atom) {
    if (atom == XCB_NONE)
      return "";
    auto it = names_.find(atom);
    if (it != names_.end())
      return it->second;
    xcb_get_atom_name_reply_t* reply =
        xcb_get_atom_name_reply(conn_, xcb_get_atom_name(conn_, atom), nullptr);
    if (!reply)
      return "";
    std::string name(xcb_get_atom_name_name(reply),
                     static_cast<size_t>(xcb_get_atom_name_name_length(reply)));
    free(reply);
    names_[atom] = name;
    atoms_[name] = atom;
    return name;
  }

  // --- Tasks ------------------------------------------------------------

  void Post(std::function<void(bool)> task) {
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      if (!dead_) {
        tasks_.push_back(std::move(task));
        uint64_t one = 1;
        ssize_t n = write(wake_, &one, sizeof(one));
        (void)n;
        return;
      }
    }
    task(false);
  }

  void RunTasks() {
    std::deque<std::function<void(bool)>> tasks;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      tasks.swap(tasks_);
    }
    for (auto& t : tasks)
      t(true);
  }

  void Loop() {
    while (true) {
      RunTasks();
      while (xcb_generic_event_t* ev = xcb_poll_for_event(conn_)) {
        Handle(ev);
        free(ev);
      }
      CheckDeadlines();
      StartConversion();
      xcb_flush(conn_);
      if (xcb_connection_has_error(conn_)) {
        Fail();
        return;
      }
      // Events read while waiting for a reply are queued already.
      if (xcb_generic_event_t* ev = xcb_poll_for_queued_event(conn_)) {
        Handle(ev);
        free(ev);
        continue;
      }
      pollfd fds[2] = {{xcb_get_file_descriptor(conn_), POLLIN, 0},
                       {wake_, POLLIN, 0}};
      int n = poll(fds, 2, NextTimeoutMs());
      if (n > 0 && (fds[1].revents & POLLIN)) {
        uint64_t count;
        ssize_t r = read(wake_, &count, sizeof(count));
        (void)r;
      }
    }
  }

  int NextTimeoutMs() {
    Clock::time_point next = Clock::time_point::max();
    if (!conversions_.empty() && conversions_.front().started) {
      next = std::min({next, conversions_.front().step_deadline,
                       conversions_.front().deadline});
    }
    for (const auto& t : transfers_)
      next = std::min({next, t.idle_deadline, t.deadline});
    if (own_pending_)
      next = std::min(next, own_deadline_);
    if (next == Clock::time_point::max())
      return -1;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  next - Clock::now())
                  .count();
    return static_cast<int>(std::max<int64_t>(0, ms) + 1);
  }

  void CheckDeadlines() {
    auto now = Clock::now();
    if (!conversions_.empty() && conversions_.front().started &&
        (now >= conversions_.front().step_deadline ||
         now >= conversions_.front().deadline))
      FinishConversion(false);
    for (auto it = transfers_.begin(); it != transfers_.end();) {
      if (now >= it->idle_deadline || now >= it->deadline) {
        // A requestor that stopped taking chunks: give up on it.
        it = EndTransfer(it);
      } else {
        ++it;
      }
    }
    if (own_pending_ && now >= own_deadline_)
      FinishOwn(false);
  }

  // The connection broke: everything waiting is answered, every task from
  // now on runs at once with `live` false.
  void Fail() {
    ok_ = false;
    std::deque<std::function<void(bool)>> tasks;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      dead_ = true;
      tasks.swap(tasks_);
    }
    for (auto& t : tasks)
      t(false);
    while (!conversions_.empty())
      FinishConversion(false);
    while (!transfers_.empty())
      EndTransfer(transfers_.begin());
    if (own_pending_)
      FinishOwn(false);
    owned_.reset();
    Teardown();
    close(wake_);
    wake_ = -1;
  }

  // --- Reading ----------------------------------------------------------

  void ConvertTargets(std::function<void(std::vector<xcb_atom_t>)> done) {
    Convert(
        targets_, 64 * 1024,
        [done = std::move(done)](bool ok, std::string data, xcb_atom_t type) {
          std::vector<xcb_atom_t> atoms;
          if (ok && type == XCB_ATOM_ATOM) {
            atoms.resize(data.size() / sizeof(xcb_atom_t));
            memcpy(atoms.data(), data.data(),
                   atoms.size() * sizeof(xcb_atom_t));
          }
          done(std::move(atoms));
        });
  }

  // Queues a conversion; one runs at a time (they share our property).
  void Convert(xcb_atom_t target, size_t max_bytes,
               std::function<void(bool, std::string, xcb_atom_t)> done) {
    Conversion c;
    c.target = target;
    c.max_bytes = max_bytes;
    c.done = std::move(done);
    conversions_.push_back(std::move(c));
    StartConversion();
  }

  void StartConversion() {
    if (conversions_.empty() || conversions_.front().started)
      return;
    Conversion& c = conversions_.front();
    c.started = true;
    auto now = Clock::now();
    c.step_deadline = now + kStepTimeout;
    c.deadline = now + kConversionTimeout;
    xcb_delete_property(conn_, window_, transfer_prop_);
    xcb_convert_selection(conn_, window_, clipboard_, c.target, transfer_prop_,
                          XCB_CURRENT_TIME);
    xcb_flush(conn_);
  }

  void FinishConversion(bool ok) {
    Conversion c = std::move(conversions_.front());
    conversions_.pop_front();
    if (c.incr || ok)
      xcb_delete_property(conn_, window_, transfer_prop_);
    c.done(ok, ok ? std::move(c.data) : "", c.type);
    StartConversion();
  }

  // Reads our transfer property whole (in chunks), deleting it. False when
  // it is unreadable or larger than `max_bytes`.
  bool TakeProperty(size_t max_bytes, std::string* out, xcb_atom_t* type) {
    out->clear();
    uint32_t offset = 0;  // in 32-bit units
    while (true) {
      xcb_get_property_reply_t* reply = xcb_get_property_reply(
          conn_,
          xcb_get_property(conn_, 0, window_, transfer_prop_,
                           XCB_GET_PROPERTY_TYPE_ANY, offset, kChunk / 4),
          nullptr);
      if (!reply)
        return false;
      *type = reply->type;
      int len = xcb_get_property_value_length(reply);
      if (len > 0) {
        out->append(static_cast<const char*>(xcb_get_property_value(reply)),
                    static_cast<size_t>(len));
      }
      uint32_t after = reply->bytes_after;
      free(reply);
      if (out->size() > max_bytes)
        return false;
      if (after == 0 || len <= 0)
        break;
      offset += static_cast<uint32_t>(len) / 4;
    }
    xcb_delete_property(conn_, window_, transfer_prop_);
    return true;
  }

  void OnSelectionNotify(const xcb_selection_notify_event_t* e) {
    if (e->requestor != window_ || conversions_.empty() ||
        !conversions_.front().started || conversions_.front().incr)
      return;
    Conversion& c = conversions_.front();
    if (e->target != c.target)
      return;  // a late answer to an earlier conversion
    if (e->property == XCB_NONE) {
      FinishConversion(false);  // refused, or no owner
      return;
    }
    std::string data;
    xcb_atom_t type = XCB_NONE;
    if (!TakeProperty(c.max_bytes, &data, &type)) {
      FinishConversion(false);
      return;
    }
    if (type == incr_) {
      // INCR: the owner sends chunks, each once we deleted the last one
      // (TakeProperty did), ending with an empty one.
      c.incr = true;
      c.step_deadline = Clock::now() + kStepTimeout;
      return;
    }
    c.data = std::move(data);
    c.type = type;
    FinishConversion(true);
  }

  void OnOurPropertyNewValue() {
    if (conversions_.empty() || !conversions_.front().incr)
      return;
    Conversion& c = conversions_.front();
    std::string chunk;
    xcb_atom_t type = XCB_NONE;
    if (!TakeProperty(c.max_bytes, &chunk, &type)) {
      FinishConversion(false);
      return;
    }
    if (chunk.empty()) {
      FinishConversion(true);
      return;
    }
    c.type = type;
    c.data.append(chunk);
    if (c.data.size() > c.max_bytes) {
      FinishConversion(false);  // too large: not delivered
      return;
    }
    c.step_deadline = Clock::now() + kStepTimeout;
  }

  // --- Owning -----------------------------------------------------------

  // Becoming the owner needs a server timestamp (ICCCM: not CurrentTime):
  // a zero-length append to a property of ours reports one.
  void Own(std::shared_ptr<Entries> entries, std::function<void(bool)> done) {
    if (own_pending_)
      FinishOwn(false);  // superseded
    own_pending_ = true;
    own_entries_ = std::move(entries);
    own_done_ = std::move(done);
    own_deadline_ = Clock::now() + kStepTimeout;
    xcb_change_property(conn_, XCB_PROP_MODE_APPEND, window_, time_prop_,
                        XCB_ATOM_STRING, 8, 0, nullptr);
    xcb_flush(conn_);
  }

  void OnTimestamp(xcb_timestamp_t time) {
    if (!own_pending_)
      return;
    xcb_set_selection_owner(conn_, window_, clipboard_, time);
    xcb_get_selection_owner_reply_t* reply = xcb_get_selection_owner_reply(
        conn_, xcb_get_selection_owner(conn_, clipboard_), nullptr);
    bool ok = reply && reply->owner == window_;
    free(reply);
    if (ok) {
      owned_ = own_entries_;
      owned_time_ = time;
    }
    FinishOwn(ok);
  }

  void FinishOwn(bool ok) {
    own_pending_ = false;
    own_entries_.reset();
    auto done = std::move(own_done_);
    own_done_ = nullptr;
    if (done)
      done(ok);
  }

  void Reply(const xcb_selection_request_event_t* e, xcb_atom_t property) {
    xcb_selection_notify_event_t n = {};
    n.response_type = XCB_SELECTION_NOTIFY;
    n.time = e->time;
    n.requestor = e->requestor;
    n.selection = e->selection;
    n.target = e->target;
    n.property = property;
    xcb_send_event(conn_, 0, e->requestor, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char*>(&n));
    xcb_flush(conn_);
  }

  void OnSelectionRequest(const xcb_selection_request_event_t* e) {
    // ICCCM: a requestor that names no property is an obsolete client; use
    // the target's name.
    xcb_atom_t property = e->property == XCB_NONE ? e->target : e->property;
    if (!owned_ || e->owner != window_ || e->selection != clipboard_ ||
        (e->time != XCB_CURRENT_TIME && e->time < owned_time_) ||
        e->target == multiple_) {
      Reply(e, XCB_NONE);
      return;
    }
    if (e->target == targets_) {
      std::vector<xcb_atom_t> list = {targets_, timestamp_};
      for (const auto& [mime, data] : *owned_) {
        xcb_atom_t a = Atom(mime);
        if (a != XCB_NONE)
          list.push_back(a);
      }
      xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, e->requestor, property,
                          XCB_ATOM_ATOM, 32, static_cast<uint32_t>(list.size()),
                          list.data());
      Reply(e, property);
      return;
    }
    if (e->target == timestamp_) {
      xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, e->requestor, property,
                          XCB_ATOM_INTEGER, 32, 1, &owned_time_);
      Reply(e, property);
      return;
    }
    std::shared_ptr<const std::string> data;
    for (const auto& [mime, d] : *owned_) {
      if (Atom(mime) == e->target) {
        data = d;
        break;
      }
    }
    if (!data) {
      Reply(e, XCB_NONE);
      return;
    }
    if (data->size() <= kChunk) {
      xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, e->requestor, property,
                          e->target, 8, static_cast<uint32_t>(data->size()),
                          data->data());
      Reply(e, property);
      return;
    }
    // INCR, at most kMaxTransfers at once.
    if (static_cast<int>(transfers_.size()) >= kMaxTransfers) {
      Reply(e, XCB_NONE);
      return;
    }
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(conn_, e->requestor, XCB_CW_EVENT_MASK, &mask);
    uint32_t size = static_cast<uint32_t>(data->size());
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, e->requestor, property,
                        incr_, 32, 1, &size);
    Transfer t;
    t.requestor = e->requestor;
    t.property = property;
    t.type = e->target;
    t.data = data;
    auto now = Clock::now();
    t.idle_deadline = now + kTransferIdleTimeout;
    t.deadline = now + kTransferTimeout;
    transfers_.push_back(std::move(t));
    g_transfers = static_cast<int>(transfers_.size());
    Reply(e, property);
  }

  // The requestor deleted the property: the next chunk (an empty one last).
  void OnRequestorPropertyDeleted(xcb_window_t window, xcb_atom_t atom) {
    for (auto it = transfers_.begin(); it != transfers_.end(); ++it) {
      if (it->requestor != window || it->property != atom)
        continue;
      if (it->finished) {
        EndTransfer(it);
        return;
      }
      size_t n = std::min(kChunk, it->data->size() - it->offset);
      xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window, atom, it->type,
                          8, static_cast<uint32_t>(n),
                          it->data->data() + it->offset);
      it->offset += n;
      if (n == 0)
        it->finished = true;
      it->idle_deadline = Clock::now() + kTransferIdleTimeout;
      xcb_flush(conn_);
      return;
    }
  }

  std::vector<Transfer>::iterator EndTransfer(
      std::vector<Transfer>::iterator it) {
    xcb_window_t window = it->requestor;
    it = transfers_.erase(it);
    g_transfers = static_cast<int>(transfers_.size());
    bool more = std::any_of(
        transfers_.begin(), transfers_.end(),
        [window](const Transfer& t) { return t.requestor == window; });
    if (!more && conn_ && window != window_) {
      // Stop listening to that window's properties.
      uint32_t none = XCB_EVENT_MASK_NO_EVENT;
      xcb_change_window_attributes(conn_, window, XCB_CW_EVENT_MASK, &none);
    }
    return it;
  }

  // --- Events -----------------------------------------------------------

  void Handle(xcb_generic_event_t* ev) {
    uint8_t type = ev->response_type & 0x7f;
    switch (type) {
      case 0:
        // An error: a requestor window that vanished mid-transfer, say.
        // Its transfer ends at its deadline.
        return;
      case XCB_SELECTION_NOTIFY:
        OnSelectionNotify(reinterpret_cast<xcb_selection_notify_event_t*>(ev));
        return;
      case XCB_SELECTION_REQUEST:
        OnSelectionRequest(
            reinterpret_cast<xcb_selection_request_event_t*>(ev));
        return;
      case XCB_SELECTION_CLEAR: {
        auto* e = reinterpret_cast<xcb_selection_clear_event_t*>(ev);
        if (e->selection == clipboard_)
          owned_.reset();  // another client owns the clipboard now
        return;
      }
      case XCB_PROPERTY_NOTIFY: {
        auto* e = reinterpret_cast<xcb_property_notify_event_t*>(ev);
        if (e->window == window_) {
          // We may be the requestor of our own INCR transfer, too.
          if (e->state == XCB_PROPERTY_DELETE)
            OnRequestorPropertyDeleted(e->window, e->atom);
          if (e->atom == time_prop_ && e->state == XCB_PROPERTY_NEW_VALUE)
            OnTimestamp(e->time);
          else if (e->atom == transfer_prop_ &&
                   e->state == XCB_PROPERTY_NEW_VALUE)
            OnOurPropertyNewValue();
        } else if (e->state == XCB_PROPERTY_DELETE) {
          OnRequestorPropertyDeleted(e->window, e->atom);
        }
        return;
      }
      default:
        if (xfixes_ && type == xfixes_event_ && watching_)
          GtkRunAsync([] { FireClipboardChange(); });
        return;
    }
  }

  std::atomic<bool> ok_{false};
  std::atomic<bool> watching_{false};

  std::mutex connect_mutex_;
  std::condition_variable connect_cv_;
  bool connect_done_ = false;
  bool connected_ = false;
  bool abandoned_ = false;

  std::mutex tasks_mutex_;
  std::deque<std::function<void(bool)>> tasks_;
  bool dead_ = false;  // under tasks_mutex_
  int wake_ = -1;      // written under tasks_mutex_

  // The bridge's thread only, from here on.
  xcb_connection_t* conn_ = nullptr;
  xcb_window_t root_ = XCB_NONE;
  xcb_window_t window_ = XCB_NONE;
  xcb_atom_t clipboard_ = XCB_NONE, targets_ = XCB_NONE, timestamp_ = XCB_NONE,
             multiple_ = XCB_NONE, incr_ = XCB_NONE, transfer_prop_ = XCB_NONE,
             time_prop_ = XCB_NONE;
  bool xfixes_ = false;
  uint8_t xfixes_event_ = 0;
  std::map<std::string, xcb_atom_t> atoms_;
  std::map<xcb_atom_t, std::string> names_;
  std::deque<Conversion> conversions_;
  std::vector<Transfer> transfers_;
  std::shared_ptr<Entries> owned_;
  xcb_timestamp_t owned_time_ = 0;
  bool own_pending_ = false;
  std::shared_ptr<Entries> own_entries_;
  std::function<void(bool)> own_done_;
  Clock::time_point own_deadline_;
};

bool Enabled() {
  if (g_forced.load())
    return true;
  static const bool enabled = [] {
    const char* forced = getenv("LAUFEY_CLIPBOARD");
    if (forced && strcmp(forced, "gtk") == 0)
      return false;
    const char* wayland = getenv("WAYLAND_DISPLAY");
    const char* x11 = getenv("DISPLAY");
    return wayland && *wayland && x11 && *x11 &&
           data_control::CompositorIsMutter();
  }();
  return enabled;
}

Bridge* Usable() {
  if (!Enabled())
    return nullptr;
  Bridge* b = Bridge::Get();
  return b && b->ok() ? b : nullptr;
}

}  // namespace

bool Available() {
  return Usable() != nullptr;
}

bool Types(std::vector<std::string>* out) {
  Bridge* b = Usable();
  return b && b->Targets(out);
}

bool Read(const std::vector<std::string>& mimes, size_t max_bytes,
          std::string* out, std::string* mime_out, bool* found,
          std::string* type_out) {
  *found = false;
  Bridge* b = Usable();
  return b && b->Read(mimes, max_bytes, out, mime_out, found, type_out);
}

bool Write(Entries entries) {
  Bridge* b = Usable();
  return b && b->Write(std::move(entries));
}

void Watch(bool on) {
  if (Bridge* b = Usable())
    b->SetWatching(on);
}

void EnableForTesting() {
  g_forced = true;
}

int ActiveTransfers() {
  return g_transfers.load();
}

}  // namespace x11_clipboard
}  // namespace laufey_common
