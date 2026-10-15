// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The share sheet on macOS (laufey_share.h): NSSharingServicePicker over the
// window, reporting the service chosen or a dismissal.

#include "laufey_share.h"

#import <Cocoa/Cocoa.h>

#include <string>

@interface LaufeySharePickerDelegate : NSObject <NSSharingServicePickerDelegate>
@property(nonatomic, assign) laufey_share_fn callback;
@property(nonatomic, assign) void* userData;
@property(nonatomic, strong) NSSharingServicePicker* picker;
@end

namespace {
// The pickers on screen (their delegates), kept until they answer: the
// picker holds its delegate weakly.
NSMutableSet<LaufeySharePickerDelegate*>* LivePickers() {
  static NSMutableSet<LaufeySharePickerDelegate*>* set = [NSMutableSet set];
  return set;
}
}  // namespace

@implementation LaufeySharePickerDelegate
- (void)sharingServicePicker:(NSSharingServicePicker*)picker
     didChooseSharingService:(NSSharingService*)service {
  laufey_share_fn cb = self.callback;
  void* ud = self.userData;
  self.callback = nullptr;
  [LivePickers() removeObject:self];
  if (!cb)
    return;
  if (service) {
    NSString* title = service.title ?: @"";
    cb(ud, LAUFEY_SHARE_COMPLETED, title.UTF8String);
  } else {
    cb(ud, LAUFEY_SHARE_CANCELLED, nullptr);
  }
}
@end

namespace laufey_common {

void ShareShowMac(void* ns_window, const ShareRequest& request,
                  laufey_share_fn callback, void* user_data) {
  @autoreleasepool {
    NSWindow* window = (__bridge NSWindow*)ns_window;
    if (!window)
      window = NSApp.keyWindow ?: NSApp.mainWindow;
    NSView* view = window.contentView;
    if (!view) {
      ShareReport(callback, user_data, LAUFEY_SHARE_FAILED,
                  "no window to show the share sheet over");
      return;
    }
    NSMutableArray* items = [NSMutableArray array];
    if (!request.text.empty()) {
      NSString* text = [NSString stringWithUTF8String:request.text.c_str()];
      if (text)
        [items addObject:text];
    }
    if (!request.url.empty()) {
      NSURL* url = [NSURL
          URLWithString:[NSString stringWithUTF8String:request.url.c_str()]];
      if (url)
        [items addObject:url];
    }
    for (const std::string& path : request.files) {
      NSString* p = [NSString stringWithUTF8String:path.c_str()];
      if (p && [[NSFileManager defaultManager] fileExistsAtPath:p])
        [items addObject:[NSURL fileURLWithPath:p]];
    }
    if (items.count == 0) {
      ShareReport(callback, user_data, LAUFEY_SHARE_FAILED,
                  "nothing to share (a missing file, a malformed URL)");
      return;
    }
    LaufeySharePickerDelegate* delegate =
        [[LaufeySharePickerDelegate alloc] init];
    delegate.callback = callback;
    delegate.userData = user_data;
    NSSharingServicePicker* picker =
        [[NSSharingServicePicker alloc] initWithItems:items];
    picker.delegate = delegate;
    delegate.picker = picker;
    [LivePickers() addObject:delegate];
    NSRect bounds = view.bounds;
    // The anchor, in the window's top-left DIPs (the view may be flipped).
    CGFloat x = request.has_point ? request.x : NSMidX(bounds);
    CGFloat y = request.has_point ? request.y : NSMidY(bounds);
    if (!view.isFlipped)
      y = NSHeight(bounds) - y;
    [picker showRelativeToRect:NSMakeRect(x, y, 1, 1)
                        ofView:view
                 preferredEdge:NSMinYEdge];
  }
}

}  // namespace laufey_common
