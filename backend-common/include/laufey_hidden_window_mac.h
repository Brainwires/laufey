// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// macOS: hiding a window by keeping it ordered in but invisible
// (LAUFEY_WINDOW_FLAG_MACOS_HIDDEN_ORDER_OUT in laufey.h). WebKit runs a
// page at full rate only while its view is on screen, and the view of an
// ordered-out window never is, so a window whose page must keep running at
// full rate (BackgroundThrottling::None) is hidden this way instead: fully
// transparent, without a shadow, passing every click through, out of window
// cycling, Mission Control and Exposé (a transient window), the Window menu,
// screen sharing and screenshots, and accessibility, at the back of the
// window list, and never key or main (ordering it out hands those to the
// next window; ordering it back in at the back doesn't take them). Showing
// it restores each of those properties exactly as they were.
//
// Every `window` is an NSWindow*. Main thread.

#ifndef LAUFEY_HIDDEN_WINDOW_MAC_H_
#define LAUFEY_HIDDEN_WINDOW_MAC_H_

namespace laufey_common {

// Hides `window` keeping it ordered in. False (nothing done) where that
// can't apply: a full-screen window (an invisible one would leave its Space
// empty), or no window; the caller orders it out instead. Hiding a window
// already hidden this way does nothing (true).
bool MacHideInvisibly(void* window);

// Undoes MacHideInvisibly: restores the properties it changed. The window
// stays where it is in the window list; the caller orders it front (and
// makes it key) as for any show. False when it wasn't hidden this way.
bool MacRestoreFromInvisibleHide(void* window);

// Drops what MacHideInvisibly recorded without restoring anything, for a
// window that is about to close: restoring first would make it visible for
// a moment. False when it wasn't hidden this way.
bool MacForgetInvisibleHide(void* window);

// Whether `window` is hidden by MacHideInvisibly.
bool MacIsInvisiblyHidden(const void* window);

// The properties MacHideInvisibly takes over, set and read through it: on a
// window it hides, a value set is recorded and applied when the window is
// shown (MacRestoreFromInvisibleHide), so it stays invisible meanwhile and
// the value isn't lost, and the getters read that value back; on any other
// window these are the NSWindow properties. `behavior` is an
// NSWindowCollectionBehavior.
void MacSetWindowAlpha(void* window, double alpha);
double MacWindowAlpha(const void* window);
void MacSetIgnoresMouseEvents(void* window, bool ignores);
bool MacIgnoresMouseEvents(const void* window);
void MacSetCollectionBehavior(void* window, unsigned long behavior);
unsigned long MacCollectionBehavior(const void* window);

// Whether `window` is on screen for the user: ordered in (isVisible) and not
// hidden by MacHideInvisibly. What "visible" means for laufey's own checks
// (is_visible, sheet parents, the Dock's reopen, click passthrough).
bool MacWindowShown(const void* window);

// The Dock's reopen: AppKit's `hasVisibleWindows` counts a window hidden by
// MacHideInvisibly, which the user doesn't see. False when every window it
// can have counted (any on screen but the status bar's) is hidden that way;
// else `appkit_says`.
bool MacHasVisibleWindowsForReopen(bool appkit_says);

}  // namespace laufey_common

#endif  // LAUFEY_HIDDEN_WINDOW_MAC_H_
