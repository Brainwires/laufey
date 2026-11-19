// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// menu_snapshot (API 59, LAUFEY_MENU_CAP_SNAPSHOT): the JSON every backend
// answers with. A backend walks its native menu objects (NSMenu, HMENU,
// GtkMenuShell, CefMenuModel), fills MenuSnapshotItem from what they hold
// now, and serializes them with MenuSnapshotJson. Header-only, no OS API.
// See laufey.h and docs/menus.md.

#ifndef LAUFEY_MENU_SNAPSHOT_H_
#define LAUFEY_MENU_SNAPSHOT_H_

#include <cstdio>
#include <string>
#include <vector>

namespace laufey_common {

struct MenuSnapshotItem {
  enum class Type { kItem, kSubmenu, kSeparator };
  Type type = Type::kItem;
  // The title the native item shows, without the platform's mnemonic marks
  // and without an accelerator the backend appended to it ("Open\tCtrl+O"
  // is "Open").
  std::string label;
  std::string id;    // a custom item's id; empty: left out
  std::string role;  // the role laufey built the item from; empty: left out
  // The key the native item is bound to, in the canonical accelerator form
  // (CanonicalAccelerator: "Shift+Super+K", "Ctrl+Shift+K"); empty: left out.
  std::string accelerator;
  bool enabled = true;
  bool checked = false;
  bool visible = true;
  std::vector<MenuSnapshotItem> children;  // kSubmenu
};

namespace menu_snapshot_internal {

inline void AppendQuoted(std::string* out, const std::string& s) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        *out += "\\\"";
        break;
      case '\\':
        *out += "\\\\";
        break;
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

inline void AppendItems(std::string* out,
                        const std::vector<MenuSnapshotItem>& items, int depth) {
  out->push_back('[');
  bool first = true;
  for (const MenuSnapshotItem& item : items) {
    if (!first)
      out->push_back(',');
    first = false;
    const char* type =
        item.type == MenuSnapshotItem::Type::kSeparator ? "separator"
        : item.type == MenuSnapshotItem::Type::kSubmenu ? "submenu"
                                                        : "item";
    *out += "{\"type\":\"";
    *out += type;
    *out += "\",\"label\":";
    AppendQuoted(out, item.label);
    if (!item.id.empty()) {
      *out += ",\"id\":";
      AppendQuoted(out, item.id);
    }
    if (!item.role.empty()) {
      *out += ",\"role\":";
      AppendQuoted(out, item.role);
    }
    *out += item.enabled ? ",\"enabled\":true" : ",\"enabled\":false";
    *out += item.checked ? ",\"checked\":true" : ",\"checked\":false";
    *out += item.visible ? ",\"visible\":true" : ",\"visible\":false";
    if (!item.accelerator.empty()) {
      *out += ",\"accelerator\":";
      AppendQuoted(out, item.accelerator);
    }
    if (item.type == MenuSnapshotItem::Type::kSubmenu) {
      *out += ",\"submenu\":";
      // A menu nested deeper than any real one (a cycle in a native menu
      // would otherwise never end) is cut off as empty.
      if (depth < 16)
        AppendItems(out, item.children, depth + 1);
      else
        *out += "[]";
    }
    out->push_back('}');
  }
  out->push_back(']');
}

}  // namespace menu_snapshot_internal

// The JSON array menu_snapshot returns ("[]" for a menu with no items).
inline std::string MenuSnapshotJson(
    const std::vector<MenuSnapshotItem>& items) {
  std::string out;
  menu_snapshot_internal::AppendItems(&out, items, 0);
  return out;
}

}  // namespace laufey_common

#endif  // LAUFEY_MENU_SNAPSHOT_H_
