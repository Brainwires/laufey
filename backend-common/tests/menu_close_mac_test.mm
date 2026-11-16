// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS-only unit tests for EnsureCloseWindowItem (menu_mac.mm): the app
// menus get a Close Window item (Cmd+W, performClose:) unless they have
// one, or the app binds plain Cmd+W itself; it goes into File, else Window,
// else a new Window submenu. Menus only: no window, no app run.

#include "laufey_backend_common.h"

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

static NSMenu* Bar(NSArray<NSString*>* titles) {
  NSMenu* bar = [[NSMenu alloc] init];
  for (NSString* t in titles) {
    NSMenuItem* top = [[NSMenuItem alloc] initWithTitle:t
                                                 action:nil
                                          keyEquivalent:@""];
    NSMenu* sub = [[NSMenu alloc] initWithTitle:t];
    [sub addItemWithTitle:@"Something"
                   action:@selector(copy:)
            keyEquivalent:@""];
    [top setSubmenu:sub];
    [bar addItem:top];
  }
  return bar;
}

static NSMenuItem* FindClose(NSMenu* menu) {
  for (NSMenuItem* item in [menu itemArray]) {
    if ([item action] == @selector(performClose:))
      return item;
    if ([item submenu])
      if (NSMenuItem* found = FindClose([item submenu]))
        return found;
  }
  return nil;
}

int main() {
  @autoreleasepool {
    EXPECT(!EnsureCloseWindowItem(nil));

    // The default menus: app menu and Edit. A new Window submenu.
    NSMenu* bar = Bar(@[ @"", @"Edit" ]);
    EXPECT(EnsureCloseWindowItem(bar));
    NSMenuItem* close = FindClose(bar);
    EXPECT(close && [[close keyEquivalent] isEqualToString:@"w"]);
    EXPECT(([close keyEquivalentModifierMask] &
            NSEventModifierFlagDeviceIndependentFlagsMask) ==
           NSEventModifierFlagCommand);
    EXPECT(
        [[[bar itemArray] lastObject].submenu.title isEqualToString:@"Window"]);
    // Twice: no second item.
    EXPECT(!EnsureCloseWindowItem(bar));
    EXPECT([bar numberOfItems] == 3);

    // A File submenu takes it, at its end.
    bar = Bar(@[ @"", @"File", @"Edit", @"Window" ]);
    EXPECT(EnsureCloseWindowItem(bar));
    NSMenu* file = [[bar itemArray][1] submenu];
    EXPECT([[file itemArray] lastObject].action == @selector(performClose:));
    EXPECT([bar numberOfItems] == 4);

    // Else the Window submenu (denext's desktop.menu.window), at its top.
    bar = Bar(@[ @"", @"Edit", @"Window" ]);
    EXPECT(EnsureCloseWindowItem(bar));
    NSMenu* window = [[bar itemArray][2] submenu];
    EXPECT([window itemArray][0].action == @selector(performClose:));
    EXPECT([window itemArray][1].isSeparatorItem);
    EXPECT([bar numberOfItems] == 3);

    // The app's own close (a "close" role item) or its own plain Cmd+W:
    // nothing added.
    bar = Bar(@[ @"", @"File" ]);
    [[[bar itemArray][1] submenu] addItemWithTitle:@"Close Tab"
                                            action:@selector(performClose:)
                                     keyEquivalent:@""];
    EXPECT(!EnsureCloseWindowItem(bar));
    bar = Bar(@[ @"", @"File" ]);
    [[[bar itemArray][1] submenu] addItemWithTitle:@"Close Tab"
                                            action:@selector(copy:)
                                     keyEquivalent:@"w"];
    EXPECT(!EnsureCloseWindowItem(bar));
    EXPECT(!FindClose(bar));
    // Cmd+Shift+W is another shortcut: Close Window is still added.
    bar = Bar(@[ @"", @"File" ]);
    NSMenuItem* all =
        [[[bar itemArray][1] submenu] addItemWithTitle:@"Close All"
                                                action:@selector(copy:)
                                         keyEquivalent:@"w"];
    [all setKeyEquivalentModifierMask:NSEventModifierFlagCommand |
                                      NSEventModifierFlagShift];
    EXPECT(EnsureCloseWindowItem(bar));
  }
  std::printf("menu_close_mac_test: all passed\n");
  return 0;
}
