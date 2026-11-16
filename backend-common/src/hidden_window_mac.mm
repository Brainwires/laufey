// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_hidden_window_mac.h"

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>

namespace laufey_common {

namespace {

// The associated object holding what MacHideInvisibly changed (its presence
// marks the window as hidden this way).
char kSavedKey;

NSString* const kAlpha = @"alpha";
NSString* const kIgnoresMouse = @"ignoresMouse";
NSString* const kShadow = @"shadow";
NSString* const kBehavior = @"behavior";
NSString* const kSharing = @"sharing";
NSString* const kExcludedFromMenu = @"excludedFromMenu";
NSString* const kAccessible = @"accessible";
NSString* const kKeyObserver = @"keyObserver";
NSString* const kMainObserver = @"mainObserver";

NSMutableDictionary* Saved(NSWindow* window) {
  return objc_getAssociatedObject(window, &kSavedKey);
}

// The collection behaviour of an invisible window: transient (not in
// Mission Control or Exposé, not moved between Spaces with the others) and
// out of the Cmd-` cycle; the other bits as they were.
NSWindowCollectionBehavior InvisibleBehavior(NSWindowCollectionBehavior b) {
  const NSWindowCollectionBehavior exclusive =
      NSWindowCollectionBehaviorManaged | NSWindowCollectionBehaviorTransient |
      NSWindowCollectionBehaviorStationary |
      NSWindowCollectionBehaviorParticipatesInCycle |
      NSWindowCollectionBehaviorIgnoresCycle;
  return (b & ~exclusive) | NSWindowCollectionBehaviorTransient |
         NSWindowCollectionBehaviorIgnoresCycle;
}

// Gives up key and main status without leaving the screen: out (AppKit
// hands them to the next window), then back in at the back of the window
// list, which takes neither.
void StepAside(NSWindow* window) {
  [window orderOut:nil];
  [window orderBack:nil];
}

// AppKit can still pick a window that is on screen as the next key or main
// window (when the key window closes, say): an invisible one steps aside at
// once, so the keyboard never goes to a window the user can't see.
id WatchFor(NSWindow* window, NSNotificationName name) {
  __weak NSWindow* weak = window;
  return [[NSNotificationCenter defaultCenter]
      addObserverForName:name
                  object:window
                   queue:[NSOperationQueue mainQueue]
              usingBlock:^(NSNotification*) {
                NSWindow* strong = weak;
                if (strong && Saved(strong))
                  StepAside(strong);
              }];
}

}  // namespace

bool MacHideInvisibly(void* raw) {
  NSWindow* window = (__bridge NSWindow*)raw;
  if (!window)
    return false;
  if (Saved(window))
    return true;
  if (window.styleMask & NSWindowStyleMaskFullScreen)
    return false;
  NSMutableDictionary* saved = [@{
    kAlpha : @(window.alphaValue),
    kIgnoresMouse : @(window.ignoresMouseEvents),
    kShadow : @(window.hasShadow),
    kBehavior : @(window.collectionBehavior),
    kSharing : @(window.sharingType),
    kExcludedFromMenu : @(window.excludedFromWindowsMenu),
    kAccessible : @(window.isAccessibilityElement),
    kKeyObserver : WatchFor(window, NSWindowDidBecomeKeyNotification),
    kMainObserver : WatchFor(window, NSWindowDidBecomeMainNotification),
  } mutableCopy];
  objc_setAssociatedObject(window, &kSavedKey, saved,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  window.alphaValue = 0;
  window.hasShadow = NO;
  window.ignoresMouseEvents = YES;
  window.collectionBehavior = InvisibleBehavior(window.collectionBehavior);
  window.sharingType = NSWindowSharingNone;
  window.excludedFromWindowsMenu = YES;
  [window setAccessibilityElement:NO];
  // On screen for WebKit, which keeps the page's process at its full rate;
  // never key or main.
  StepAside(window);
  return true;
}

namespace {

// Ends the hide's bookkeeping (the record and its observers); the record,
// or nil when the window wasn't hidden this way.
NSDictionary* TakeSaved(NSWindow* window) {
  NSDictionary* saved = window ? Saved(window) : nil;
  if (!saved)
    return nil;
  objc_setAssociatedObject(window, &kSavedKey, nil,
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
  [center removeObserver:saved[kKeyObserver]];
  [center removeObserver:saved[kMainObserver]];
  return saved;
}

}  // namespace

bool MacForgetInvisibleHide(void* raw) {
  return TakeSaved((__bridge NSWindow*)raw) != nil;
}

bool MacRestoreFromInvisibleHide(void* raw) {
  NSWindow* window = (__bridge NSWindow*)raw;
  NSDictionary* saved = TakeSaved(window);
  if (!saved)
    return false;
  window.collectionBehavior =
      (NSWindowCollectionBehavior)[saved[kBehavior] unsignedIntegerValue];
  window.sharingType = (NSWindowSharingType)[saved[kSharing] integerValue];
  window.excludedFromWindowsMenu = [saved[kExcludedFromMenu] boolValue];
  [window setAccessibilityElement:[saved[kAccessible] boolValue]];
  window.ignoresMouseEvents = [saved[kIgnoresMouse] boolValue];
  window.hasShadow = [saved[kShadow] boolValue];
  window.alphaValue = [saved[kAlpha] doubleValue];
  return true;
}

bool MacIsInvisiblyHidden(const void* raw) {
  NSWindow* window = (__bridge NSWindow*)raw;
  return window && Saved(window) != nil;
}

void MacSetWindowAlpha(void* raw, double alpha) {
  NSWindow* window = (__bridge NSWindow*)raw;
  if (!window)
    return;
  if (NSMutableDictionary* saved = Saved(window))
    saved[kAlpha] = @(alpha);
  else
    window.alphaValue = alpha;
}

double MacWindowAlpha(const void* raw) {
  NSWindow* window = (__bridge NSWindow*)raw;
  if (!window)
    return 1.0;
  NSDictionary* saved = Saved(window);
  return saved ? [saved[kAlpha] doubleValue] : window.alphaValue;
}

void MacSetIgnoresMouseEvents(void* raw, bool ignores) {
  NSWindow* window = (__bridge NSWindow*)raw;
  if (!window)
    return;
  if (NSMutableDictionary* saved = Saved(window))
    saved[kIgnoresMouse] = @(ignores);
  else
    window.ignoresMouseEvents = ignores;
}

bool MacIgnoresMouseEvents(const void* raw) {
  NSWindow* window = (__bridge NSWindow*)raw;
  if (!window)
    return false;
  NSDictionary* saved = Saved(window);
  return saved ? [saved[kIgnoresMouse] boolValue] : window.ignoresMouseEvents;
}

void MacSetCollectionBehavior(void* raw, unsigned long behavior) {
  NSWindow* window = (__bridge NSWindow*)raw;
  if (!window)
    return;
  NSWindowCollectionBehavior b = (NSWindowCollectionBehavior)behavior;
  if (NSMutableDictionary* saved = Saved(window)) {
    saved[kBehavior] = @(b);
    // Still out of Mission Control and the cycle while hidden.
    window.collectionBehavior = InvisibleBehavior(b);
  } else {
    window.collectionBehavior = b;
  }
}

unsigned long MacCollectionBehavior(const void* raw) {
  NSWindow* window = (__bridge NSWindow*)raw;
  if (!window)
    return 0;
  NSDictionary* saved = Saved(window);
  return saved ? [saved[kBehavior] unsignedLongValue]
               : (unsigned long)window.collectionBehavior;
}

bool MacWindowShown(const void* raw) {
  NSWindow* window = (__bridge NSWindow*)raw;
  return window && window.isVisible && !Saved(window);
}

bool MacHasVisibleWindowsForReopen(bool appkit_says) {
  if (!appkit_says)
    return false;
  bool invisible = false;
  for (NSWindow* window in NSApp.windows) {
    if (!window.isVisible)
      continue;
    if (Saved(window)) {
      invisible = true;
      continue;
    }
    // The menu bar's status items are windows of the app too.
    if ([NSStringFromClass([window class]) hasPrefix:@"NSStatusBar"])
      continue;
    return true;
  }
  return !invisible;
}

}  // namespace laufey_common
