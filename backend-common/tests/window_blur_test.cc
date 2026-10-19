// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// KWin's blur (laufey_window_blur.h), on both display servers, against
// stand-ins that record what the client asks for:
//   - Wayland: a compositor of the test's own (libwayland-server) offering
//     wl_compositor and, when asked, org_kde_kwin_blur_manager. The blur is
//     found by its global, set on a surface (create, set_region(null) =
//     the whole surface, commit), unset (release, unset); a global removed
//     while the app runs is no longer offered, a blur object made from it is
//     replaced once the global is back, and a compositor without the global
//     (any but KWin with its Blur effect on) refuses. The session probe
//     reports "kwin-wayland" or why not.
//   - X11: a private Xvfb whose root window's _NET_SUPPORTED the test writes,
//     standing in for KWin (which lists _KDE_NET_WM_BLUR_BEHIND_REGION there
//     while the effect is loaded): found only when listed (also past the
//     first 1024 atoms), the property written as an empty CARDINAL/32 list
//     (the whole window) and deleted for off, refused for a window that
//     doesn't exist and once the atom is no longer listed. The session probe
//     reports "kwin-x11" or why not.
// Exits 77 (skipped) when neither part could run (no Xvfb and no
// temporary directory for a socket); each part says when it is skipped.

#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <xcb/xcb.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "laufey_platform_features.h"
#include "laufey_window_blur.h"

using namespace laufey_common;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      Finish(1);                                                             \
    }                                                                        \
  } while (0)

namespace {

pid_t g_xvfb = -1;
std::string g_runtime_dir;

[[noreturn]] void Finish(int code) {
  if (g_xvfb > 0)
    kill(g_xvfb, SIGTERM);
  std::fflush(stdout);
  std::_Exit(code);
}

// --- A compositor of our own
// ---------------------------------------------------

// The blur protocol's tables (the same wire description the client uses).
extern const wl_interface kBlur;
const wl_interface* kTypes[] = {nullptr, &kBlur, &wl_surface_interface,
                                &wl_surface_interface, &wl_region_interface};
const wl_message kManagerRequests[] = {{"create", "no", kTypes + 1},
                                       {"unset", "o", kTypes + 3}};
const wl_message kBlurRequests[] = {{"commit", "", kTypes + 0},
                                    {"set_region", "?o", kTypes + 4},
                                    {"release", "", kTypes + 0}};
const wl_interface kManager = {
    "org_kde_kwin_blur_manager", 1, 2, kManagerRequests, 0, nullptr};
const wl_interface kBlur = {"org_kde_kwin_blur", 1, 3,
                            kBlurRequests,       0, nullptr};

class Compositor {
 public:
  explicit Compositor(bool offer_blur) {
    display_ = wl_display_create();
    EXPECT(display_);
    const char* socket = wl_display_add_socket_auto(display_);
    EXPECT(socket);
    socket_ = socket;
    wl_global_create(display_, &wl_compositor_interface, 1, this,
                     BindCompositor);
    if (offer_blur)
      AddBlur();
    thread_ = std::thread([this] { Loop(); });
  }
  ~Compositor() {
    stop_ = true;
    thread_.join();
    wl_display_destroy(display_);
  }

  const std::string& socket() const {
    return socket_;
  }

  // Runs `fn` on the compositor's thread and waits for it.
  void Run(std::function<void()> fn) {
    std::atomic<bool> done{false};
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back([&] {
        fn();
        done = true;
      });
    }
    while (!done)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  void AddBlur() {
    blur_global_ = wl_global_create(display_, &kManager, 1, this, BindManager);
  }
  void RemoveBlur() {
    Run([this] {
      wl_global_destroy(blur_global_);
      blur_global_ = nullptr;
    });
  }
  void AddBlurLater() {
    Run([this] { AddBlur(); });
  }

  // The requests seen so far ("interface.request", with "(null)" for a
  // null object argument), cleared.
  std::vector<std::string> Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.swap(log_);
    return out;
  }

 private:
  void Loop() {
    wl_event_loop* loop = wl_display_get_event_loop(display_);
    while (!stop_) {
      std::deque<std::function<void()>> tasks;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks.swap(tasks_);
      }
      for (auto& t : tasks)
        t();
      wl_event_loop_dispatch(loop, 5);
      wl_display_flush_clients(display_);
    }
  }

  void Log(std::string entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_.push_back(std::move(entry));
  }

  static void BindCompositor(wl_client* client, void* data, uint32_t version,
                             uint32_t id) {
    wl_resource* r =
        wl_resource_create(client, &wl_compositor_interface, version, id);
    wl_resource_set_dispatcher(r, Dispatch, nullptr, data, nullptr);
  }
  static void BindManager(wl_client* client, void* data, uint32_t version,
                          uint32_t id) {
    wl_resource* r = wl_resource_create(client, &kManager, version, id);
    wl_resource_set_dispatcher(r, Dispatch, nullptr, data, nullptr);
  }

  // Every request of every object: logged; new objects created with this
  // dispatcher; destructors destroy.
  static int Dispatch(const void*, void* target, uint32_t opcode,
                      const wl_message* message, wl_argument* args) {
    auto* resource = static_cast<wl_resource*>(target);
    auto* self = static_cast<Compositor*>(wl_resource_get_user_data(resource));
    std::string name =
        std::string(wl_resource_get_class(resource)) + "." + message->name;
    // The signature's arguments, in order ('?' marks nullable ones).
    int i = 0;
    for (const char* p = message->signature; *p; p++) {
      if (*p == '?' || (*p >= '0' && *p <= '9'))
        continue;
      if (*p == 'n') {
        const wl_interface* type = message->types[i];
        wl_resource* made =
            wl_resource_create(wl_resource_get_client(resource), type,
                               wl_resource_get_version(resource), args[i].n);
        wl_resource_set_dispatcher(made, Dispatch, nullptr, self, nullptr);
      } else if (*p == 'o' && !args[i].o) {
        name += "(null)";
      }
      i++;
    }
    self->Log(name);
    std::string req = message->name;
    if (req == "release" || req == "destroy")
      wl_resource_destroy(resource);
    (void)opcode;
    return 0;
  }

  wl_display* display_ = nullptr;
  std::string socket_;
  wl_global* blur_global_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::deque<std::function<void()>> tasks_;
  std::vector<std::string> log_;
};

// The client: a connection with a wl_compositor bound and a surface made.
struct Client {
  wl_display* display = nullptr;
  wl_compositor* compositor = nullptr;
  wl_surface* surface = nullptr;
};

void OnGlobal(void* data, wl_registry* registry, uint32_t name,
              const char* interface, uint32_t) {
  auto* c = static_cast<Client*>(data);
  if (std::strcmp(interface, "wl_compositor") == 0) {
    c->compositor = static_cast<wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, 1));
  }
}
void OnGlobalRemove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kListener = {OnGlobal, OnGlobalRemove};

Client Connect(const std::string& socket) {
  Client c;
  c.display = wl_display_connect(socket.c_str());
  EXPECT(c.display);
  wl_registry* registry = wl_display_get_registry(c.display);
  wl_registry_add_listener(registry, &kListener, &c);
  wl_display_roundtrip(c.display);
  EXPECT(c.compositor);
  c.surface = wl_compositor_create_surface(c.compositor);
  EXPECT(c.surface);
  wl_display_roundtrip(c.display);
  return c;
}

bool Has(const std::vector<std::string>& log, const std::string& entry) {
  for (const auto& e : log) {
    if (e == entry)
      return true;
  }
  return false;
}

// What the compositor saw by now (after a round trip), in order.
std::vector<std::string> Seen(Compositor& comp, Client& c) {
  wl_display_roundtrip(c.display);
  return comp.Take();
}

std::string Join(const std::vector<std::string>& v) {
  std::string out;
  for (const auto& e : v)
    out += (out.empty() ? "" : " ") + e;
  return out;
}

bool WaylandPart() {
  // A runtime directory of our own: the sockets never land in a session's.
  char tmpl[] = "/tmp/laufey-blur-XXXXXX";
  if (!mkdtemp(tmpl)) {
    std::printf("wayland part skipped: no temporary directory\n");
    return false;
  }
  g_runtime_dir = tmpl;
  chmod(tmpl, 0700);
  setenv("XDG_RUNTIME_DIR", tmpl, 1);
  unsetenv("WAYLAND_SOCKET");

  // --- KWin with its Blur effect on: the global is there.
  {
    Compositor comp(true);
    Client c = Connect(comp.socket());
    comp.Take();
    EXPECT(KwinBlurWaylandAvailable(c.display));

    wl_proxy* blur = nullptr;
    EXPECT(KwinBlurWaylandSet(c.display, c.surface, &blur, true));
    EXPECT(blur);
    std::vector<std::string> seen = Seen(comp, c);
    std::printf("on: %s\n", Join(seen).c_str());
    EXPECT(seen.size() == 3);
    EXPECT(seen[0] == "org_kde_kwin_blur_manager.create");
    EXPECT(seen[1] == "org_kde_kwin_blur.set_region(null)");
    EXPECT(seen[2] == "org_kde_kwin_blur.commit");

    // On again (e.g. after a map): the same object, set again.
    wl_proxy* same = blur;
    EXPECT(KwinBlurWaylandSet(c.display, c.surface, &blur, true));
    EXPECT(blur == same);
    seen = Seen(comp, c);
    EXPECT(seen.size() == 2 && !Has(seen, "org_kde_kwin_blur_manager.create"));

    // Off: released, and unset on the surface.
    EXPECT(KwinBlurWaylandSet(c.display, c.surface, &blur, false));
    EXPECT(!blur);
    seen = Seen(comp, c);
    std::printf("off: %s\n", Join(seen).c_str());
    EXPECT(Has(seen, "org_kde_kwin_blur.release"));
    EXPECT(Has(seen, "org_kde_kwin_blur_manager.unset"));

    // The surface goes away (GTK unmaps): only released.
    EXPECT(KwinBlurWaylandSet(c.display, c.surface, &blur, true));
    Seen(comp, c);
    KwinBlurWaylandForget(&blur);
    EXPECT(!blur);
    seen = Seen(comp, c);
    EXPECT(seen.size() == 1 && seen[0] == "org_kde_kwin_blur.release");

    // The effect turned off: the global goes, the blur is refused, and off
    // still succeeds (nothing to unset).
    EXPECT(KwinBlurWaylandSet(c.display, c.surface, &blur, true));
    Seen(comp, c);
    comp.RemoveBlur();
    EXPECT(!KwinBlurWaylandAvailable(c.display));
    wl_proxy* other = nullptr;
    EXPECT(!KwinBlurWaylandSet(c.display, c.surface, &other, true));
    EXPECT(!other);
    // ... and back on: the object made from the old manager is replaced.
    comp.AddBlurLater();
    EXPECT(KwinBlurWaylandAvailable(c.display));
    comp.Take();
    wl_proxy* old = blur;
    EXPECT(KwinBlurWaylandSet(c.display, c.surface, &blur, true));
    EXPECT(blur);
    (void)old;
    seen = Seen(comp, c);
    std::printf("re-made: %s\n", Join(seen).c_str());
    EXPECT(seen.size() == 4);
    EXPECT(seen[0] == "org_kde_kwin_blur.release");
    EXPECT(seen[1] == "org_kde_kwin_blur_manager.create");

    // Arguments that can't name a surface.
    EXPECT(!KwinBlurWaylandSet(nullptr, c.surface, &blur, true));
    EXPECT(!KwinBlurWaylandSet(c.display, nullptr, &blur, true));
    EXPECT(!KwinBlurWaylandAvailable(nullptr));

    // The session probe, on a connection of its own.
    setenv("WAYLAND_DISPLAY", comp.socket().c_str(), 1);
    std::string reason;
    EXPECT(ProbeWindowBlur(&reason) == "kwin-wayland");
    EXPECT(reason.empty());
    // A connection handed over with WAYLAND_SOCKET is never taken.
    setenv("WAYLAND_SOCKET", "99", 1);
    EXPECT(ProbeWindowBlur(&reason).empty());
    EXPECT(reason.find("WAYLAND_SOCKET") != std::string::npos);
    unsetenv("WAYLAND_SOCKET");

    KwinBlurWaylandSet(c.display, c.surface, &blur, false);
    wl_display_roundtrip(c.display);
    // The connection stays open: the blur client keeps per-display state
    // for the process's life (GTK's display never goes away).
  }

  // --- Any other compositor: no global, no blur.
  {
    Compositor comp(false);
    Client c = Connect(comp.socket());
    comp.Take();
    EXPECT(!KwinBlurWaylandAvailable(c.display));
    wl_proxy* blur = nullptr;
    EXPECT(!KwinBlurWaylandSet(c.display, c.surface, &blur, true));
    EXPECT(!blur);
    EXPECT(KwinBlurWaylandSet(c.display, c.surface, &blur, false));
    std::vector<std::string> seen = Seen(comp, c);
    EXPECT(seen.empty());
    setenv("WAYLAND_DISPLAY", comp.socket().c_str(), 1);
    std::string reason;
    EXPECT(ProbeWindowBlur(&reason).empty());
    EXPECT(reason.find("org_kde_kwin_blur_manager") != std::string::npos);
  }
  unsetenv("WAYLAND_DISPLAY");
  std::printf("wayland part passed\n");
  return true;
}

// --- X11 ---------------------------------------------------------------------

pid_t StartXvfb() {
  gchar* xvfb = g_find_program_in_path("Xvfb");
  if (!xvfb)
    return -1;
  int fds[2];
  EXPECT(pipe(fds) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int null = open("/dev/null", O_RDWR);
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    std::string fd = std::to_string(fds[1]);
    execl(xvfb, xvfb, "-displayfd", fd.c_str(), "-nolisten", "tcp", "-screen",
          "0", "640x480x24", static_cast<char*>(nullptr));
    _exit(127);
  }
  g_free(xvfb);
  close(fds[1]);
  pollfd p = {fds[0], POLLIN, 0};
  char buf[32] = {};
  if (poll(&p, 1, 10000) != 1 || read(fds[0], buf, sizeof(buf) - 1) <= 0) {
    kill(pid, SIGKILL);
    return -1;
  }
  close(fds[0]);
  std::string display = std::string(":") + strtok(buf, "\n");
  setenv("DISPLAY", display.c_str(), 1);
  return pid;
}

xcb_atom_t Atom(xcb_connection_t* c, const std::string& name) {
  xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(
      c,
      xcb_intern_atom(c, 0, static_cast<uint16_t>(name.size()), name.c_str()),
      nullptr);
  EXPECT(r);
  xcb_atom_t a = r->atom;
  free(r);
  return a;
}

void SetSupported(xcb_connection_t* c, xcb_window_t root,
                  const std::vector<xcb_atom_t>& atoms) {
  xcb_change_property(c, XCB_PROP_MODE_REPLACE, root, Atom(c, "_NET_SUPPORTED"),
                      XCB_ATOM_ATOM, 32, static_cast<uint32_t>(atoms.size()),
                      atoms.data());
  xcb_flush(c);
  free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), nullptr));
}

// The blur property on `w`: whether it exists, its type, format and length.
bool BlurProperty(xcb_connection_t* c, xcb_window_t w, xcb_atom_t* type,
                  uint8_t* format, int* length) {
  xcb_get_property_reply_t* r = xcb_get_property_reply(
      c,
      xcb_get_property(c, 0, w, Atom(c, "_KDE_NET_WM_BLUR_BEHIND_REGION"),
                       XCB_GET_PROPERTY_TYPE_ANY, 0, 64),
      nullptr);
  EXPECT(r);
  bool exists = r->type != XCB_NONE;
  *type = r->type;
  *format = r->format;
  *length = xcb_get_property_value_length(r);
  free(r);
  return exists;
}

bool X11Part() {
  g_xvfb = StartXvfb();
  if (g_xvfb < 0) {
    std::printf("x11 part skipped: no Xvfb\n");
    return false;
  }
  unsetenv("WAYLAND_DISPLAY");
  xcb_connection_t* c = xcb_connect(nullptr, nullptr);
  EXPECT(c && !xcb_connection_has_error(c));
  xcb_window_t root = xcb_setup_roots_iterator(xcb_get_setup(c)).data->root;
  xcb_window_t w = xcb_generate_id(c);
  xcb_create_window(c, XCB_COPY_FROM_PARENT, w, root, 0, 0, 100, 100, 0,
                    XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0,
                    nullptr);
  xcb_flush(c);

  // No window manager at all: nothing announced.
  EXPECT(!KwinBlurX11AvailableOn(c, root));
  EXPECT(!KwinBlurX11Available());
  EXPECT(!KwinBlurX11Set(w, true));
  std::string reason;
  EXPECT(ProbeWindowBlur(&reason).empty());
  EXPECT(reason.find("_NET_SUPPORTED") != std::string::npos);

  // A window manager without the blur (the atom may exist, unlisted).
  xcb_atom_t blur = Atom(c, "_KDE_NET_WM_BLUR_BEHIND_REGION");
  SetSupported(c, root, {Atom(c, "_NET_WM_NAME"), Atom(c, "_NET_WM_STATE")});
  EXPECT(!KwinBlurX11AvailableOn(c, root));
  EXPECT(!KwinBlurX11Available());

  // KWin with the effect: listed, here past the first 1024 entries.
  std::vector<xcb_atom_t> many;
  for (int i = 0; i < 1500; i++)
    many.push_back(Atom(c, "_LAUFEY_TEST_" + std::to_string(i)));
  many.push_back(blur);
  SetSupported(c, root, many);
  EXPECT(KwinBlurX11AvailableOn(c, root));
  EXPECT(KwinBlurX11Available());
  EXPECT(ProbeWindowBlur(&reason) == "kwin-x11");

  // On: an empty CARDINAL/32 list (the whole window).
  EXPECT(KwinBlurX11Set(w, true));
  xcb_atom_t type = XCB_NONE;
  uint8_t format = 0;
  int length = -1;
  EXPECT(BlurProperty(c, w, &type, &format, &length));
  EXPECT(type == XCB_ATOM_CARDINAL && format == 32 && length == 0);
  // Off: gone.
  EXPECT(KwinBlurX11Set(w, false));
  EXPECT(!BlurProperty(c, w, &type, &format, &length));
  // Off again is fine.
  EXPECT(KwinBlurX11Set(w, false));
  // A window that doesn't exist: refused.
  EXPECT(!KwinBlurX11Set(0x3fffff0, true));

  // The effect turned off: no longer listed, refused; off still works.
  SetSupported(c, root, {Atom(c, "_NET_WM_NAME")});
  EXPECT(!KwinBlurX11Available());
  EXPECT(!KwinBlurX11Set(w, true));
  EXPECT(!BlurProperty(c, w, &type, &format, &length));
  EXPECT(KwinBlurX11Set(w, false));

  xcb_disconnect(c);
  std::printf("x11 part passed\n");
  return true;
}

}  // namespace

int main() {
  bool wayland = WaylandPart();
  bool x11 = X11Part();
  if (!g_runtime_dir.empty())
    rmdir(g_runtime_dir.c_str());
  if (!wayland && !x11) {
    std::printf("laufey_window_blur_test: nothing could run, skipped\n");
    Finish(77);
  }
  std::printf("laufey_window_blur_test: ok\n");
  Finish(0);
}
