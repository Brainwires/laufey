// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Overlay windows and the activation policy on macOS (API 50); see
// laufey_overlay.h.
//
// macOS has no input-shape API for a window. A window with an input region
// keeps ignoresMouseEvents on while the cursor is outside its rectangles and
// turns it off while the cursor is over one, so the click that follows lands
// in the window. The state is recomputed on every event that can change it,
// never polled: a cursor move (a global and a local mouse monitor; mouse
// moves need no permission), a new region, and the window moving, resizing,
// showing or hiding, or changing screen, so a click right after any of them
// meets the current state. The state lives in a weak-keyed map, so a window
// that is gone drops out by itself.
//
// What stays: the window server routes a click by the state it has when the
// click arrives, and a cursor move reaches this process only after the
// window server has seen it. A click posted within about 2 ms of the move
// that brought the cursor over a rectangle (a synthetic warp-and-click) can
// still go to the window beneath; a pointing device's click comes much later.

#include "laufey_overlay.h"

#include "laufey_launch_config.h"
#include "laufey_ui_tasks.h"

#import <AppKit/AppKit.h>

@interface LaufeyOverlayState : NSObject
@property(nonatomic) BOOL explicitPassthrough;
// The window's frame / visibility / screen observers while it has a region.
@property(nonatomic, strong) NSArray<id>* windowObservers;
@end

@implementation LaufeyOverlayState {
 @public
  laufey_common::InputRegion region;
}
- (void)dealloc {
  for (id observer in _windowObservers)
    [[NSNotificationCenter defaultCenter] removeObserver:observer];
}
@end

namespace laufey_common {

namespace {

NSMapTable<NSWindow*, LaufeyOverlayState*>* States() {
  static NSMapTable* table = [NSMapTable weakToStrongObjectsMapTable];
  return table;
}

LaufeyOverlayState* StateFor(NSWindow* window, bool create) {
  LaufeyOverlayState* st = [States() objectForKey:window];
  if (!st && create) {
    st = [[LaufeyOverlayState alloc] init];
    [States() setObject:st forKey:window];
  }
  return st;
}

id g_global_monitor = nil;
id g_local_monitor = nil;
// The app-wide observers (screens reconfigured, the active Space changed).
NSArray<id>* g_app_observers = nil;

void Recompute(NSWindow* window, LaufeyOverlayState* st) {
  bool pass = st.explicitPassthrough;
  if (!pass && st->region.active) {
    NSPoint p = [NSEvent mouseLocation];  // bottom-left screen space
    NSRect content = [window contentRectForFrameRect:[window frame]];
    bool inside = [window isVisible] && NSPointInRect(p, content);
    double x = p.x - content.origin.x;
    double y = content.origin.y + content.size.height - p.y;
    pass = ShouldPassThrough(false, st->region, inside, x, y);
  }
  if ((bool)[window ignoresMouseEvents] != pass)
    [window setIgnoresMouseEvents:pass];
}

void RecomputeAll() {
  for (NSWindow* window in [[States() keyEnumerator] allObjects]) {
    LaufeyOverlayState* st = [States() objectForKey:window];
    if (st && st->region.active)
      Recompute(window, st);
  }
}

bool AnyRegion() {
  for (LaufeyOverlayState* st in [[States() objectEnumerator] allObjects]) {
    if (st->region.active)
      return true;
  }
  return false;
}

// Recompute `window` whenever its frame, visibility or screen changes, while
// it has a region: a window that moves under a cursor at rest (a walking
// desktop companion) takes or passes the next click by where it is now.
void WatchWindow(NSWindow* window, LaufeyOverlayState* st) {
  bool want = st->region.active;
  if (want == (st.windowObservers != nil))
    return;
  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  if (!want) {
    for (id observer in st.windowObservers)
      [center removeObserver:observer];
    st.windowObservers = nil;
    return;
  }
  NSMutableArray<id>* observers = [NSMutableArray array];
  __weak NSWindow* weak_window = window;
  for (NSNotificationName name in @[
         NSWindowDidMoveNotification, NSWindowDidResizeNotification,
         NSWindowDidChangeOcclusionStateNotification,
         NSWindowDidChangeScreenNotification,
         NSWindowDidDeminiaturizeNotification
       ]) {
    [observers addObject:[center addObserverForName:name
                                             object:window
                                              queue:nil
                                         usingBlock:^(NSNotification*) {
                                           NSWindow* w = weak_window;
                                           LaufeyOverlayState* s =
                                               w ? StateFor(w, false) : nil;
                                           if (s && s->region.active)
                                             Recompute(w, s);
                                         }]];
  }
  st.windowObservers = observers;
}

void UpdateFollowers() {
  bool want = AnyRegion();
  NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
                     NSEventMaskRightMouseDragged |
                     NSEventMaskOtherMouseDragged;
  if (want && !g_global_monitor) {
    g_global_monitor =
        [NSEvent addGlobalMonitorForEventsMatchingMask:mask
                                               handler:^(NSEvent*) {
                                                 RecomputeAll();
                                               }];
    g_local_monitor =
        [NSEvent addLocalMonitorForEventsMatchingMask:mask
                                              handler:^NSEvent*(NSEvent* e) {
                                                RecomputeAll();
                                                return e;
                                              }];
    id screens = [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidChangeScreenParametersNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  RecomputeAll();
                }];
    id space = [[[NSWorkspace sharedWorkspace] notificationCenter]
        addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) {
                  RecomputeAll();
                }];
    g_app_observers = @[ screens, space ];
  } else if (!want && g_global_monitor) {
    [NSEvent removeMonitor:g_global_monitor];
    [NSEvent removeMonitor:g_local_monitor];
    [[NSNotificationCenter defaultCenter] removeObserver:g_app_observers[0]];
    [[[NSWorkspace sharedWorkspace] notificationCenter]
        removeObserver:g_app_observers[1]];
    g_global_monitor = nil;
    g_local_monitor = nil;
    g_app_observers = nil;
  }
}

NSWindowLevel LevelFor(int32_t level) {
  switch (level) {
    case LAUFEY_WINDOW_LEVEL_FLOATING:
      return NSFloatingWindowLevel;
    case LAUFEY_WINDOW_LEVEL_STATUS:
      return NSStatusWindowLevel;
    case LAUFEY_WINDOW_LEVEL_POP_UP_MENU:
      return NSPopUpMenuWindowLevel;
    case LAUFEY_WINDOW_LEVEL_SCREEN_SAVER:
      return NSScreenSaverWindowLevel;
    default:
      return NSNormalWindowLevel;
  }
}

NSApplicationActivationPolicy NativePolicy(int32_t policy) {
  switch (policy) {
    case LAUFEY_ACTIVATION_POLICY_ACCESSORY:
      return NSApplicationActivationPolicyAccessory;
    case LAUFEY_ACTIVATION_POLICY_PROHIBITED:
      return NSApplicationActivationPolicyProhibited;
    default:
      return NSApplicationActivationPolicyRegular;
  }
}

}  // namespace

void MacSetClickPassthrough(void* nswindow, bool enabled) {
  NSWindow* window = (__bridge NSWindow*)nswindow;
  if (!window)
    return;
  LaufeyOverlayState* st = StateFor(window, true);
  st.explicitPassthrough = enabled;
  Recompute(window, st);
}

bool MacIsClickPassthrough(void* nswindow) {
  NSWindow* window = (__bridge NSWindow*)nswindow;
  if (!window)
    return false;
  LaufeyOverlayState* st = StateFor(window, false);
  return st ? st.explicitPassthrough : (bool)[window ignoresMouseEvents];
}

void MacSetInputRegion(void* nswindow, const InputRegion& region) {
  NSWindow* window = (__bridge NSWindow*)nswindow;
  if (!window)
    return;
  LaufeyOverlayState* st = StateFor(window, true);
  st->region = region;
  Recompute(window, st);
  WatchWindow(window, st);
  UpdateFollowers();
}

void MacForgetOverlayWindow(void* nswindow) {
  NSWindow* window = (__bridge NSWindow*)nswindow;
  if (!window)
    return;
  if (LaufeyOverlayState* st = StateFor(window, false)) {
    st->region = InputRegion();
    WatchWindow(window, st);
  }
  [States() removeObjectForKey:window];
  UpdateFollowers();
}

bool MacSetVisibleOnAllWorkspaces(void* nswindow, bool visible,
                                  bool visible_on_full_screen) {
  NSWindow* window = (__bridge NSWindow*)nswindow;
  if (!window)
    return false;
  NSWindowCollectionBehavior b = [window collectionBehavior];
  b &= ~(NSWindowCollectionBehaviorCanJoinAllSpaces |
         NSWindowCollectionBehaviorMoveToActiveSpace |
         NSWindowCollectionBehaviorFullScreenAuxiliary |
         NSWindowCollectionBehaviorStationary |
         NSWindowCollectionBehaviorFullScreenPrimary);
  if (visible) {
    // Stationary: Exposé / Mission Control leave it where it is.
    b |= NSWindowCollectionBehaviorCanJoinAllSpaces |
         NSWindowCollectionBehaviorStationary;
  }
  if (visible_on_full_screen) {
    // Allowed on another app's full-screen Space; a window there must also
    // be above it (set_window_level STATUS or higher).
    b |= NSWindowCollectionBehaviorFullScreenAuxiliary;
  }
  [window setCollectionBehavior:b];
  return true;
}

bool MacSetWindowLevel(void* nswindow, int32_t level) {
  NSWindow* window = (__bridge NSWindow*)nswindow;
  if (!window || !IsValidWindowLevel(level))
    return false;
  [window setLevel:LevelFor(level)];
  return true;
}

bool MacSetActivationPolicy(int32_t policy) {
  if (policy < LAUFEY_ACTIVATION_POLICY_REGULAR ||
      policy > LAUFEY_ACTIVATION_POLICY_PROHIBITED)
    return false;
  NSApplicationActivationPolicy native = NativePolicy(policy);
  dispatch_async(dispatch_get_main_queue(), ^{
    bool was_regular =
        [NSApp activationPolicy] == NSApplicationActivationPolicyRegular;
    [NSApp setActivationPolicy:native];
    if (native == NSApplicationActivationPolicyRegular && !was_regular) {
      // The Dock icon and the menu bar appear for an active app; without
      // this the windows the app shows next stay behind the frontmost app.
      [NSApp activateIgnoringOtherApps:YES];
    }
  });
  return true;
}

int32_t MacGetActivationPolicy() {
  __block int32_t result = LAUFEY_ACTIVATION_POLICY_REGULAR;
  RunOnMainSync(^{
    switch ([NSApp activationPolicy]) {
      case NSApplicationActivationPolicyAccessory:
        result = LAUFEY_ACTIVATION_POLICY_ACCESSORY;
        break;
      case NSApplicationActivationPolicyProhibited:
        result = LAUFEY_ACTIVATION_POLICY_PROHIBITED;
        break;
      default:
        result = LAUFEY_ACTIVATION_POLICY_REGULAR;
    }
  });
  return result;
}

void MacApplyLaunchActivationPolicy() {
  id ui_element =
      [[NSBundle mainBundle] objectForInfoDictionaryKey:@"LSUIElement"];
  bool ls_ui_element = [ui_element respondsToSelector:@selector(boolValue)] &&
                       [ui_element boolValue];
  int32_t policy =
      LaunchActivationPolicy(LaunchActivationPolicyName(), ls_ui_element);
  [NSApp setActivationPolicy:NativePolicy(policy)];
}

}  // namespace laufey_common
