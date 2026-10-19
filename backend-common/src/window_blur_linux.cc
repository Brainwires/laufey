// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// KWin's blur behind translucent windows; see laufey_window_blur.h.

#include "laufey_window_blur.h"

#include <wayland-client.h>
#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "laufey_platform_features.h"

namespace laufey_common {

namespace {

// --- org_kde_kwin_blur_manager / org_kde_kwin_blur (version 1) ---------------
//
// The interface tables of KDE's blur protocol, from its wire description:
//
//   org_kde_kwin_blur_manager
//     request 0 create(id: new_id<org_kde_kwin_blur>, surface: wl_surface)
//     request 1 unset(surface: wl_surface)
//   org_kde_kwin_blur
//     request 0 commit()
//     request 1 set_region(region: wl_region, allow null)
//     request 2 release()  (destructor)
//
// Neither interface has events. The names are the protocol's own (the
// compositor matches them); the symbols are ours, so they can't clash with
// another copy in the process.

extern const wl_interface kBlurInterface;

const wl_interface* kBlurTypes[] = {
    nullptr,
    &kBlurInterface,        // create: id
    &wl_surface_interface,  // create: surface
    &wl_surface_interface,  // unset: surface
    &wl_region_interface,   // set_region: region
};

const wl_message kManagerRequests[] = {
    {"create", "no", kBlurTypes + 1},
    {"unset", "o", kBlurTypes + 3},
};

const wl_message kBlurRequests[] = {
    {"commit", "", kBlurTypes + 0},
    {"set_region", "?o", kBlurTypes + 4},
    {"release", "", kBlurTypes + 0},
};

const wl_interface kManagerInterface = {
    "org_kde_kwin_blur_manager", 1, 2, kManagerRequests, 0, nullptr,
};

const wl_interface kBlurInterface = {
    "org_kde_kwin_blur", 1, 3, kBlurRequests, 0, nullptr,
};

constexpr uint32_t kManagerCreate = 0;
constexpr uint32_t kManagerUnset = 1;
constexpr uint32_t kBlurCommit = 0;
constexpr uint32_t kBlurSetRegion = 1;
constexpr uint32_t kBlurRelease = 2;

// One display's registry, read on a queue of our own.
struct WaylandClient {
  wl_event_queue* queue = nullptr;
  wl_registry* registry = nullptr;
  uint32_t manager_name = 0;
  wl_proxy* manager = nullptr;
  // Blur objects made from the current manager, and those made from one the
  // compositor has since removed (re-made on the next "on").
  std::set<wl_proxy*> live;
  std::set<wl_proxy*> stale;
};

void OnGlobal(void* data, wl_registry* registry, uint32_t name,
              const char* interface, uint32_t /*version*/) {
  auto* c = static_cast<WaylandClient*>(data);
  if (std::strcmp(interface, kManagerInterface.name) != 0 || c->manager)
    return;
  c->manager = static_cast<wl_proxy*>(
      wl_registry_bind(registry, name, &kManagerInterface, 1));
  c->manager_name = c->manager ? name : 0;
}

void OnGlobalRemove(void* data, wl_registry* /*registry*/, uint32_t name) {
  auto* c = static_cast<WaylandClient*>(data);
  if (!c->manager || name != c->manager_name)
    return;
  // No destructor request: the proxy only goes. The blur objects stay valid
  // client objects (the compositor keeps their resources until we release
  // them), but count as made from a manager that is gone.
  wl_proxy_destroy(c->manager);
  c->manager = nullptr;
  c->manager_name = 0;
  c->stale.insert(c->live.begin(), c->live.end());
  c->live.clear();
}

const wl_registry_listener kRegistryListener = {OnGlobal, OnGlobalRemove};

std::mutex g_wayland_mutex;
std::map<wl_display*, WaylandClient*> g_wayland_clients;

// The display's client, with its registry read up to now. Never freed: the
// display outlives every window (GTK's connection lives for the process).
WaylandClient* Client(wl_display* display) {
  WaylandClient*& c = g_wayland_clients[display];
  if (!c) {
    c = new WaylandClient();
    c->queue = wl_display_create_queue(display);
    auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
    if (!c->queue || !wrapper) {
      if (wrapper)
        wl_proxy_wrapper_destroy(wrapper);
      return c;
    }
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), c->queue);
    c->registry = wl_display_get_registry(wrapper);
    wl_proxy_wrapper_destroy(wrapper);
    if (c->registry)
      wl_registry_add_listener(c->registry, &kRegistryListener, c);
  }
  if (c->queue && c->registry)
    wl_display_roundtrip_queue(display, c->queue);
  return c;
}

void Release(WaylandClient* c, wl_proxy* blur) {
  c->live.erase(blur);
  c->stale.erase(blur);
  wl_proxy_marshal_flags(blur, kBlurRelease, nullptr,
                         wl_proxy_get_version(blur), WL_MARSHAL_FLAG_DESTROY);
}

// --- X11
// ----------------------------------------------------------------------

constexpr char kBlurAtom[] = "_KDE_NET_WM_BLUR_BEHIND_REGION";

xcb_atom_t InternAtom(xcb_connection_t* conn, const char* name,
                      bool only_if_exists) {
  xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(
      conn,
      xcb_intern_atom(conn, only_if_exists ? 1 : 0,
                      static_cast<uint16_t>(std::strlen(name)), name),
      nullptr);
  xcb_atom_t atom = reply ? reply->atom : static_cast<xcb_atom_t>(XCB_NONE);
  free(reply);
  return atom;
}

std::mutex g_x11_mutex;
xcb_connection_t* g_x11 = nullptr;
xcb_window_t g_x11_root = XCB_NONE;

// The private connection, opened (or opened again after it broke) on use.
// Under g_x11_mutex.
bool X11Locked() {
  if (g_x11 && !xcb_connection_has_error(g_x11))
    return true;
  if (g_x11)
    xcb_disconnect(g_x11);
  g_x11 = nullptr;
  const char* display = std::getenv("DISPLAY");
  if (!display || !*display)
    return false;
  int screen_num = 0;
  xcb_connection_t* conn = xcb_connect(nullptr, &screen_num);
  if (!conn || xcb_connection_has_error(conn)) {
    if (conn)
      xcb_disconnect(conn);
    return false;
  }
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
  for (int i = 0; i < screen_num && it.rem; i++)
    xcb_screen_next(&it);
  if (!it.rem) {
    xcb_disconnect(conn);
    return false;
  }
  g_x11 = conn;
  g_x11_root = it.data->root;
  return true;
}

}  // namespace

bool KwinBlurWaylandAvailable(wl_display* display) {
  if (!display)
    return false;
  std::lock_guard<std::mutex> lock(g_wayland_mutex);
  return Client(display)->manager != nullptr;
}

bool KwinBlurWaylandSet(wl_display* display, wl_surface* surface,
                        wl_proxy** blur, bool on) {
  if (!display || !surface || !blur)
    return false;
  std::lock_guard<std::mutex> lock(g_wayland_mutex);
  WaylandClient* c = Client(display);
  if (*blur && c->stale.count(*blur)) {
    Release(c, *blur);
    *blur = nullptr;
  }
  if (!on) {
    if (*blur) {
      Release(c, *blur);
      *blur = nullptr;
    }
    if (c->manager) {
      wl_proxy_marshal_flags(c->manager, kManagerUnset, nullptr,
                             wl_proxy_get_version(c->manager), 0, surface);
    }
    wl_display_flush(display);
    return true;
  }
  if (!c->manager)
    return false;
  if (!*blur) {
    *blur = wl_proxy_marshal_flags(c->manager, kManagerCreate, &kBlurInterface,
                                   wl_proxy_get_version(c->manager), 0, nullptr,
                                   surface);
    if (!*blur)
      return false;
    c->live.insert(*blur);
  }
  // A null region is the whole surface.
  wl_proxy_marshal_flags(*blur, kBlurSetRegion, nullptr,
                         wl_proxy_get_version(*blur), 0, nullptr);
  wl_proxy_marshal_flags(*blur, kBlurCommit, nullptr,
                         wl_proxy_get_version(*blur), 0);
  wl_display_flush(display);
  return true;
}

void KwinBlurWaylandForget(wl_proxy** blur) {
  if (!blur || !*blur)
    return;
  std::lock_guard<std::mutex> lock(g_wayland_mutex);
  for (auto& [display, c] : g_wayland_clients) {
    if (c->live.count(*blur) || c->stale.count(*blur)) {
      Release(c, *blur);
      break;
    }
  }
  *blur = nullptr;
}

bool KwinBlurX11AvailableOn(xcb_connection_t* conn, uint32_t root) {
  if (!conn || xcb_connection_has_error(conn))
    return false;
  // Only atoms that exist: KWin interns the blur atom when it announces it.
  xcb_atom_t blur = InternAtom(conn, kBlurAtom, true);
  xcb_atom_t supported = InternAtom(conn, "_NET_SUPPORTED", true);
  if (blur == XCB_NONE || supported == XCB_NONE)
    return false;
  bool found = false;
  uint32_t offset = 0;
  // _NET_SUPPORTED lists a few hundred atoms at most; read it in pieces.
  for (int pieces = 0; pieces < 64; pieces++) {
    xcb_get_property_reply_t* reply = xcb_get_property_reply(
        conn,
        xcb_get_property(conn, 0, root, supported, XCB_ATOM_ATOM, offset, 1024),
        nullptr);
    if (!reply)
      break;
    bool more = false;
    if (reply->type == XCB_ATOM_ATOM && reply->format == 32) {
      int n = xcb_get_property_value_length(reply) / 4;
      auto* atoms = static_cast<xcb_atom_t*>(xcb_get_property_value(reply));
      for (int i = 0; i < n && !found; i++)
        found = atoms[i] == blur;
      offset += static_cast<uint32_t>(n);
      more = reply->bytes_after > 0;
    }
    free(reply);
    if (found || !more)
      break;
  }
  return found;
}

bool KwinBlurX11SetOn(xcb_connection_t* conn, uint32_t root, uint32_t window,
                      bool on) {
  if (!conn || xcb_connection_has_error(conn) || window == XCB_NONE)
    return false;
  if (on && !KwinBlurX11AvailableOn(conn, root))
    return false;
  xcb_atom_t atom = InternAtom(conn, kBlurAtom, !on);
  if (atom == XCB_NONE)
    return !on;  // nothing was ever set: off is done
  // An empty CARDINAL list is the whole window.
  xcb_void_cookie_t cookie =
      on ? xcb_change_property_checked(conn, XCB_PROP_MODE_REPLACE, window,
                                       atom, XCB_ATOM_CARDINAL, 32, 0, nullptr)
         : xcb_delete_property_checked(conn, window, atom);
  xcb_generic_error_t* error = xcb_request_check(conn, cookie);
  bool ok = error == nullptr;
  free(error);
  return ok;
}

bool KwinBlurX11Available() {
  std::lock_guard<std::mutex> lock(g_x11_mutex);
  return X11Locked() && KwinBlurX11AvailableOn(g_x11, g_x11_root);
}

bool KwinBlurX11Set(uint32_t window, bool on) {
  std::lock_guard<std::mutex> lock(g_x11_mutex);
  return X11Locked() && KwinBlurX11SetOn(g_x11, g_x11_root, window, on);
}

std::string ProbeWindowBlur(std::string* reason) {
  std::string unused;
  std::string& why = reason ? *reason : unused;
  why.clear();
  std::string backend = DisplayBackend();
  if (backend == "wayland") {
    // WAYLAND_SOCKET hands one connection over, to whoever connects first:
    // it is GTK's (or Chromium's), never a probe's.
    const char* name = std::getenv("WAYLAND_DISPLAY");
    if (std::getenv("WAYLAND_SOCKET") || !name || !*name) {
      why =
          "can't probe the compositor: its connection was handed over "
          "with WAYLAND_SOCKET";
      return "";
    }
    wl_display* display = wl_display_connect(name);
    if (!display) {
      why = "can't connect to the Wayland display";
      return "";
    }
    bool found = false;
    wl_registry* registry = wl_display_get_registry(display);
    struct Seen {
      bool* found;
    } seen{&found};
    static const wl_registry_listener listener = {
        [](void* data, wl_registry*, uint32_t, const char* interface,
           uint32_t) {
          if (std::strcmp(interface, "org_kde_kwin_blur_manager") == 0)
            *static_cast<Seen*>(data)->found = true;
        },
        [](void*, wl_registry*, uint32_t) {}};
    if (registry) {
      wl_registry_add_listener(registry, &listener, &seen);
      wl_display_roundtrip(display);
      wl_registry_destroy(registry);
    }
    wl_display_disconnect(display);
    if (found)
      return "kwin-wayland";
    why =
        "the compositor offers no org_kde_kwin_blur_manager (KWin offers "
        "it while its Blur desktop effect is on)";
    return "";
  }
  if (backend == "x11") {
    if (KwinBlurX11Available())
      return "kwin-x11";
    why =
        "the window manager doesn't list _KDE_NET_WM_BLUR_BEHIND_REGION in "
        "_NET_SUPPORTED (KWin does while its Blur desktop effect is on)";
    return "";
  }
  why = "no display";
  return "";
}

}  // namespace laufey_common
