// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Auth sessions on macOS: ASWebAuthenticationSession (macOS 10.15+), a sheet
// on the app's window. See laufey_auth_session.h and docs/auth-session.md.

#import <AppKit/AppKit.h>
#import <AuthenticationServices/AuthenticationServices.h>

#include "laufey_auth_session.h"

#include <memory>
#include <string>

using laufey_common::AuthSession;

// The SDK knows ASWebAuthenticationSessionCallback (https callbacks).
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && \
    __MAC_OS_X_VERSION_MAX_ALLOWED >= 140400
#define LAUFEY_HAS_AUTH_SESSION_CALLBACK 1
#else
#define LAUFEY_HAS_AUTH_SESSION_CALLBACK 0
#endif

// Owns one session: the presentation context provider of its
// ASWebAuthenticationSession, which holds it weakly, so live providers are
// kept in LiveProviders() (main thread only) until the session ends.
@interface LaufeyAuthSessionProvider
    : NSObject <ASWebAuthenticationPresentationContextProviding>
@property(nonatomic, strong) ASWebAuthenticationSession* session;
@property(nonatomic, strong) NSWindow* anchor;
@end

@implementation LaufeyAuthSessionProvider
- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:
    (ASWebAuthenticationSession*)session {
  return self.anchor;
}
@end

namespace laufey_common {

namespace {

NSMutableSet* LiveProviders() {
  static NSMutableSet* set = [NSMutableSet set];
  return set;
}

std::string ToStdString(NSString* s) {
  const char* utf8 = s ? s.UTF8String : nullptr;
  return utf8 ? std::string(utf8) : std::string();
}

// A sheet's parent for a sheet (a cancelled session's sheet can still be the
// key window while it leaves), else the window itself.
NSWindow* SheetOwner(NSWindow* w) {
  return w && w.isSheet && w.sheetParent ? w.sheetParent : w;
}

// The window to anchor the sheet to when the caller named none. Never a
// sheet: its parent instead.
NSWindow* DefaultAnchor() {
  if (NSApp.keyWindow)
    return SheetOwner(NSApp.keyWindow);
  if (NSApp.mainWindow)
    return SheetOwner(NSApp.mainWindow);
  for (NSWindow* w in NSApp.orderedWindows) {
    if (w.visible && !w.isSheet)
      return w;
  }
  return nil;
}

// Whether `w` has a sheet on it, or one still on its way out.
bool HasSheet(NSWindow* w) {
  return w && (w.attachedSheet != nil || w.sheets.count > 0);
}

// Sessions laufey cancelled (AuthSession::Cancel: the app, the anchor window
// closing, the test hook, shutdown) whose OS side has not ended yet, and the
// starts waiting for them. Main thread only.
//
// AuthSession::Cancel frees the app-wide slot at once, but the OS is still
// dismissing the cancelled sheet then. A session started meanwhile (with the
// cancelled ASWebAuthenticationSession released mid-dismissal, as it used to
// be) was now and then never presented and never answered, and kept the slot
// for good (about 2% of macOS CI legs, which did this once each). So the
// cancelled session is kept until the OS reports its end, and a start waits
// for that.
NSMutableSet* RetiringProviders() {
  static NSMutableSet* set = [NSMutableSet set];
  return set;
}

NSMutableArray* WaitingStarts() {
  static NSMutableArray* starts = [NSMutableArray array];
  return starts;
}

// The OS reports nothing for a session cancelled while its consent prompt is
// up, so a cancelled session is let go without that report: no sooner than
// this (the floor), and then only once its anchor window has no sheet left
// (the cancelled sheet can take longer to leave on a slow or loaded Mac; a
// session presented meanwhile was never shown and never answered: a macOS
// Intel CI leg, run 43985473064), polled every kSheetPollNs.
constexpr int64_t kRetireGraceNs = 3 * NSEC_PER_SEC;
constexpr int64_t kSheetPollNs = 250 * NSEC_PER_MSEC;
// The longest a cancelled session waits for its anchor's sheet to leave, and
// a start waits for a sheet to leave its window: never forever.
constexpr CFTimeInterval kSheetWaitCeilingS = 15.0;

void RunWaitingStarts() {
  if (RetiringProviders().count != 0 || WaitingStarts().count == 0)
    return;
  NSArray* starts = [WaitingStarts() copy];
  [WaitingStarts() removeAllObjects];
  for (dispatch_block_t start in starts)
    start();
}

// The OS ended the session (its completion handler ran), starting it failed,
// or a cancelled one's grace period passed: let it go, and run the starts
// that waited for it.
void Release(LaufeyAuthSessionProvider* provider) {
  dispatch_async(dispatch_get_main_queue(), ^{
    provider.session = nil;
    provider.anchor = nil;
    [LiveProviders() removeObject:provider];
    [RetiringProviders() removeObject:provider];
    RunWaitingStarts();
  });
}

// A retiring session past its floor: let go once its anchor has no sheet
// (or the anchor is gone), or at the ceiling.
void ReleaseWhenSheetLeft(LaufeyAuthSessionProvider* provider,
                          __weak NSWindow* weak_anchor, CFAbsoluteTime since) {
  if (![RetiringProviders() containsObject:provider])
    return;  // The OS reported its end (Release).
  NSWindow* anchor = weak_anchor;
  if (HasSheet(anchor) &&
      CFAbsoluteTimeGetCurrent() - since < kSheetWaitCeilingS) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kSheetPollNs),
                   dispatch_get_main_queue(), ^{
                     ReleaseWhenSheetLeft(provider, weak_anchor, since);
                   });
    return;
  }
  Release(provider);
}

// Main thread, right after [session cancel]: keep the session until the OS
// reports its end (Release), or, past the floor, until its anchor's sheet
// has left (at most kSheetWaitCeilingS).
void Retire(LaufeyAuthSessionProvider* provider) {
  if (![LiveProviders() containsObject:provider])
    return;  // Already let go.
  [RetiringProviders() addObject:provider];
  __weak NSWindow* weak_anchor = provider.anchor;
  CFAbsoluteTime since = CFAbsoluteTimeGetCurrent();
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kRetireGraceNs),
                 dispatch_get_main_queue(), ^{
                   ReleaseWhenSheetLeft(provider, weak_anchor, since);
                 });
}

void StartOnMain(std::shared_ptr<AuthSession> session, NSWindow* window,
                 CFAbsoluteTime sheet_since);

}  // namespace

uint32_t AuthSessionCapabilities() {
  uint32_t caps = 0;
  if (@available(macOS 10.15, *)) {
    caps |=
        LAUFEY_AUTH_SESSION_CAP_SUPPORTED | LAUFEY_AUTH_SESSION_CAP_EPHEMERAL;
  }
#if LAUFEY_HAS_AUTH_SESSION_CALLBACK
  if (@available(macOS 14.4, *)) {
    caps |= LAUFEY_AUTH_SESSION_CAP_HTTPS_CALLBACK;
  }
#endif
  return caps;
}

void AuthSessionStartMac(std::shared_ptr<AuthSession> session,
                         void* ns_window) {
  if (!session)
    return;
  NSWindow* window = (__bridge NSWindow*)ns_window;
  if (![NSThread isMainThread]) {
    dispatch_async(dispatch_get_main_queue(), ^{
      StartOnMain(session, window, CFAbsoluteTimeGetCurrent());
    });
    return;
  }
  StartOnMain(session, window, CFAbsoluteTimeGetCurrent());
}

namespace {

void StartOnMain(std::shared_ptr<AuthSession> session, NSWindow* window,
                 CFAbsoluteTime sheet_since) {
  if (@available(macOS 10.15, *)) {
    @autoreleasepool {
      // Cancelled (window closed, test hook, shutdown) before it started.
      if (session->delivered()) {
        session->Finish(LAUFEY_AUTH_SESSION_CANCELLED,
                        "the auth session was cancelled");
        return;
      }
      if (!window)
        window = DefaultAnchor();
      // A cancelled session is still leaving: start once it has. The window
      // key is set now, so the anchor closing meanwhile cancels this one (and
      // the delivered() check above then ends the waiting start).
      if (RetiringProviders().count != 0) {
        session->SetWindowKey((__bridge const void*)window);
        NSWindow* waiting_window = window;
        [WaitingStarts() addObject:^{
          StartOnMain(session, waiting_window, CFAbsoluteTimeGetCurrent());
        }];
        return;
      }
      // Never presented into a window whose sheet is still there or leaving
      // (one presented then was never shown and never answered): queued
      // behind it, polled, for at most kSheetWaitCeilingS, then presented.
      // The window key is set, so the window closing meanwhile cancels it.
      if (HasSheet(window) &&
          CFAbsoluteTimeGetCurrent() - sheet_since < kSheetWaitCeilingS) {
        session->SetWindowKey((__bridge const void*)window);
        NSWindow* waiting_window = window;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kSheetPollNs),
                       dispatch_get_main_queue(), ^{
                         StartOnMain(session, waiting_window, sheet_since);
                       });
        return;
      }
      LaufeyAuthSessionProvider* provider =
          [[LaufeyAuthSessionProvider alloc] init];
      // An app with no window (a menu-bar app) still gets the sheet: the OS
      // shows it on a window of its own when the anchor is not on screen.
      provider.anchor = window ? window : [[NSWindow alloc] init];

      NSURL* url = [NSURL URLWithString:@(session->url().c_str())];
      if (!url) {
        session->Finish(LAUFEY_AUTH_SESSION_INVALID, "url is not a valid URL");
        return;
      }

      std::shared_ptr<AuthSession> owner = session;
      __weak LaufeyAuthSessionProvider* weak_provider = provider;
      // Any thread (AuthenticationServices calls it on one of its own).
      auto completion = ^(NSURL* callback_url, NSError* error) {
        LaufeyAuthSessionProvider* p = weak_provider;
        if (callback_url) {
          owner->Finish(LAUFEY_AUTH_SESSION_OK,
                        ToStdString(callback_url.absoluteString));
        } else if (error &&
                   [error.domain
                       isEqualToString:ASWebAuthenticationSessionErrorDomain] &&
                   error.code ==
                       ASWebAuthenticationSessionErrorCodeCanceledLogin) {
          owner->Finish(LAUFEY_AUTH_SESSION_CANCELLED,
                        "the user cancelled the sign-in");
        } else {
          std::string message = "the auth session failed";
          if (error)
            message += ": " + ToStdString(error.localizedDescription);
          owner->Finish(LAUFEY_AUTH_SESSION_FAILED, message);
        }
        if (p)
          Release(p);
      };

      const AuthSessionCallback& cb = session->callback();
      ASWebAuthenticationSession* as = nil;
#if LAUFEY_HAS_AUTH_SESSION_CALLBACK
      if (@available(macOS 14.4, *)) {
        ASWebAuthenticationSessionCallback* callback =
            cb.https ? [ASWebAuthenticationSessionCallback
                           callbackWithHTTPSHost:@(cb.host.c_str())
                                            path:@(cb.path.c_str())]
                     : [ASWebAuthenticationSessionCallback
                           callbackWithCustomScheme:@(cb.scheme.c_str())];
        as = [[ASWebAuthenticationSession alloc] initWithURL:url
                                                    callback:callback
                                           completionHandler:completion];
      }
#endif
      if (!as) {
        if (cb.https) {
          // AuthSessionBegin refuses this without the capability.
          session->Finish(LAUFEY_AUTH_SESSION_NOT_SUPPORTED,
                          "an https callback needs macOS 14.4 or later");
          return;
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        as =
            [[ASWebAuthenticationSession alloc] initWithURL:url
                                          callbackURLScheme:@(cb.scheme.c_str())
                                          completionHandler:completion];
#pragma clang diagnostic pop
      }
      as.presentationContextProvider = provider;
      as.prefersEphemeralWebBrowserSession = session->ephemeral();
      provider.session = as;
      [LiveProviders() addObject:provider];

      session->SetWindowKey((__bridge const void*)window);
      // Window closed / test hook / app / shutdown: dismiss the sheet. The
      // OS reports CanceledLogin afterwards for a session past its consent
      // prompt, and nothing while the prompt is up; either way the result
      // was already delivered by AuthSession::Cancel. The session is kept
      // until then (Retire), and the next start waits for it. On the main
      // thread this happens at once, so a start right after it, in the same
      // turn of the run loop, waits too.
      __weak ASWebAuthenticationSession* weak_session = as;
      session->SetCanceller([weak_session, weak_provider] {
        dispatch_block_t cancel = ^{
          [weak_session cancel];
          LaufeyAuthSessionProvider* p = weak_provider;
          if (p)
            Retire(p);
        };
        if ([NSThread isMainThread])
          cancel();
        else
          dispatch_async(dispatch_get_main_queue(), cancel);
      });

      BOOL started = NO;
      @try {
        started = [as start];
      } @catch (NSException* e) {
        session->Finish(
            LAUFEY_AUTH_SESSION_FAILED,
            "starting the auth session failed: " + ToStdString(e.reason));
        Release(provider);
        return;
      }
      if (!started) {
        session->Finish(LAUFEY_AUTH_SESSION_FAILED,
                        "the OS did not start the auth session");
        Release(provider);
      }
    }
  } else {
    session->Finish(LAUFEY_AUTH_SESSION_NOT_SUPPORTED,
                    "auth sessions need macOS 10.15 or newer");
  }
}

}  // namespace

void AuthSessionStartMacQueued(std::shared_ptr<AuthSession> session,
                               void* ns_window) {
  if (!session)
    return;
  NSWindow* window = (__bridge NSWindow*)ns_window;
  dispatch_async(dispatch_get_main_queue(), ^{
    AuthSessionStartMac(session, (__bridge void*)window);
  });
}

}  // namespace laufey_common
