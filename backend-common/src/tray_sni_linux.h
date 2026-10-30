// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Internal to backend-common: the StatusNotifierItem tray (tray_sni_linux.cc),
// driven by tray_linux.cc on the GTK main thread. Not a public header.

#ifndef LAUFEY_BACKEND_COMMON_TRAY_SNI_LINUX_H_
#define LAUFEY_BACKEND_COMMON_TRAY_SNI_LINUX_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "laufey.h"
#include "laufey_menu.h"

namespace laufey_common {

// Whether new trays take the StatusNotifierItem path when a watcher is
// there (not with LAUFEY_TRAY_BACKEND=appindicator). Any thread.
bool SniTrayEnabledLinux();

// GTK main thread.
bool SniOwnsTrayLinux(uint32_t tray_id);
bool SniCreateTrayLinux(uint32_t tray_id);
void SniDestroyTrayLinux(uint32_t tray_id);
void SniSetIconLinux(uint32_t tray_id, const void* png_bytes, size_t len);
void SniSetTooltipLinux(uint32_t tray_id, const char* tooltip_or_null);
void SniSetMenuLinux(uint32_t tray_id, const std::vector<MenuEntry>& entries,
                     laufey_menu_click_fn on_click, void* on_click_data);
void SniSetClickHandlerLinux(uint32_t tray_id, laufey_tray_click_fn handler,
                             void* user_data);
void SniSetDoubleClickHandlerLinux(uint32_t tray_id,
                                   laufey_tray_click_fn handler,
                                   void* user_data);
void SniSetEventHandlerLinux(uint32_t tray_id, laufey_tray_event_fn handler,
                             void* user_data);

}  // namespace laufey_common

#endif  // LAUFEY_BACKEND_COMMON_TRAY_SNI_LINUX_H_
