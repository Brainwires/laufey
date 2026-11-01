// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS's side of laufey_quit_request.h.

#import <Cocoa/Cocoa.h>

#include "laufey_quit_request.h"

namespace laufey_common {

int32_t QuitRequestKindMac() {
  NSAppleEventDescriptor* event =
      [[NSAppleEventManager sharedAppleEventManager] currentAppleEvent];
  if (!event || event.eventClass != kCoreEventClass ||
      event.eventID != kAEQuitApplication) {
    return LAUFEY_QUIT_REQUEST_APP;
  }
  switch ([event attributeDescriptorForKeyword:kAEQuitReason].enumCodeValue) {
    case kAELogOut:
    case kAEReallyLogOut:
    case kAEShowRestartDialog:
    case kAERestart:
    case kAEShowShutdownDialog:
    case kAEShutDown:
      return LAUFEY_QUIT_REQUEST_SESSION_END;
    default:
      return LAUFEY_QUIT_REQUEST_APP;
  }
}

bool AskQuitRequestMac(std::function<void(bool allow)> reply, bool* pending) {
  int32_t kind = QuitRequestKindMac();
  uint64_t generation = 0;
  // Always later, on the main thread: an answer from inside the handler must
  // not reach AppKit before applicationShouldTerminate: has returned. Through
  // the run loop, not the main dispatch queue: AppKit's wait is a nested loop,
  // and a terminate: sent from a main-queue block would wait with that queue
  // held, so nothing queued there could ever end it.
  auto on_main = [reply = std::move(reply)](bool allow) {
    CFRunLoopRef loop = CFRunLoopGetMain();
    CFRunLoopPerformBlock(loop, kCFRunLoopCommonModes, ^{
      reply(allow);
    });
    CFRunLoopWakeUp(loop);
  };
  QuitRequestStart start = BeginQuitRequest(kind, on_main, &generation);
  *pending = start == QuitRequestStart::kPending;
  if (start != QuitRequestStart::kAsked)
    return false;
  int ms = kind == LAUFEY_QUIT_REQUEST_SESSION_END
               ? kQuitRequestSessionEndDeadlineMacMs
               : kQuitRequestAppDeadlineMs;
  NSTimer* deadline = [NSTimer timerWithTimeInterval:ms / 1000.0
                                             repeats:NO
                                               block:^(NSTimer*) {
                                                 ExpireQuitRequest(generation);
                                               }];
  [[NSRunLoop mainRunLoop] addTimer:deadline forMode:NSRunLoopCommonModes];
  return true;
}

}  // namespace laufey_common
