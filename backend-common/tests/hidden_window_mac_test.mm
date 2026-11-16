// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS-only unit tests for hidden_window_mac.mm: a window hidden by keeping
// it ordered in is invisible, passes clicks through, is out of cycling, the
// Window menu, screen sharing and accessibility, gives up key, still counts
// as on screen for AppKit (and so for WebKit), is not "shown" for laufey, and
// is restored exactly, with what was set on it while hidden; a full-screen
// window is left to orderOut. Needs a window server session (a macOS runner
// has one). Run via
// `ctest --test-dir webview/build`.

#include "laufey_hidden_window_mac.h"
#include "laufey_window.h"

#import <Cocoa/Cocoa.h>

#include <cstdio>
#include <cstdlib>

using namespace laufey_common;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static NSWindow* MakeWindow(NSRect frame) {
  NSWindow* window = [[NSWindow alloc]
      initWithContentRect:frame
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  window.releasedWhenClosed = NO;
  return window;
}

static void Spin() {
  [[NSRunLoop currentRunLoop]
      runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
}

int main() {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    // Nowhere near the middle of the screen, and tiny.
    NSWindow* window = MakeWindow(NSMakeRect(0, 0, 120, 80));
    NSWindow* other = MakeWindow(NSMakeRect(130, 0, 120, 80));
    window.alphaValue = 0.75;
    window.hasShadow = YES;
    window.ignoresMouseEvents = NO;
    window.collectionBehavior = NSWindowCollectionBehaviorManaged |
                                NSWindowCollectionBehaviorParticipatesInCycle |
                                NSWindowCollectionBehaviorFullScreenAuxiliary;
    window.sharingType = NSWindowSharingReadOnly;
    window.excludedFromWindowsMenu = NO;
    [window setAccessibilityElement:YES];
    [other orderFront:nil];
    [window makeKeyAndOrderFront:nil];
    Spin();

    EXPECT(!MacIsInvisiblyHidden((__bridge void*)window));
    EXPECT(MacWindowShown((__bridge void*)window));
    EXPECT(!MacRestoreFromInvisibleHide((__bridge void*)window));
    EXPECT(!MacHideInvisibly(nullptr));

    EXPECT(MacHideInvisibly((__bridge void*)window));
    Spin();
    EXPECT(MacIsInvisiblyHidden((__bridge void*)window));
    // On screen for AppKit (WebKit's test), not for laufey.
    EXPECT(window.isVisible);
    EXPECT(!MacWindowShown((__bridge void*)window));
    EXPECT(window.alphaValue == 0);
    EXPECT(!window.hasShadow);
    EXPECT(window.ignoresMouseEvents);
    EXPECT(window.collectionBehavior & NSWindowCollectionBehaviorTransient);
    EXPECT(window.collectionBehavior & NSWindowCollectionBehaviorIgnoresCycle);
    EXPECT(!(window.collectionBehavior & NSWindowCollectionBehaviorManaged));
    EXPECT(!(window.collectionBehavior &
             NSWindowCollectionBehaviorParticipatesInCycle));
    // Bits outside the exclusive groups are kept.
    EXPECT(window.collectionBehavior &
           NSWindowCollectionBehaviorFullScreenAuxiliary);
    EXPECT(window.sharingType == NSWindowSharingNone);
    EXPECT(window.excludedFromWindowsMenu);
    EXPECT(!window.isAccessibilityElement);
    EXPECT(!window.isKeyWindow);
    EXPECT(NSApp.keyWindow != window);
    // Behind the other window.
    NSArray<NSWindow*>* order = NSApp.orderedWindows;
    EXPECT([order indexOfObject:window] > [order indexOfObject:other]);
    // Hiding it again changes nothing.
    EXPECT(MacHideInvisibly((__bridge void*)window));
    EXPECT(window.alphaValue == 0);

    EXPECT(MacRestoreFromInvisibleHide((__bridge void*)window));
    EXPECT(!MacIsInvisiblyHidden((__bridge void*)window));
    EXPECT(MacWindowShown((__bridge void*)window));
    EXPECT(window.alphaValue == 0.75);
    EXPECT(window.hasShadow);
    EXPECT(!window.ignoresMouseEvents);
    EXPECT(window.collectionBehavior ==
           (NSWindowCollectionBehaviorManaged |
            NSWindowCollectionBehaviorParticipatesInCycle |
            NSWindowCollectionBehaviorFullScreenAuxiliary));
    EXPECT(window.sharingType == NSWindowSharingReadOnly);
    EXPECT(!window.excludedFromWindowsMenu);
    EXPECT(window.isAccessibilityElement);
    EXPECT(!MacRestoreFromInvisibleHide((__bridge void*)window));

    // Opacity, click passthrough and the collection behaviour set while
    // hidden: the window stays invisible, ignoring clicks and out of
    // Mission Control, the getters read the new values, and a show applies
    // them rather than the ones from before the hide.
    EXPECT(MacHideInvisibly((__bridge void*)window));
    MacSetWindowAlpha((__bridge void*)window, 0.5);
    MacSetIgnoresMouseEvents((__bridge void*)window, true);
    MacSetCollectionBehavior((__bridge void*)window,
                             NSWindowCollectionBehaviorManaged |
                                 NSWindowCollectionBehaviorCanJoinAllSpaces);
    EXPECT(window.alphaValue == 0);
    EXPECT(window.ignoresMouseEvents);
    EXPECT(window.collectionBehavior & NSWindowCollectionBehaviorTransient);
    EXPECT(window.collectionBehavior &
           NSWindowCollectionBehaviorCanJoinAllSpaces);
    EXPECT(!(window.collectionBehavior & NSWindowCollectionBehaviorManaged));
    EXPECT(MacWindowAlpha((__bridge void*)window) == 0.5);
    EXPECT(MacIgnoresMouseEvents((__bridge void*)window));
    EXPECT(MacCollectionBehavior((__bridge void*)window) ==
           (NSWindowCollectionBehaviorManaged |
            NSWindowCollectionBehaviorCanJoinAllSpaces));
    MacSetIgnoresMouseEvents((__bridge void*)window, false);
    EXPECT(window.ignoresMouseEvents);
    EXPECT(MacRestoreFromInvisibleHide((__bridge void*)window));
    EXPECT(window.alphaValue == 0.5);
    EXPECT(!window.ignoresMouseEvents);
    EXPECT(window.collectionBehavior ==
           (NSWindowCollectionBehaviorManaged |
            NSWindowCollectionBehaviorCanJoinAllSpaces));
    // On a shown window they are the window's own properties.
    MacSetWindowAlpha((__bridge void*)window, 0.75);
    EXPECT(window.alphaValue == 0.75);
    EXPECT(MacWindowAlpha((__bridge void*)window) == 0.75);
    MacSetCollectionBehavior((__bridge void*)window,
                             NSWindowCollectionBehaviorManaged);
    EXPECT(window.collectionBehavior == NSWindowCollectionBehaviorManaged);

    // Forgetting a hide (before a close) restores nothing.
    EXPECT(MacHideInvisibly((__bridge void*)window));
    EXPECT(MacForgetInvisibleHide((__bridge void*)window));
    EXPECT(!MacIsInvisiblyHidden((__bridge void*)window));
    EXPECT(window.alphaValue == 0);
    EXPECT(!MacForgetInvisibleHide((__bridge void*)window));
    [window orderOut:nil];
    window.alphaValue = 1;

    // A window that was never on screen (created hidden) is ordered in.
    NSWindow* fresh = MakeWindow(NSMakeRect(260, 0, 120, 80));
    EXPECT(!fresh.isVisible);
    EXPECT(!MacWindowShown((__bridge void*)fresh));
    EXPECT(MacHideInvisibly((__bridge void*)fresh));
    Spin();
    EXPECT(fresh.isVisible);
    EXPECT(!MacWindowShown((__bridge void*)fresh));
    EXPECT(MacRestoreFromInvisibleHide((__bridge void*)fresh));
    EXPECT(fresh.alphaValue == 1);

    // Full screen and minimize are refused while hidden this way (an empty
    // Space, a Dock tile that opens to nothing): it stays hidden.
    EXPECT(MacHideInvisibly((__bridge void*)fresh));
    MacSetWindowState((__bridge void*)fresh,
                      LAUFEY_WINDOW_ACTION_ENTER_FULLSCREEN);
    MacSetWindowState((__bridge void*)fresh, LAUFEY_WINDOW_ACTION_MINIMIZE);
    for (int i = 0; i < 10; ++i)
      Spin();
    EXPECT(!(fresh.styleMask & NSWindowStyleMaskFullScreen));
    EXPECT(!fresh.isMiniaturized);
    EXPECT(MacIsInvisiblyHidden((__bridge void*)fresh));
    EXPECT(fresh.alphaValue == 0);
    EXPECT(MacRestoreFromInvisibleHide((__bridge void*)fresh));

    // The Dock's reopen: with only invisible windows on screen there is
    // nothing visible; one shown window is enough.
    EXPECT(!MacHasVisibleWindowsForReopen(false));
    [other orderOut:nil];
    [window orderOut:nil];
    EXPECT(MacHideInvisibly((__bridge void*)fresh));
    Spin();
    EXPECT(!MacHasVisibleWindowsForReopen(true));
    [other orderFront:nil];
    Spin();
    EXPECT(MacHasVisibleWindowsForReopen(true));
    MacRestoreFromInvisibleHide((__bridge void*)fresh);

    [window close];
    [other close];
    [fresh close];
  }
  std::printf("hidden_window_mac_test: all passed\n");
  return 0;
}
