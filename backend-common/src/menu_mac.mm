// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// NSMenu construction from a laufey_value_t menu template. Each non-role
// menu item carries its own click handler+data in an attached
// LaufeyCommonMenuItem object (via NSMenuItem.representedObject), so the
// same builder serves the application menu, context menu, dock menu,
// and tray menus without per-context global state.

#include "laufey_backend_common.h"
#include "laufey_menu.h"
#include "laufey_menu_snapshot.h"
#include "laufey_ui_tasks.h"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace laufey_common {
namespace {
// Item clicks so far (main thread): how the accelerator test hook tells an
// item that ran from a match on a disabled one, for which
// -performKeyEquivalent: answers YES as well.
uint64_t g_menu_clicks = 0;
}  // namespace
void NoteMenuItemClickedMac() {
  g_menu_clicks++;
}
}  // namespace laufey_common

@interface LaufeyCommonMenuItem : NSObject
@property(nonatomic, copy) NSString* itemId;
@property(nonatomic, assign) laufey_menu_click_fn clickFn;
@property(nonatomic, assign) void* clickData;
@property(nonatomic, assign) uint32_t windowId;
@end

@implementation LaufeyCommonMenuItem
@end

@interface LaufeyCommonMenuTarget : NSObject
+ (instancetype)shared;
- (void)menuItemClicked:(id)sender;
@end

@implementation LaufeyCommonMenuTarget
+ (instancetype)shared {
  static LaufeyCommonMenuTarget* instance = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    instance = [[LaufeyCommonMenuTarget alloc] init];
  });
  return instance;
}

- (void)menuItemClicked:(id)sender {
  laufey_common::NoteMenuItemClickedMac();
  NSMenuItem* item = (NSMenuItem*)sender;
  id rep = [item representedObject];
  if (![rep isKindOfClass:[LaufeyCommonMenuItem class]])
    return;
  LaufeyCommonMenuItem* w = (LaufeyCommonMenuItem*)rep;
  if (!w.clickFn || !w.itemId)
    return;
  w.clickFn(w.clickData, w.windowId, [w.itemId UTF8String]);
}
@end

namespace laufey_common {

namespace {

// The NSMenuItem key equivalent and modifier mask for an accelerator. False
// for a key a key equivalent can't name (media and volume keys).
bool KeyEquivalentFor(const Accelerator& a, NSString** outKey,
                      NSEventModifierFlags* outMask) {
  NSEventModifierFlags mask = 0;
  if (a.mods & kModSuper)
    mask |= NSEventModifierFlagCommand;
  if (a.mods & kModCtrl)
    mask |= NSEventModifierFlagControl;
  if (a.mods & kModAlt)
    mask |= NSEventModifierFlagOption;
  if (a.mods & kModShift)
    mask |= NSEventModifierFlagShift;
  unichar c = 0;
  switch (a.kind) {
    case KeyKind::kLetter:
      c = static_cast<unichar>(a.ch - 'A' + 'a');
      break;
    case KeyKind::kDigit:
    case KeyKind::kPunct:
      c = static_cast<unichar>(a.ch);
      break;
    case KeyKind::kFunction:
      c = static_cast<unichar>(NSF1FunctionKey + (a.number - 1));
      break;
    case KeyKind::kNumpad:
      mask |= NSEventModifierFlagNumericPad;
      switch (a.named) {
        case NamedKey::kNone:
          c = static_cast<unichar>('0' + a.number);
          break;
        case NamedKey::kNumDecimal:
          c = '.';
          break;
        case NamedKey::kNumAdd:
          c = '+';
          break;
        case NamedKey::kNumSubtract:
          c = '-';
          break;
        case NamedKey::kNumMultiply:
          c = '*';
          break;
        case NamedKey::kNumDivide:
          c = '/';
          break;
        default:
          return false;
      }
      break;
    case KeyKind::kNamed:
      switch (a.named) {
        case NamedKey::kSpace:
          c = ' ';
          break;
        case NamedKey::kTab:
          c = '\t';
          break;
        case NamedKey::kBackspace:
          c = NSBackspaceCharacter;
          break;
        case NamedKey::kDelete:
          c = NSDeleteFunctionKey;
          break;
        case NamedKey::kInsert:
          c = NSInsertFunctionKey;
          break;
        case NamedKey::kEnter:
          c = '\r';
          break;
        case NamedKey::kEscape:
          c = 0x1b;
          break;
        case NamedKey::kUp:
          c = NSUpArrowFunctionKey;
          break;
        case NamedKey::kDown:
          c = NSDownArrowFunctionKey;
          break;
        case NamedKey::kLeft:
          c = NSLeftArrowFunctionKey;
          break;
        case NamedKey::kRight:
          c = NSRightArrowFunctionKey;
          break;
        case NamedKey::kHome:
          c = NSHomeFunctionKey;
          break;
        case NamedKey::kEnd:
          c = NSEndFunctionKey;
          break;
        case NamedKey::kPageUp:
          c = NSPageUpFunctionKey;
          break;
        case NamedKey::kPageDown:
          c = NSPageDownFunctionKey;
          break;
        case NamedKey::kPrintScreen:
          c = NSPrintScreenFunctionKey;
          break;
        default:
          return false;
      }
      break;
  }
  if (!c)
    return false;
  *outKey = [NSString stringWithCharacters:&c length:1];
  *outMask = mask;
  return true;
}

NSMenuItem* CreateRoleMenuItem(const MenuEntry& entry) {
  const std::string& role = entry.role;
  NSString* title = @"";
  SEL action = nil;
  NSString* keyEquiv = @"";
  NSEventModifierFlags mask = NSEventModifierFlagCommand;

  if (RoleIs(role, "quit")) {
    title = @"Quit";
    action = @selector(terminate:);
    keyEquiv = @"q";
  } else if (RoleIs(role, "copy")) {
    title = @"Copy";
    action = @selector(copy:);
    keyEquiv = @"c";
  } else if (RoleIs(role, "paste")) {
    title = @"Paste";
    action = @selector(paste:);
    keyEquiv = @"v";
  } else if (RoleIs(role, "cut")) {
    title = @"Cut";
    action = @selector(cut:);
    keyEquiv = @"x";
  } else if (RoleIs(role, "selectall")) {
    title = @"Select All";
    action = @selector(selectAll:);
    keyEquiv = @"a";
  } else if (RoleIs(role, "undo")) {
    title = @"Undo";
    action = @selector(undo:);
    keyEquiv = @"z";
  } else if (RoleIs(role, "redo")) {
    title = @"Redo";
    action = @selector(redo:);
    keyEquiv = @"Z";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
  } else if (RoleIs(role, "minimize")) {
    title = @"Minimize";
    action = @selector(performMiniaturize:);
    keyEquiv = @"m";
  } else if (RoleIs(role, "zoom")) {
    title = @"Zoom";
    action = @selector(performZoom:);
  } else if (RoleIs(role, "close")) {
    title = @"Close";
    action = @selector(performClose:);
    keyEquiv = @"w";
  } else if (RoleIs(role, "about")) {
    title = @"About";
    action = @selector(orderFrontStandardAboutPanel:);
  } else if (RoleIs(role, "hide")) {
    title = @"Hide";
    action = @selector(hide:);
    keyEquiv = @"h";
  } else if (RoleIs(role, "hideothers")) {
    title = @"Hide Others";
    action = @selector(hideOtherApplications:);
    keyEquiv = @"h";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
  } else if (RoleIs(role, "unhide")) {
    title = @"Show All";
    action = @selector(unhideAllApplications:);
  } else if (RoleIs(role, "front")) {
    title = @"Bring All to Front";
    action = @selector(arrangeInFront:);
  } else if (RoleIs(role, "togglefullscreen")) {
    title = @"Toggle Full Screen";
    action = @selector(toggleFullScreen:);
    keyEquiv = @"f";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
  } else {
    return nil;
  }
  if (!entry.label.empty())
    title = [NSString stringWithUTF8String:entry.label.c_str()];

  NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title
                                                action:action
                                         keyEquivalent:keyEquiv];
  [item setKeyEquivalentModifierMask:mask];
  return item;
}

// The role a role item was built from (an NSString, read by menu_snapshot).
const char kMenuItemRoleKey = 0;

// The accelerator an NSMenuItem's key equivalent is: KeyEquivalentFor's
// mapping read backwards. False for no key equivalent or one laufey never
// writes.
bool AcceleratorFromKeyEquivalent(NSString* key, NSEventModifierFlags mask,
                                  Accelerator* out) {
  if (!key || key.length != 1)
    return false;
  unichar c = [key characterAtIndex:0];
  Accelerator a;
  if (mask & NSEventModifierFlagCommand)
    a.mods |= kModSuper;
  if (mask & NSEventModifierFlagControl)
    a.mods |= kModCtrl;
  if (mask & NSEventModifierFlagOption)
    a.mods |= kModAlt;
  if (mask & NSEventModifierFlagShift)
    a.mods |= kModShift;
  const bool numpad = (mask & NSEventModifierFlagNumericPad) != 0;
  if (c >= NSF1FunctionKey && c <= NSF35FunctionKey) {
    a.kind = KeyKind::kFunction;
    a.number = static_cast<int>(c - NSF1FunctionKey) + 1;
  } else if (numpad && c >= '0' && c <= '9') {
    a.kind = KeyKind::kNumpad;
    a.named = NamedKey::kNone;
    a.number = c - '0';
  } else if (numpad &&
             (c == '.' || c == '+' || c == '-' || c == '*' || c == '/')) {
    a.kind = KeyKind::kNumpad;
    a.named = c == '.'   ? NamedKey::kNumDecimal
              : c == '+' ? NamedKey::kNumAdd
              : c == '-' ? NamedKey::kNumSubtract
              : c == '*' ? NamedKey::kNumMultiply
                         : NamedKey::kNumDivide;
  } else if (c >= 'a' && c <= 'z') {
    a.kind = KeyKind::kLetter;
    a.ch = static_cast<char>(c - 'a' + 'A');
  } else if (c >= 'A' && c <= 'Z') {
    // An upper-case key equivalent means Shift to AppKit.
    a.kind = KeyKind::kLetter;
    a.ch = static_cast<char>(c);
    a.mods |= kModShift;
  } else if (c >= '0' && c <= '9') {
    a.kind = KeyKind::kDigit;
    a.ch = static_cast<char>(c);
  } else {
    a.kind = KeyKind::kNamed;
    switch (c) {
      case ' ':
        a.named = NamedKey::kSpace;
        break;
      case '\t':
        a.named = NamedKey::kTab;
        break;
      case NSBackspaceCharacter:
        a.named = NamedKey::kBackspace;
        break;
      case NSDeleteFunctionKey:
        a.named = NamedKey::kDelete;
        break;
      case NSInsertFunctionKey:
        a.named = NamedKey::kInsert;
        break;
      case '\r':
        a.named = NamedKey::kEnter;
        break;
      case 0x1b:
        a.named = NamedKey::kEscape;
        break;
      case NSUpArrowFunctionKey:
        a.named = NamedKey::kUp;
        break;
      case NSDownArrowFunctionKey:
        a.named = NamedKey::kDown;
        break;
      case NSLeftArrowFunctionKey:
        a.named = NamedKey::kLeft;
        break;
      case NSRightArrowFunctionKey:
        a.named = NamedKey::kRight;
        break;
      case NSHomeFunctionKey:
        a.named = NamedKey::kHome;
        break;
      case NSEndFunctionKey:
        a.named = NamedKey::kEnd;
        break;
      case NSPageUpFunctionKey:
        a.named = NamedKey::kPageUp;
        break;
      case NSPageDownFunctionKey:
        a.named = NamedKey::kPageDown;
        break;
      case NSPrintScreenFunctionKey:
        a.named = NamedKey::kPrintScreen;
        break;
      default:
        if (c <= 0x20 || c >= 0x7f)
          return false;
        a.kind = KeyKind::kPunct;
        a.ch = static_cast<char>(c);
        break;
    }
  }
  *out = a;
  return true;
}

// What `menu` holds now, as menu_snapshot reports it.
std::vector<MenuSnapshotItem> SnapshotNSMenu(NSMenu* menu, int depth) {
  std::vector<MenuSnapshotItem> out;
  if (!menu || depth > 16)
    return out;
  for (NSMenuItem* item in [menu itemArray]) {
    MenuSnapshotItem s;
    if ([item isSeparatorItem]) {
      s.type = MenuSnapshotItem::Type::kSeparator;
      s.visible = ![item isHidden];
      out.push_back(std::move(s));
      continue;
    }
    NSString* title = [item title];
    s.label = title ? std::string([title UTF8String] ?: "") : std::string();
    s.enabled = [item isEnabled];
    s.checked = [item state] == NSControlStateValueOn;
    s.visible = ![item isHidden];
    id rep = [item representedObject];
    if ([rep isKindOfClass:[LaufeyCommonMenuItem class]]) {
      NSString* item_id = [(LaufeyCommonMenuItem*)rep itemId];
      if (item_id)
        s.id = [item_id UTF8String] ?: "";
    }
    id role = objc_getAssociatedObject(item, &kMenuItemRoleKey);
    if ([role isKindOfClass:[NSString class]])
      s.role = [(NSString*)role UTF8String] ?: "";
    Accelerator accel;
    if (AcceleratorFromKeyEquivalent([item keyEquivalent],
                                     [item keyEquivalentModifierMask], &accel))
      s.accelerator = CanonicalAccelerator(accel);
    if ([item hasSubmenu]) {
      // The menu bar shows a submenu under its own title when the item has
      // none (laufey's Edit menu).
      if (s.label.empty() && [[item submenu] title])
        s.label = [[[item submenu] title] UTF8String] ?: "";
      s.type = MenuSnapshotItem::Type::kSubmenu;
      s.children = SnapshotNSMenu([item submenu], depth + 1);
    }
    out.push_back(std::move(s));
  }
  return out;
}

NSString* NSStr(const std::string& s) {
  NSString* r = [NSString stringWithUTF8String:s.c_str()];
  return r ? r : @"";
}

// Per-window app menus, for the accelerator test hook. Main thread only.
std::map<uint32_t, NSMenu*>& WindowMenus() {
  static std::map<uint32_t, NSMenu*> menus;
  return menus;
}

}  // namespace

NSMenu* BuildNSMenuFromEntries(const std::vector<MenuEntry>& entries,
                               laufey_menu_click_fn on_click,
                               void* on_click_data, uint32_t window_id) {
  NSMenu* menu = [[NSMenu alloc] init];
  [menu setAutoenablesItems:NO];
  for (const MenuEntry& e : entries) {
    switch (e.kind) {
      case MenuEntry::Kind::kSeparator:
        [menu addItem:[NSMenuItem separatorItem]];
        continue;
      case MenuEntry::Kind::kRole: {
        NSMenuItem* roleItem = CreateRoleMenuItem(e);
        if (roleItem) {
          // menu_snapshot reports the role an item was built from.
          objc_setAssociatedObject(roleItem, &kMenuItemRoleKey, NSStr(e.role),
                                   OBJC_ASSOCIATION_COPY_NONATOMIC);
          [menu addItem:roleItem];
        }
        continue;
      }
      case MenuEntry::Kind::kSubmenu: {
        NSMenuItem* submenuItem = [[NSMenuItem alloc] init];
        [submenuItem setTitle:NSStr(e.label)];
        NSMenu* submenu = BuildNSMenuFromEntries(e.children, on_click,
                                                 on_click_data, window_id);
        [submenu setTitle:NSStr(e.label)];
        [submenuItem setSubmenu:submenu];
        [menu addItem:submenuItem];
        continue;
      }
      case MenuEntry::Kind::kItem:
        break;
    }

    NSString* keyEquiv = @"";
    NSEventModifierFlags modMask = 0;
    if (e.has_accel && !KeyEquivalentFor(e.accel, &keyEquiv, &modMask)) {
      keyEquiv = @"";
      modMask = 0;
    }

    NSMenuItem* nsItem =
        [[NSMenuItem alloc] initWithTitle:NSStr(e.label)
                                   action:@selector(menuItemClicked:)
                            keyEquivalent:keyEquiv];
    [nsItem setKeyEquivalentModifierMask:modMask];
    [nsItem setTarget:[LaufeyCommonMenuTarget shared]];

    if (!e.id.empty()) {
      LaufeyCommonMenuItem* wrap = [[LaufeyCommonMenuItem alloc] init];
      wrap.itemId = NSStr(e.id);
      wrap.clickFn = on_click;
      wrap.clickData = on_click_data;
      wrap.windowId = window_id;
      [nsItem setRepresentedObject:wrap];
      RegisterMenuClick(e.id, on_click, on_click_data, window_id);
    }

    [nsItem setEnabled:e.enabled ? YES : NO];
    if (e.checked)
      [nsItem setState:NSControlStateValueOn];
    if (!e.tooltip.empty())
      [nsItem setToolTip:NSStr(e.tooltip)];
    // "destructive" (API 49): AppKit has no destructive item style, so the
    // title is drawn in the system red (what Finder's own "Delete" items
    // look like in a contextual menu's emphasis).
    if (e.destructive) {
      NSDictionary* attrs = @{
        NSForegroundColorAttributeName : [NSColor systemRedColor],
        NSFontAttributeName : [NSFont menuFontOfSize:0],
      };
      [nsItem setAttributedTitle:[[NSAttributedString alloc]
                                     initWithString:NSStr(e.label)
                                         attributes:attrs]];
    }

    // icon -> NSMenuItem.image, decoded from PNG bytes. Marked as a template
    // image so a monochrome (black + alpha) icon tints correctly: black in the
    // normal state, white when the item is highlighted/selected. (A full-color
    // icon is flattened to its alpha mask under this default, matching the
    // tray-icon behavior.)
    if (!e.icon_png.empty()) {
      NSData* iconData = [NSData dataWithBytes:e.icon_png.data()
                                        length:e.icon_png.size()];
      NSImage* iconImg = [[NSImage alloc] initWithData:iconData];
      if (iconImg) {
        [iconImg setSize:NSMakeSize(16, 16)];
        [iconImg setTemplate:YES];
        [nsItem setImage:iconImg];
      }
    }

    [menu addItem:nsItem];
  }
  return menu;
}

NSMenu* BuildNSMenuFromValue(laufey_value_t* val,
                             const laufey_backend_api_t* api,
                             laufey_menu_click_fn on_click, void* on_click_data,
                             uint32_t window_id) {
  if (!val || !api->value_is_list(val))
    return nil;
  return BuildNSMenuFromEntries(ParseMenuTemplate(val, api, true), on_click,
                                on_click_data, window_id);
}

std::string MenuSnapshotMac(int32_t kind, uint32_t window_id) {
  __block std::string json;
  RunOnMainSync(^{
    NSMenu* menu = nil;
    if (kind == LAUFEY_MENU_KIND_APPLICATION) {
      if (window_id == 0) {
        menu = [NSApp mainMenu];
      } else {
        auto it = WindowMenus().find(window_id);
        if (it != WindowMenus().end())
          menu = it->second;
      }
    } else if (kind == LAUFEY_MENU_KIND_DOCK)
      menu = GetDockMenuMac();
    if (menu)
      json = MenuSnapshotJson(SnapshotNSMenu(menu, 0));
  });
  return json;
}

void RegisterWindowMenuMac(uint32_t window_id, NSMenu* menu) {
  RunOnMainSync(^{
    if (menu)
      WindowMenus()[window_id] = menu;
    else
      WindowMenus().erase(window_id);
  });
}

bool TestTriggerMenuAcceleratorMac(uint32_t window_id,
                                   const char* accelerator) {
  if (!accelerator)
    return false;
  Accelerator accel;
  if (!ParseAccelerator(accelerator, true, &accel, nullptr))
    return false;
  NSString* key = nil;
  NSEventModifierFlags mask = 0;
  if (!KeyEquivalentFor(accel, &key, &mask))
    return false;
  __block bool fired = false;
  RunOnMainSync(^{
    auto it = WindowMenus().find(window_id);
    if (it == WindowMenus().end())
      return;
    NSMenu* menu = it->second;
    NSEvent* event = [NSEvent
                   keyEventWithType:NSEventTypeKeyDown
                           location:NSZeroPoint
                      modifierFlags:mask
                          timestamp:[[NSProcessInfo processInfo] systemUptime]
                       windowNumber:0
                            context:nil
                         characters:key
        charactersIgnoringModifiers:key
                          isARepeat:NO
                            keyCode:0];
    // The AppKit matching the main menu does for a key press: the item whose
    // key equivalent and modifiers match runs its action (a disabled one
    // matches without running).
    uint64_t before = g_menu_clicks;
    fired =
        event && [menu performKeyEquivalent:event] && g_menu_clicks != before;
  });
  return fired;
}

void RunFromMainRunLoopMac(std::function<void()> fn) {
  auto task = std::make_shared<std::function<void()>>(std::move(fn));
  CFRunLoopRef main = CFRunLoopGetMain();
  CFRunLoopPerformBlock(main, kCFRunLoopCommonModes, ^{
    (*task)();
  });
  CFRunLoopWakeUp(main);
}

void ShowContextMenuMac(void* nsview, int x, int y,
                        const std::vector<MenuEntry>& entries,
                        laufey_menu_click_fn on_click, void* on_click_data,
                        laufey_menu_closed_fn on_closed, void* on_closed_data,
                        uint32_t window_id) {
  NSView* view = (__bridge NSView*)nsview;
  if (!view || entries.empty()) {
    FireContextMenuClosedNow(window_id, on_closed, on_closed_data);
    return;
  }
  NSMenu* menu =
      BuildNSMenuFromEntries(entries, on_click, on_click_data, window_id);
  __weak NSMenu* weakMenu = menu;
  uint64_t session =
      BeginContextMenu(window_id, on_closed, on_closed_data, [weakMenu] {
        // The menu runs its own tracking loop on the main thread; a block in
        // the common modes runs inside it.
        CFRunLoopRef main = CFRunLoopGetMain();
        CFRunLoopPerformBlock(main, kCFRunLoopCommonModes, ^{
          [weakMenu cancelTracking];
        });
        CFRunLoopWakeUp(main);
      });
  // LAUFEY coordinates are window-relative with a top-left origin (the web
  // convention). -popUpMenuPositioningItem:atLocation:inView: reads the
  // location in the view's *own* coordinate system, which is top-left only
  // when the view is flipped.
  NSPoint loc =
      NSMakePoint(x, [view isFlipped] ? y : [view frame].size.height - y);
  // Modal: returns once the menu closed, after a chosen item's action ran.
  [menu popUpMenuPositioningItem:nil atLocation:loc inView:view];
  dispatch_async(dispatch_get_main_queue(), ^{
    EndContextMenu(session);
  });
}

namespace {

bool MenuTreeTakesCmdW(NSMenu* menu) {
  for (NSMenuItem* item in [menu itemArray]) {
    if ([item action] == @selector(performClose:))
      return true;
    NSEventModifierFlags mask =
        [item keyEquivalentModifierMask] &
        (NSEventModifierFlagCommand | NSEventModifierFlagShift |
         NSEventModifierFlagOption | NSEventModifierFlagControl);
    if ([[item keyEquivalent] isEqualToString:@"w"] &&
        mask == NSEventModifierFlagCommand)
      return true;
    if ([item submenu] && MenuTreeTakesCmdW([item submenu]))
      return true;
  }
  return false;
}

NSMenu* TopSubmenuTitled(NSMenu* menubar, NSString* title) {
  for (NSMenuItem* item in [menubar itemArray]) {
    NSMenu* sub = [item submenu];
    if (sub && ([[sub title] isEqualToString:title] ||
                [[item title] isEqualToString:title]))
      return sub;
  }
  return nil;
}

}  // namespace

bool EnsureCloseWindowItem(NSMenu* menubar) {
  if (!menubar || MenuTreeTakesCmdW(menubar))
    return false;
  NSMenuItem* close = [[NSMenuItem alloc] initWithTitle:@"Close Window"
                                                 action:@selector(performClose:)
                                          keyEquivalent:@"w"];
  if (NSMenu* file = TopSubmenuTitled(menubar, @"File")) {
    if ([file numberOfItems] > 0)
      [file addItem:[NSMenuItem separatorItem]];
    [file addItem:close];
    return true;
  }
  if (NSMenu* window = TopSubmenuTitled(menubar, @"Window")) {
    if ([window numberOfItems] > 0)
      [window insertItem:[NSMenuItem separatorItem] atIndex:0];
    [window insertItem:close atIndex:0];
    return true;
  }
  NSMenu* window = [[NSMenu alloc] initWithTitle:@"Window"];
  [window addItem:close];
  NSMenuItem* windowItem = [[NSMenuItem alloc] initWithTitle:@"Window"
                                                      action:nil
                                               keyEquivalent:@""];
  [windowItem setSubmenu:window];
  [menubar addItem:windowItem];
  return true;
}

}  // namespace laufey_common
