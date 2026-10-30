// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// NSStatusItem-backed tray / status-bar icon. One NSStatusItem per
// tray_id. Right-click pops the attached NSMenu; left-click /
// double-click fire the registered handlers. Light vs dark icons are
// resolved against [NSApp effectiveAppearance], and a distributed
// notification observer re-applies them on theme changes.

#include "laufey_backend_common.h"
#include "laufey_ui_tasks.h"

#import <AppKit/AppKit.h>

#include <atomic>
#include <map>

namespace laufey_common {

namespace {

struct TrayEntry {
  NSStatusItem* item;
  NSMenu* menu;
  laufey_menu_click_fn menu_click_fn;
  void* menu_click_data;
  laufey_tray_click_fn click_fn;
  void* click_data;
  laufey_tray_click_fn dblclick_fn;
  void* dblclick_data;
  // API 50: every event with its detail.
  laufey_tray_event_fn event_fn;
  void* event_data;
  NSTrackingArea* hover_area;
  NSImage* light_image;
  NSImage* dark_image;
};

std::map<uint32_t, TrayEntry>& TrayMap() {
  static std::map<uint32_t, TrayEntry> map;
  return map;
}

std::atomic<uint32_t> g_next_tray_id{1};

}  // namespace
}  // namespace laufey_common

@interface LaufeyCommonTrayTarget : NSObject
+ (instancetype)shared;
- (void)trayClicked:(id)sender;
@end

@implementation LaufeyCommonTrayTarget
+ (instancetype)shared {
  static LaufeyCommonTrayTarget* instance = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    instance = [[LaufeyCommonTrayTarget alloc] init];
  });
  return instance;
}

// Reports a tray event with the current modifiers and cursor (top-left
// screen space, the get_window_position convention).
static void LaufeyEmitTrayEvent(uint32_t tray_id, int32_t kind,
                                int32_t button, NSEventModifierFlags flags) {
  auto& map = laufey_common::TrayMap();
  auto it = map.find(tray_id);
  if (it == map.end() || !it->second.event_fn) return;
  laufey_tray_event_t e = {};
  e.tray_id = tray_id;
  e.kind = kind;
  e.button = button;
  if (flags & NSEventModifierFlagShift) e.modifiers |= LAUFEY_MOD_SHIFT;
  if (flags & NSEventModifierFlagControl) e.modifiers |= LAUFEY_MOD_CONTROL;
  if (flags & NSEventModifierFlagOption) e.modifiers |= LAUFEY_MOD_ALT;
  if (flags & NSEventModifierFlagCommand) e.modifiers |= LAUFEY_MOD_META;
  NSPoint p = [NSEvent mouseLocation];
  NSArray<NSScreen*>* screens = [NSScreen screens];
  CGFloat primary_height =
      screens.count > 0 ? [screens[0] frame].size.height : 0;
  e.x = (int32_t)p.x;
  e.y = (int32_t)(primary_height - p.y);
  e.has_position = true;
  it->second.event_fn(it->second.event_data, &e);
}

- (void)mouseEntered:(NSEvent*)event {
  NSNumber* tray =
      [(NSDictionary*)[[event trackingArea] userInfo] objectForKey:@"tray"];
  if (tray)
    LaufeyEmitTrayEvent([tray unsignedIntValue], LAUFEY_TRAY_EVENT_MOUSE_ENTER,
                        LAUFEY_MOUSE_BUTTON_LEFT, [NSEvent modifierFlags]);
}

- (void)mouseExited:(NSEvent*)event {
  NSNumber* tray =
      [(NSDictionary*)[[event trackingArea] userInfo] objectForKey:@"tray"];
  if (tray)
    LaufeyEmitTrayEvent([tray unsignedIntValue], LAUFEY_TRAY_EVENT_MOUSE_LEAVE,
                        LAUFEY_MOUSE_BUTTON_LEFT, [NSEvent modifierFlags]);
}

- (void)trayClicked:(id)sender {
  NSStatusBarButton* button = (NSStatusBarButton*)sender;
  NSNumber* tagObj = [[button cell] representedObject];
  if (!tagObj) return;
  uint32_t tray_id = (uint32_t)[tagObj unsignedIntValue];
  auto& map = laufey_common::TrayMap();
  auto it = map.find(tray_id);
  if (it == map.end()) return;
  NSEvent* event = [NSApp currentEvent];
  if (event && it->second.event_fn) {
    int32_t kind = LAUFEY_TRAY_EVENT_CLICK;
    int32_t mouse_button = LAUFEY_MOUSE_BUTTON_LEFT;
    if (event.type == NSEventTypeRightMouseUp) {
      kind = LAUFEY_TRAY_EVENT_RIGHT_CLICK;
      mouse_button = LAUFEY_MOUSE_BUTTON_RIGHT;
    } else if (event.type == NSEventTypeOtherMouseUp) {
      kind = LAUFEY_TRAY_EVENT_MIDDLE_CLICK;
      mouse_button = LAUFEY_MOUSE_BUTTON_MIDDLE;
    } else if (event.clickCount >= 2) {
      kind = LAUFEY_TRAY_EVENT_DOUBLE_CLICK;
    }
    LaufeyEmitTrayEvent(tray_id, kind, mouse_button, event.modifierFlags);
    // The handler may have destroyed the tray.
    it = map.find(tray_id);
    if (it == map.end()) return;
  }
  if (event && event.type == NSEventTypeOtherMouseUp) return;
  if (event && event.type == NSEventTypeRightMouseUp && it->second.menu) {
    [it->second.item popUpStatusItemMenu:it->second.menu];
    return;
  }
  if (event && event.clickCount >= 2 && it->second.dblclick_fn) {
    it->second.dblclick_fn(it->second.dblclick_data, tray_id);
    return;
  }
  if (it->second.click_fn) {
    it->second.click_fn(it->second.click_data, tray_id);
  }
}
@end

namespace laufey_common {

namespace {

bool SystemIsDarkMode() {
  if (@available(macOS 10.14, *)) {
    NSAppearance* appearance = [NSApp effectiveAppearance];
    NSAppearanceName match = [appearance bestMatchFromAppearancesWithNames:@[
      NSAppearanceNameAqua, NSAppearanceNameDarkAqua
    ]];
    return [match isEqualToString:NSAppearanceNameDarkAqua];
  }
  return false;
}

void ApplyActiveIconForTray(TrayEntry& entry) {
  if (!entry.item) return;
  bool dark = SystemIsDarkMode();
  NSImage* chosen =
      (dark && entry.dark_image) ? entry.dark_image : entry.light_image;
  if (chosen) [[entry.item button] setImage:chosen];
}

NSImage* ImageFromPng(const void* bytes, size_t len) {
  if (!bytes || len == 0) return nil;
  NSData* data = [NSData dataWithBytes:bytes length:len];
  NSImage* image = [[NSImage alloc] initWithData:data];
  if (!image) return nil;
  [image setSize:NSMakeSize(18, 18)];
  // Treating the supplied PNG as a template gives the best menu-bar
  // appearance in most cases. Users who want a full-color icon should
  // simply not mark it as template — but our API doesn't expose that yet,
  // so default to template for v1 (matches Electron's behavior).
  [image setTemplate:YES];
  return image;
}

// One-shot installer for a distributed-notification observer that fires
// on system theme changes (light <-> dark). Re-applies every tray icon.
void EnsureTrayAppearanceObserver() {
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    [[NSDistributedNotificationCenter defaultCenter]
        addObserverForName:@"AppleInterfaceThemeChangedNotification"
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* /*n*/) {
                  for (auto& [tid, entry] : TrayMap()) {
                    ApplyActiveIconForTray(entry);
                  }
                }];
  });
}

}  // namespace

uint32_t CreateTrayIconMac() {
  uint32_t tray_id = g_next_tray_id.fetch_add(1, std::memory_order_relaxed);
  dispatch_async(dispatch_get_main_queue(), ^{
    NSStatusItem* item = [[NSStatusBar systemStatusBar]
        statusItemWithLength:NSSquareStatusItemLength];
    if (!item) return;
    NSStatusBarButton* button = [item button];
    if (button) {
      [[button cell] setRepresentedObject:@(tray_id)];
      [button setTarget:[LaufeyCommonTrayTarget shared]];
      [button setAction:@selector(trayClicked:)];
      [button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp |
                            NSEventMaskOtherMouseUp];
    }
    TrayEntry entry = {};
    entry.item = item;
    TrayMap()[tray_id] = entry;
  });
  return tray_id;
}

void DestroyTrayIconMac(uint32_t tray_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end()) return;
    if (it->second.item) {
      [[NSStatusBar systemStatusBar] removeStatusItem:it->second.item];
    }
    map.erase(it);
  });
}

void SetTrayIconMac(uint32_t tray_id, const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0) return;
  NSData* data = [NSData dataWithBytes:png_bytes length:len];
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item) return;
    NSImage* image = ImageFromPng([data bytes], [data length]);
    if (!image) return;
    it->second.light_image = image;
    EnsureTrayAppearanceObserver();
    ApplyActiveIconForTray(it->second);
  });
}

void SetTrayIconDarkMac(uint32_t tray_id, const void* png_bytes, size_t len) {
  NSData* data = (png_bytes && len > 0)
                     ? [NSData dataWithBytes:png_bytes length:len]
                     : nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item) return;
    it->second.dark_image =
        data ? ImageFromPng([data bytes], [data length]) : nil;
    EnsureTrayAppearanceObserver();
    ApplyActiveIconForTray(it->second);
  });
}

void SetTrayTooltipMac(uint32_t tray_id, const char* tooltip_or_null) {
  NSString* tip = (tooltip_or_null && *tooltip_or_null)
                      ? [NSString stringWithUTF8String:tooltip_or_null]
                      : nil;
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item) return;
    [[it->second.item button] setToolTip:tip];
  });
}

void SetTrayMenuMac(uint32_t tray_id, laufey_value_t* menu_template,
                     const laufey_backend_api_t* api,
                     laufey_menu_click_fn on_click, void* on_click_data) {
  if (!menu_template) {
    dispatch_async(dispatch_get_main_queue(), ^{
      auto& map = TrayMap();
      auto it = map.find(tray_id);
      if (it == map.end()) return;
      it->second.menu = nil;
      it->second.menu_click_fn = nullptr;
      it->second.menu_click_data = nullptr;
    });
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
    // tray_id passed as window_id so the shared click dispatcher routes
    // it back through on_click with the right tray identifier.
    NSMenu* menu =
        BuildNSMenuFromValue(menu_template, api, on_click, on_click_data,
                             tray_id);
    if (!menu) return;
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end()) return;
    it->second.menu = menu;
    it->second.menu_click_fn = on_click;
    it->second.menu_click_data = on_click_data;
  });
}

void SetTrayClickHandlerMac(uint32_t tray_id, laufey_tray_click_fn handler,
                             void* user_data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end()) return;
    it->second.click_fn = handler;
    it->second.click_data = user_data;
  });
}

void SetTrayDoubleClickHandlerMac(uint32_t tray_id,
                                   laufey_tray_click_fn handler,
                                   void* user_data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end()) return;
    it->second.dblclick_fn = handler;
    it->second.dblclick_data = user_data;
  });
}

bool GetTrayIconBoundsMac(uint32_t tray_id, int* x, int* y, int* width,
                          int* height) {
  // Must touch AppKit on the main thread. Tray clicks (the usual caller)
  // already arrive on the main thread, so run synchronously there.
  __block bool ok = false;
  __block NSRect rect = NSZeroRect;
  void (^work)(void) = ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item) return;
    NSStatusBarButton* button = [it->second.item button];
    NSWindow* window = [button window];
    if (!button || !window) return;
    // Button bounds → screen rect (AppKit: bottom-left origin, points).
    rect = [window convertRectToScreen:[button convertRect:[button bounds]
                                                    toView:nil]];
    ok = true;
  };
  RunOnMainSync(work);
  if (!ok) return false;
  // Convert to top-left origin to match get_window_position. The menu bar
  // lives on the primary screen, whose frame origin is (0,0) bottom-left.
  CGFloat primary_height = 0;
  NSArray<NSScreen*>* screens = [NSScreen screens];
  if (screens.count > 0) primary_height = [screens[0] frame].size.height;
  if (x) *x = (int)rect.origin.x;
  if (y) *y = (int)(primary_height - (rect.origin.y + rect.size.height));
  if (width) *width = (int)rect.size.width;
  if (height) *height = (int)rect.size.height;
  return true;
}

void SetTrayEventHandlerMac(uint32_t tray_id, laufey_tray_event_fn handler,
                            void* user_data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end()) return;
    it->second.event_fn = handler;
    it->second.event_data = user_data;
    NSStatusBarButton* button = [it->second.item button];
    if (!button) return;
    if (handler && !it->second.hover_area) {
      NSTrackingArea* area = [[NSTrackingArea alloc]
          initWithRect:NSZeroRect
               options:NSTrackingMouseEnteredAndExited |
                       NSTrackingActiveAlways | NSTrackingInVisibleRect
                 owner:[LaufeyCommonTrayTarget shared]
              userInfo:@{@"tray" : @(tray_id)}];
      [button addTrackingArea:area];
      it->second.hover_area = area;
    } else if (!handler && it->second.hover_area) {
      [button removeTrackingArea:it->second.hover_area];
      it->second.hover_area = nil;
    }
  });
}

uint32_t TrayCapabilitiesMac() {
  return LAUFEY_TRAY_CAP_CLICK | LAUFEY_TRAY_CAP_DOUBLE_CLICK |
         LAUFEY_TRAY_CAP_RIGHT_CLICK | LAUFEY_TRAY_CAP_MIDDLE_CLICK |
         LAUFEY_TRAY_CAP_HOVER | LAUFEY_TRAY_CAP_MODIFIERS |
         LAUFEY_TRAY_CAP_POSITION | LAUFEY_TRAY_CAP_POP_UP_MENU;
}

bool PopUpTrayMenuMac(uint32_t tray_id) {
  __block bool has_menu = false;
  RunOnMainSync(^{
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    has_menu = it != map.end() && it->second.item && it->second.menu;
  });
  if (!has_menu) return false;
  // The menu runs a modal tracking loop: open it from the run loop, not
  // inside a main-queue block (see RunFromMainRunLoopMac).
  RunFromMainRunLoopMac([tray_id] {
    auto& map = TrayMap();
    auto it = map.find(tray_id);
    if (it == map.end() || !it->second.item || !it->second.menu) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [it->second.item popUpStatusItemMenu:it->second.menu];
#pragma clang diagnostic pop
  });
  return true;
}

}  // namespace laufey_common
