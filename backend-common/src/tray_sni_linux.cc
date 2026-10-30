// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The tray icon as a StatusNotifierItem of our own over GDBus (API 50), with
// its menu exported as com.canonical.dbusmenu. AppIndicator answers a click
// on the icon with its menu and reports nothing to the app; a StatusNotifier
// host calls the item's Activate (primary click), SecondaryActivate (middle
// click) and ContextMenu (right click), with the click point where the host
// fills it in, so the app gets every click. Used whenever the session has a
// StatusNotifierWatcher (KDE Plasma, the GNOME AppIndicator extension,
// waybar, xfce4-panel's SNI plugin, ...); LAUFEY_TRAY_BACKEND=appindicator
// keeps the AppIndicator path. Without a watcher (an X11 session with only
// an XEmbed tray) AppIndicator is used as before.
//
// Every function here runs on the GTK main thread (tray_linux.cc marshals),
// where the session bus delivers the method calls too.

#include "laufey_backend_common.h"
#include "laufey_launch_config.h"
#include "laufey_menu.h"
#include "tray_sni_linux.h"

#include <gio/gio.h>
#include <gtk/gtk.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

constexpr char kWatcherName[] = "org.kde.StatusNotifierWatcher";
constexpr char kWatcherPath[] = "/StatusNotifierWatcher";

const char kSniXml[] = R"XML(<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
    <method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewAttentionIcon"/>
    <signal name="NewOverlayIcon"/>
    <signal name="NewToolTip"/>
    <signal name="NewStatus"><arg type="s"/></signal>
  </interface>
</node>)XML";

const char kMenuXml[] = R"XML(<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
      <arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg type="ai" direction="in"/><arg type="as" direction="in"/>
      <arg type="a(ia{sv})" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg type="i" direction="in"/><arg type="s" direction="in"/>
      <arg type="v" direction="out"/>
    </method>
    <method name="Event">
      <arg type="i" direction="in"/><arg type="s" direction="in"/>
      <arg type="v" direction="in"/><arg type="u" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg type="a(isvu)" direction="in"/><arg type="ai" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg type="i" direction="in"/><arg type="b" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg type="ai" direction="in"/><arg type="ai" direction="out"/><arg type="ai" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg type="a(ia{sv})"/><arg type="a(ias)"/>
    </signal>
    <signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
    <signal name="ItemActivationRequested"><arg type="i"/><arg type="u"/></signal>
  </interface>
</node>)XML";

// One dbusmenu item (id 0 is the root).
struct MenuNode {
  std::string type;  // "" or "separator"
  std::string label;
  std::string click_key;  // "" for the root, submenus and separators
  bool enabled = true;
  bool checked = false;
  bool submenu = false;
  std::vector<uint8_t> icon_png;
  std::vector<int32_t> children;
};

struct SniTray {
  uint32_t id = 0;
  std::string item_path;
  std::string menu_path;
  guint item_reg = 0;
  guint menu_reg = 0;
  std::string title;
  // ARGB32, network byte order, as IconPixmap wants it.
  int icon_w = 0;
  int icon_h = 0;
  std::vector<uint8_t> icon_argb;
  std::string icon_theme_path;
  std::string icon_name;
  uint32_t revision = 1;
  std::map<int32_t, MenuNode> nodes;
  laufey_menu_click_fn menu_click_fn = nullptr;
  void* menu_click_data = nullptr;
  laufey_tray_click_fn click_fn = nullptr;
  void* click_data = nullptr;
  laufey_tray_click_fn dblclick_fn = nullptr;
  void* dblclick_data = nullptr;
  laufey_tray_event_fn event_fn = nullptr;
  void* event_data = nullptr;
  gint64 last_activate_us = 0;
  int last_x = 0;
  int last_y = 0;
};

GDBusConnection* g_bus = nullptr;
GDBusNodeInfo* g_sni_info = nullptr;
GDBusNodeInfo* g_menu_info = nullptr;
guint g_watch = 0;
std::map<uint32_t, std::unique_ptr<SniTray>>& Trays() {
  static auto* map = new std::map<uint32_t, std::unique_ptr<SniTray>>();
  return *map;
}

SniTray* TrayFor(gpointer user_data) {
  uint32_t id = GPOINTER_TO_UINT(user_data);
  auto it = Trays().find(id);
  return it == Trays().end() ? nullptr : it->second.get();
}

GDBusConnection* Bus() {
  if (!g_bus) {
    GError* error = nullptr;
    g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (error)
      g_error_free(error);
  }
  return g_bus;
}

// dbusmenu reads '_' as a mnemonic marker: double it for a literal one.
std::string EscapeLabel(const std::string& s) {
  std::string out;
  for (char c : s) {
    out += c;
    if (c == '_')
      out += '_';
  }
  return out;
}

const char* RoleLabelSni(const std::string& role) {
  static const char* const kRoles[][2] = {
      {"quit", "Quit"},
      {"copy", "Copy"},
      {"paste", "Paste"},
      {"cut", "Cut"},
      {"selectall", "Select All"},
      {"undo", "Undo"},
      {"redo", "Redo"},
      {"minimize", "Minimize"},
      {"close", "Close"},
      {"about", "About"},
      {"togglefullscreen", "Toggle Full Screen"},
  };
  for (const auto& r : kRoles) {
    if (RoleIs(role, r[0]))
      return r[1];
  }
  return nullptr;
}

void AddNodes(SniTray& tray, int32_t parent,
              const std::vector<MenuEntry>& entries, int32_t* next_id) {
  for (const MenuEntry& e : entries) {
    MenuNode node;
    switch (e.kind) {
      case MenuEntry::Kind::kSeparator:
        node.type = "separator";
        break;
      case MenuEntry::Kind::kRole: {
        const char* label =
            e.label.empty() ? RoleLabelSni(e.role) : e.label.c_str();
        if (!label)
          continue;
        node.label = label;
        node.click_key = e.role;
        break;
      }
      case MenuEntry::Kind::kSubmenu:
        node.label = e.label;
        node.submenu = true;
        break;
      case MenuEntry::Kind::kItem:
        node.label = e.label;
        node.click_key = e.ClickKey();
        node.enabled = e.enabled;
        node.checked = e.checked;
        node.icon_png = e.icon_png;
        RegisterMenuClick(node.click_key, tray.menu_click_fn,
                          tray.menu_click_data, tray.id);
        break;
    }
    int32_t id = (*next_id)++;
    tray.nodes[parent].children.push_back(id);
    tray.nodes[id] = node;
    if (e.kind == MenuEntry::Kind::kSubmenu)
      AddNodes(tray, id, e.children, next_id);
  }
}

GVariant* NodeProperties(const MenuNode& n, GVariant* names) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  auto wanted = [names](const char* key) {
    if (!names || g_variant_n_children(names) == 0)
      return true;
    for (gsize i = 0; i < g_variant_n_children(names); i++) {
      const gchar* s = nullptr;
      g_variant_get_child(names, i, "&s", &s);
      if (s && strcmp(s, key) == 0)
        return true;
    }
    return false;
  };
  if (!n.type.empty() && wanted("type"))
    g_variant_builder_add(&b, "{sv}", "type",
                          g_variant_new_string(n.type.c_str()));
  if (n.type.empty() && wanted("label"))
    g_variant_builder_add(&b, "{sv}", "label",
                          g_variant_new_string(EscapeLabel(n.label).c_str()));
  if (!n.enabled && wanted("enabled"))
    g_variant_builder_add(&b, "{sv}", "enabled", g_variant_new_boolean(FALSE));
  if (n.checked) {
    if (wanted("toggle-type"))
      g_variant_builder_add(&b, "{sv}", "toggle-type",
                            g_variant_new_string("checkmark"));
    if (wanted("toggle-state"))
      g_variant_builder_add(&b, "{sv}", "toggle-state", g_variant_new_int32(1));
  }
  if (n.submenu && wanted("children-display"))
    g_variant_builder_add(&b, "{sv}", "children-display",
                          g_variant_new_string("submenu"));
  if (!n.icon_png.empty() && wanted("icon-data"))
    g_variant_builder_add(
        &b, "{sv}", "icon-data",
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, n.icon_png.data(),
                                  n.icon_png.size(), 1));
  return g_variant_builder_end(&b);
}

GVariant* Layout(const SniTray& tray, int32_t id, int depth, GVariant* names) {
  auto it = tray.nodes.find(id);
  MenuNode empty;
  const MenuNode& n = it == tray.nodes.end() ? empty : it->second;
  GVariantBuilder children;
  g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
  if (depth != 0) {
    for (int32_t child : n.children) {
      g_variant_builder_add(
          &children, "v",
          Layout(tray, child, depth < 0 ? -1 : depth - 1, names));
    }
  }
  GVariant* props = NodeProperties(n, names);
  if (id == 0) {
    // The root: a submenu holding the items.
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "children-display",
                          g_variant_new_string("submenu"));
    g_variant_unref(g_variant_ref_sink(props));
    props = g_variant_builder_end(&b);
  }
  return g_variant_new("(i@a{sv}av)", id, props, &children);
}

GVariant* Pixmaps(const SniTray& tray) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
  if (!tray.icon_argb.empty()) {
    g_variant_builder_add(
        &b, "(ii@ay)", tray.icon_w, tray.icon_h,
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, tray.icon_argb.data(),
                                  tray.icon_argb.size(), 1));
  }
  return g_variant_builder_end(&b);
}

GVariant* EmptyPixmaps() {
  return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), nullptr, 0);
}

GVariant* SniGetProperty(GDBusConnection*, const gchar*, const gchar*,
                         const gchar*, const gchar* name, GError**,
                         gpointer user_data) {
  SniTray* tray = TrayFor(user_data);
  if (!tray)
    return nullptr;
  std::string n = name;
  if (n == "Category")
    return g_variant_new_string("ApplicationStatus");
  if (n == "Id") {
    std::string app = LaunchAppId();
    std::string id = (app.empty() ? std::string("laufey") : app) + "-tray-" +
                     std::to_string(tray->id);
    return g_variant_new_string(id.c_str());
  }
  if (n == "Title")
    return g_variant_new_string(tray->title.c_str());
  if (n == "Status")
    return g_variant_new_string("Active");
  if (n == "WindowId")
    return g_variant_new_int32(0);
  if (n == "IconThemePath")
    return g_variant_new_string(tray->icon_theme_path.c_str());
  if (n == "IconName")
    return g_variant_new_string(tray->icon_name.c_str());
  if (n == "IconPixmap")
    return Pixmaps(*tray);
  if (n == "OverlayIconName" || n == "AttentionIconName" ||
      n == "AttentionMovieName")
    return g_variant_new_string("");
  if (n == "OverlayIconPixmap" || n == "AttentionIconPixmap")
    return EmptyPixmaps();
  if (n == "ToolTip") {
    return g_variant_new("(s@a(iiay)ss)", "", EmptyPixmaps(),
                         tray->title.c_str(), "");
  }
  if (n == "ItemIsMenu")
    return g_variant_new_boolean(FALSE);
  if (n == "Menu")
    return g_variant_new_object_path(tray->menu_path.c_str());
  return nullptr;
}

void EmitEvent(SniTray* tray, int32_t kind, int32_t button, int x, int y) {
  if (!tray->event_fn)
    return;
  laufey_tray_event_t e = {};
  e.tray_id = tray->id;
  e.kind = kind;
  e.button = button;
  // The host doesn't say which modifiers are held; GDK knows only for our
  // own windows, so none are reported (LAUFEY_TRAY_CAP_MODIFIERS unset).
  // Hosts that send no point send (0, 0).
  if (x != 0 || y != 0) {
    e.x = x;
    e.y = y;
    e.has_position = true;
  }
  tray->event_fn(tray->event_data, &e);
}

void SniMethodCall(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                   const gchar* method, GVariant* params,
                   GDBusMethodInvocation* invocation, gpointer user_data) {
  uint32_t tray_id = GPOINTER_TO_UINT(user_data);
  SniTray* tray = TrayFor(user_data);
  std::string m = method;
  // Reply first: a handler may destroy the tray (and its registration).
  g_dbus_method_invocation_return_value(invocation, nullptr);
  if (!tray)
    return;
  gint32 x = 0, y = 0;
  if (m == "Activate" || m == "SecondaryActivate" || m == "ContextMenu")
    g_variant_get(params, "(ii)", &x, &y);
  if (m == "Activate") {
    // SNI has no double click: two activations within the desktop's
    // double-click time make one.
    gint double_ms = 400;
    g_object_get(gtk_settings_get_default(), "gtk-double-click-time",
                 &double_ms, nullptr);
    gint64 now = g_get_monotonic_time();
    bool is_double = tray->last_activate_us &&
                     now - tray->last_activate_us <= (gint64)double_ms * 1000;
    tray->last_activate_us = is_double ? 0 : now;
    laufey_tray_click_fn fn = is_double ? tray->dblclick_fn : tray->click_fn;
    void* data = is_double ? tray->dblclick_data : tray->click_data;
    EmitEvent(
        tray,
        is_double ? LAUFEY_TRAY_EVENT_DOUBLE_CLICK : LAUFEY_TRAY_EVENT_CLICK,
        LAUFEY_MOUSE_BUTTON_LEFT, x, y);
    if (fn && Trays().count(tray_id))
      fn(data, tray_id);
  } else if (m == "SecondaryActivate") {
    EmitEvent(tray, LAUFEY_TRAY_EVENT_MIDDLE_CLICK, LAUFEY_MOUSE_BUTTON_MIDDLE,
              x, y);
  } else if (m == "ContextMenu") {
    EmitEvent(tray, LAUFEY_TRAY_EVENT_RIGHT_CLICK, LAUFEY_MOUSE_BUTTON_RIGHT, x,
              y);
  }
}

GVariant* MenuGetProperty(GDBusConnection*, const gchar*, const gchar*,
                          const gchar*, const gchar* name, GError**, gpointer) {
  std::string n = name;
  if (n == "Version")
    return g_variant_new_uint32(3);
  if (n == "TextDirection")
    return g_variant_new_string("ltr");
  if (n == "Status")
    return g_variant_new_string("normal");
  if (n == "IconThemePath")
    return g_variant_new_strv(nullptr, 0);
  return nullptr;
}

// Reports a click on menu item `id` (the "clicked" event).
void ClickMenuItem(uint32_t tray_id, int32_t id) {
  auto it = Trays().find(tray_id);
  if (it == Trays().end())
    return;
  SniTray* tray = it->second.get();
  auto node = tray->nodes.find(id);
  if (node == tray->nodes.end() || node->second.click_key.empty() ||
      !node->second.enabled || !tray->menu_click_fn)
    return;
  std::string key = node->second.click_key;
  tray->menu_click_fn(tray->menu_click_data, tray_id, key.c_str());
}

void MenuMethodCall(GDBusConnection*, const gchar*, const gchar*, const gchar*,
                    const gchar* method, GVariant* params,
                    GDBusMethodInvocation* invocation, gpointer user_data) {
  uint32_t tray_id = GPOINTER_TO_UINT(user_data);
  SniTray* tray = TrayFor(user_data);
  std::string m = method;
  if (m == "GetLayout") {
    gint32 parent = 0, depth = -1;
    GVariant* names = nullptr;
    g_variant_get(params, "(ii@as)", &parent, &depth, &names);
    GVariant* layout = nullptr;
    if (tray) {
      layout = Layout(*tray, parent, depth, names);
    } else {
      GVariantBuilder none;
      g_variant_builder_init(&none, G_VARIANT_TYPE("av"));
      layout = g_variant_new(
          "(i@a{sv}av)", 0,
          g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0), &none);
    }
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(u@(ia{sv}av))", tray ? tray->revision : 0, layout));
    g_variant_unref(names);
    return;
  }
  if (m == "GetGroupProperties") {
    GVariant* ids = nullptr;
    GVariant* names = nullptr;
    g_variant_get(params, "(@ai@as)", &ids, &names);
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a(ia{sv})"));
    if (tray) {
      gsize n = 0;
      const gint32* list =
          static_cast<const gint32*>(g_variant_get_fixed_array(ids, &n, 4));
      for (gsize i = 0; i < n; i++) {
        auto node = tray->nodes.find(list[i]);
        if (node != tray->nodes.end())
          g_variant_builder_add(&b, "(i@a{sv})", list[i],
                                NodeProperties(node->second, names));
      }
    }
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(a(ia{sv}))", &b));
    g_variant_unref(ids);
    g_variant_unref(names);
    return;
  }
  if (m == "GetProperty") {
    gint32 id = 0;
    const gchar* name = nullptr;
    g_variant_get(params, "(i&s)", &id, &name);
    GVariant* value = nullptr;
    if (tray) {
      auto node = tray->nodes.find(id);
      if (node != tray->nodes.end()) {
        GVariant* one = g_variant_new_strv(&name, 1);
        GVariant* props = NodeProperties(node->second, one);
        g_variant_ref_sink(props);
        value = g_variant_lookup_value(props, name, nullptr);
        g_variant_unref(props);
        g_variant_unref(g_variant_ref_sink(one));
      }
    }
    if (!value) {
      g_dbus_method_invocation_return_dbus_error(
          invocation, "com.canonical.dbusmenu.Error", "no such property");
      return;
    }
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(v)", value));
    return;
  }
  if (m == "Event") {
    gint32 id = 0;
    const gchar* event = nullptr;
    GVariant* data = nullptr;
    guint32 timestamp = 0;
    g_variant_get(params, "(i&svu)", &id, &event, &data, &timestamp);
    bool clicked = event && strcmp(event, "clicked") == 0;
    if (data)
      g_variant_unref(data);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    if (clicked)
      ClickMenuItem(tray_id, id);
    return;
  }
  if (m == "EventGroup") {
    GVariantIter* iter = nullptr;
    g_variant_get(params, "(a(isvu))", &iter);
    std::vector<int32_t> clicks;
    GVariantBuilder missing;
    g_variant_builder_init(&missing, G_VARIANT_TYPE("ai"));
    gint32 id = 0;
    const gchar* event = nullptr;
    GVariant* data = nullptr;
    guint32 timestamp = 0;
    while (
        g_variant_iter_loop(iter, "(i&svu)", &id, &event, &data, &timestamp)) {
      if (!tray || !tray->nodes.count(id))
        g_variant_builder_add(&missing, "i", id);
      else if (event && strcmp(event, "clicked") == 0)
        clicks.push_back(id);
    }
    g_variant_iter_free(iter);
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(ai)", &missing));
    for (int32_t c : clicks)
      ClickMenuItem(tray_id, c);
    return;
  }
  if (m == "AboutToShow") {
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(b)", FALSE));
    return;
  }
  if (m == "AboutToShowGroup") {
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(@ai@ai)",
                      g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0),
                      g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
    return;
  }
  g_dbus_method_invocation_return_dbus_error(
      invocation, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

const GDBusInterfaceVTable kSniVtable = {
    SniMethodCall, SniGetProperty, nullptr, {}};
const GDBusInterfaceVTable kMenuVtable = {
    MenuMethodCall, MenuGetProperty, nullptr, {}};

void Emit(SniTray* tray, const char* path, const char* iface,
          const char* signal, GVariant* params) {
  if (GDBusConnection* bus = Bus())
    g_dbus_connection_emit_signal(bus, nullptr, path, iface, signal, params,
                                  nullptr);
  (void)tray;
}

void RegisterWithWatcher(SniTray* tray) {
  GDBusConnection* bus = Bus();
  if (!bus)
    return;
  // The object path, as libayatana-appindicator passes it: the watcher
  // takes the bus name from the call's sender.
  g_dbus_connection_call(bus, kWatcherName, kWatcherPath, kWatcherName,
                         "RegisterStatusNotifierItem",
                         g_variant_new("(s)", tray->item_path.c_str()), nullptr,
                         G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

void OnWatcherAppeared(GDBusConnection*, const gchar*, const gchar*, gpointer) {
  // A new (or restarted) host: register every item with it.
  for (auto& [id, tray] : Trays())
    RegisterWithWatcher(tray.get());
}

}  // namespace

bool SniTrayEnabledLinux() {
  const char* forced = getenv("LAUFEY_TRAY_BACKEND");
  if (forced && strcmp(forced, "appindicator") == 0)
    return false;
  return true;
}

bool SniOwnsTrayLinux(uint32_t tray_id) {
  return Trays().count(tray_id) != 0;
}

bool SniCreateTrayLinux(uint32_t tray_id) {
  GDBusConnection* bus = Bus();
  if (!bus)
    return false;
  if (!g_sni_info) {
    g_sni_info = g_dbus_node_info_new_for_xml(kSniXml, nullptr);
    g_menu_info = g_dbus_node_info_new_for_xml(kMenuXml, nullptr);
    if (!g_sni_info || !g_menu_info)
      return false;
  }
  auto tray = std::make_unique<SniTray>();
  tray->id = tray_id;
  tray->item_path = "/org/laufey/StatusNotifierItem/" + std::to_string(tray_id);
  tray->menu_path = "/org/laufey/Menu/" + std::to_string(tray_id);
  tray->nodes[0] = MenuNode{};
  GError* error = nullptr;
  tray->item_reg = g_dbus_connection_register_object(
      bus, tray->item_path.c_str(), g_sni_info->interfaces[0], &kSniVtable,
      GUINT_TO_POINTER(tray_id), nullptr, &error);
  if (error) {
    g_error_free(error);
    return false;
  }
  tray->menu_reg = g_dbus_connection_register_object(
      bus, tray->menu_path.c_str(), g_menu_info->interfaces[0], &kMenuVtable,
      GUINT_TO_POINTER(tray_id), nullptr, &error);
  if (error) {
    g_error_free(error);
    g_dbus_connection_unregister_object(bus, tray->item_reg);
    return false;
  }
  SniTray* raw = tray.get();
  Trays()[tray_id] = std::move(tray);
  if (!g_watch) {
    // Fires once now if the watcher is there (registering every item, this
    // one included), and again whenever a host restarts.
    g_watch = g_bus_watch_name_on_connection(
        bus, kWatcherName, G_BUS_NAME_WATCHER_FLAGS_NONE, OnWatcherAppeared,
        nullptr, nullptr, nullptr);
  } else {
    RegisterWithWatcher(raw);
  }
  return true;
}

void SniDestroyTrayLinux(uint32_t tray_id) {
  auto it = Trays().find(tray_id);
  if (it == Trays().end())
    return;
  if (GDBusConnection* bus = Bus()) {
    g_dbus_connection_unregister_object(bus, it->second->item_reg);
    g_dbus_connection_unregister_object(bus, it->second->menu_reg);
  }
  if (!it->second->icon_theme_path.empty() && !it->second->icon_name.empty()) {
    std::string file =
        it->second->icon_theme_path + "/" + it->second->icon_name + ".png";
    unlink(file.c_str());
  }
  Trays().erase(it);
}

void SniSetIconLinux(uint32_t tray_id, const void* png_bytes, size_t len) {
  auto it = Trays().find(tray_id);
  if (it == Trays().end())
    return;
  SniTray* tray = it->second.get();
  GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
  if (!loader)
    return;
  bool ok = gdk_pixbuf_loader_write(
                loader, static_cast<const guchar*>(png_bytes), len, nullptr) &&
            gdk_pixbuf_loader_close(loader, nullptr);
  GdkPixbuf* pixbuf = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
  if (pixbuf) {
    int w = gdk_pixbuf_get_width(pixbuf), h = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    int stride = gdk_pixbuf_get_rowstride(pixbuf);
    bool alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    const guchar* px = gdk_pixbuf_read_pixels(pixbuf);
    tray->icon_w = w;
    tray->icon_h = h;
    tray->icon_argb.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const guchar* p = px + y * stride + x * channels;
        guchar* o = &tray->icon_argb[((size_t)y * w + x) * 4];
        o[0] = alpha ? p[3] : 255;
        o[1] = p[0];
        o[2] = p[1];
        o[3] = p[2];
      }
    }
  }
  g_object_unref(loader);
  // Also as a file under a private directory: hosts that prefer a name over
  // a pixmap (and the observer in docs/e2e-testing.md) read IconThemePath +
  // IconName.
  std::string dir = std::string(g_get_user_runtime_dir()) + "/laufey-tray-" +
                    std::to_string(getpid());
  g_mkdir_with_parents(dir.c_str(), 0700);
  if (!tray->icon_name.empty())
    unlink((dir + "/" + tray->icon_name + ".png").c_str());
  static uint32_t serial = 0;
  tray->icon_theme_path = dir;
  tray->icon_name =
      "laufey-tray-" + std::to_string(tray_id) + "-" + std::to_string(++serial);
  g_file_set_contents((dir + "/" + tray->icon_name + ".png").c_str(),
                      static_cast<const gchar*>(png_bytes), (gssize)len,
                      nullptr);
  Emit(tray, tray->item_path.c_str(), "org.kde.StatusNotifierItem", "NewIcon",
       nullptr);
}

void SniSetTooltipLinux(uint32_t tray_id, const char* tooltip_or_null) {
  auto it = Trays().find(tray_id);
  if (it == Trays().end())
    return;
  SniTray* tray = it->second.get();
  tray->title = tooltip_or_null ? tooltip_or_null : "";
  Emit(tray, tray->item_path.c_str(), "org.kde.StatusNotifierItem", "NewTitle",
       nullptr);
  Emit(tray, tray->item_path.c_str(), "org.kde.StatusNotifierItem",
       "NewToolTip", nullptr);
}

void SniSetMenuLinux(uint32_t tray_id, const std::vector<MenuEntry>& entries,
                     laufey_menu_click_fn on_click, void* on_click_data) {
  auto it = Trays().find(tray_id);
  if (it == Trays().end())
    return;
  SniTray* tray = it->second.get();
  tray->menu_click_fn = on_click;
  tray->menu_click_data = on_click_data;
  tray->nodes.clear();
  tray->nodes[0] = MenuNode{};
  int32_t next = 1;
  AddNodes(*tray, 0, entries, &next);
  tray->revision++;
  Emit(tray, tray->menu_path.c_str(), "com.canonical.dbusmenu", "LayoutUpdated",
       g_variant_new("(ui)", tray->revision, 0));
}

void SniSetClickHandlerLinux(uint32_t tray_id, laufey_tray_click_fn handler,
                             void* user_data) {
  auto it = Trays().find(tray_id);
  if (it == Trays().end())
    return;
  it->second->click_fn = handler;
  it->second->click_data = user_data;
}

void SniSetDoubleClickHandlerLinux(uint32_t tray_id,
                                   laufey_tray_click_fn handler,
                                   void* user_data) {
  auto it = Trays().find(tray_id);
  if (it == Trays().end())
    return;
  it->second->dblclick_fn = handler;
  it->second->dblclick_data = user_data;
}

void SniSetEventHandlerLinux(uint32_t tray_id, laufey_tray_event_fn handler,
                             void* user_data) {
  auto it = Trays().find(tray_id);
  if (it == Trays().end())
    return;
  it->second->event_fn = handler;
  it->second->event_data = user_data;
}

}  // namespace laufey_common
