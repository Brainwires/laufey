// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS message dialogs (API 54): NSAlert, a sheet on its window or
// app-modal. Everything runs from the main run loop (RunFromMainRunLoopMac),
// not as a main-queue block: an app-modal alert's runModal is a nested loop,
// and while a main-queue block runs none of the others do, so the backend's
// UI-thread calls (dispatch_sync / dispatch_async to the main queue) would
// wait for the user to answer.

#include "laufey_backend_common.h"
#include "laufey_menu.h"
#include "laufey_message_dialog.h"

#import <Cocoa/Cocoa.h>

#include <string>

namespace laufey_common {

namespace {

NSString* ToNS(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

NSAlert* BuildAlert(const MessageDialogRequest& req) {
  NSAlert* alert = [[NSAlert alloc] init];
  switch (req.kind) {
    case LAUFEY_MESSAGE_DIALOG_WARNING:
      alert.alertStyle = NSAlertStyleWarning;
      break;
    case LAUFEY_MESSAGE_DIALOG_ERROR:
      alert.alertStyle = NSAlertStyleCritical;
      break;
    default:
      alert.alertStyle = NSAlertStyleInformational;
      break;
  }
  alert.messageText = ToNS(req.message);
  alert.informativeText = ToNS(req.detail);
  // An alert has no visible title bar; the title still names its window
  // for accessibility.
  if (!req.title.empty())
    alert.window.title = ToNS(req.title);
  for (size_t i = 0; i < req.buttons.size(); i++) {
    NSButton* button = [alert addButtonWithTitle:ToNS(req.buttons[i])];
    // NSAlert gives Return to the first button and Escape to one titled
    // "Cancel"; the request says which.
    button.keyEquivalent = @"";
    button.keyEquivalentModifierMask = 0;
  }
  NSArray<NSButton*>* buttons = alert.buttons;
  if (req.cancel_index >= 0 &&
      static_cast<NSUInteger>(req.cancel_index) < buttons.count)
    buttons[req.cancel_index].keyEquivalent = @"\033";
  if (static_cast<NSUInteger>(req.default_index) < buttons.count)
    buttons[req.default_index].keyEquivalent = @"\r";
  return alert;
}

void FinishWithResponse(uint32_t id, NSModalResponse response,
                        size_t button_count) {
  NSInteger index = response - NSAlertFirstButtonReturn;
  if (index >= 0 && static_cast<size_t>(index) < button_count) {
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_ANSWERED,
                        static_cast<int32_t>(index));
  } else {
    // NSModalResponseStop / Abort: ended without a button (the app quit,
    // its window went away).
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_CANCELLED, -1);
  }
}

}  // namespace

uint32_t ShowMessageDialogMac(ParentResolver parent,
                              const laufey_message_dialog_options_t* options,
                              laufey_message_dialog_result_fn callback,
                              void* user_data) {
  return ShowMessageDialogCommon(
      options, callback, user_data,
      [](std::function<void()> fn) { RunFromMainRunLoopMac(std::move(fn)); },
      [parent](uint32_t id, const MessageDialogRequest& request) {
        @autoreleasepool {
          NSWindow* window = parent ? (__bridge NSWindow*)parent() : nil;
          NSAlert* alert = BuildAlert(request);
          size_t count = request.buttons.size();
          if (window && window.isVisible && !window.attachedSheet) {
            [alert beginSheetModalForWindow:window
                          completionHandler:^(NSModalResponse response) {
                            FinishWithResponse(id, response, count);
                          }];
            return;
          }
          // App-modal: there may be no window at all (a tray app, a quit
          // guard), so bring the app forward for the alert to be seen.
          [NSApp activateIgnoringOtherApps:YES];
          NSModalResponse response;
          {
            ScopedNativeModalLoop modal_loop;
            response = [alert runModal];
          }
          FinishWithResponse(id, response, count);
        }
      });
}

}  // namespace laufey_common
