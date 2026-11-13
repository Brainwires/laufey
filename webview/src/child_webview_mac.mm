// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views on WKWebView (API 58); see child_webview_mac.h and
// docs/webviews.md.
//
// Each view is its own WKWebView with a fresh WKWebViewConfiguration: no
// "laufey" script message handler, no init script, no custom-scheme
// handlers, and a website data store of its own (a persistent store per
// partition name on macOS 14+, else a non-persistent one per name), never
// the default store the app's page uses. It is a subview of the window's
// page WKWebView, so it draws above the page and moves with the window; the
// window's navigation guard and will-navigate handler belong to the page
// view's delegate and never see it.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "child_webview_mac.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "laufey_launch_config.h"
#include "laufey_navigation_guard.h"

using laufey_common::WebViewOptions;
using laufey_common::WebViewSinks;

@class LaufeyChildWebViewDelegate;

@interface LaufeyChildWebView : WKWebView
@property(nonatomic, assign) uint32_t laufeyViewId;
@end

namespace {

struct ChildView {
  uint32_t id = 0;
  uint32_t window_id = 0;
  LaufeyChildWebView* view = nil;
  LaufeyChildWebViewDelegate* delegate = nil;
  WebViewOptions options;
  const char* load = "complete";
  laufey_common::WebViewBounds bounds;
  bool visible = true;
  bool focused = false;
  // The last URL event, so the KVO bursts of a navigation send it once.
  std::string sent_url;
  bool sent_back = false;
  bool sent_forward = false;
  bool url_flush_pending = false;
  // Console events this second (a page can't flood the embedder).
  int64_t console_second = 0;
  int console_count = 0;
};

// Main thread only.
std::map<uint32_t, ChildView>& Views() {
  static auto* views = new std::map<uint32_t, ChildView>();
  return *views;
}

ChildView* Find(uint32_t id) {
  auto it = Views().find(id);
  return it == Views().end() ? nullptr : &it->second;
}

std::string Utf8(NSString* s) {
  return s ? std::string(s.UTF8String ?: "") : std::string();
}

NSString* NS(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

constexpr int kConsolePerSecond = 200;
constexpr size_t kConsoleMaxBytes = 16 * 1024;
NSString* const kConsoleHandler = @"__laufeyWebViewConsole";

// Page-world shim: WKWebView reports no console messages to the embedder.
// The page can see (and post to) this handler; it carries console text only.
NSString* const kConsoleShim =
    @"(() => {"
     " const h = window.webkit && window.webkit.messageHandlers &&"
     "  window.webkit.messageHandlers.__laufeyWebViewConsole;"
     " if (!h) return;"
     " const text = (args) => Array.prototype.map.call(args, (a) => {"
     "  if (typeof a === 'string') return a;"
     "  try { const s = JSON.stringify(a); return s === undefined ? String(a) "
     ": s; }"
     "  catch (_) { return String(a); } }).join(' ');"
     " for (const level of ['log', 'info', 'warn', 'error', 'debug']) {"
     "  const orig = console[level];"
     "  console[level] = function (...args) {"
     "   try { h.postMessage({ level, message: text(args) }); } catch (_) {}"
     "   return orig.apply(this, args);"
     "  };"
     " }"
     " window.addEventListener('error', (e) => {"
     "  try { h.postMessage({ level: 'error', message: String(e.message),"
     "   source: String(e.filename || ''), line: e.lineno | 0 }); } catch (_) "
     "{}"
     " });"
     "})();";

void Emit(uint32_t id, int32_t kind, const std::string& detail) {
  WebViewSinks::Instance().Emit(id, kind, detail);
}

void SetLoad(ChildView* v, const char* state, const std::string& url,
             const char* code = nullptr, const std::string& message = "") {
  v->load = state;
  Emit(v->id, LAUFEY_WEBVIEW_EVENT_LOAD,
       laufey_common::WebViewLoadJson(state, url, code, message));
}

// Sends the URL event when the URL or the history edges changed; KVO reports
// URL, canGoBack and canGoForward one by one, so it waits for the burst.
void ScheduleUrlEvent(uint32_t id) {
  ChildView* v = Find(id);
  if (!v || v->url_flush_pending)
    return;
  v->url_flush_pending = true;
  dispatch_async(dispatch_get_main_queue(), ^{
    ChildView* view = Find(id);
    if (!view)
      return;
    view->url_flush_pending = false;
    std::string url = Utf8(view->view.URL.absoluteString);
    bool back = view->view.canGoBack;
    bool forward = view->view.canGoForward;
    if (url == view->sent_url && back == view->sent_back &&
        forward == view->sent_forward)
      return;
    view->sent_url = url;
    view->sent_back = back;
    view->sent_forward = forward;
    Emit(id, LAUFEY_WEBVIEW_EVENT_URL,
         laufey_common::WebViewUrlJson(url, back, forward));
  });
}

void OpenInBrowser(const std::string& url) {
  if (!laufey_common::IsAllowedWebViewUrl(url))
    return;
  if (laufey_common::ExternalOpenCapture::Instance().Capture(url))
    return;
  NSURL* nsurl = [NSURL URLWithString:NS(url)];
  if (nsurl)
    [[NSWorkspace sharedWorkspace] openURL:nsurl];
}

// The engine-neutral error code of a failed load, or nullptr for a load that
// ended without failing (a stop, a navigation replaced by another, a
// download, a navigation this view refused).
const char* LoadErrorCode(NSError* error) {
  if ([error.domain isEqualToString:NSURLErrorDomain]) {
    switch (error.code) {
      case NSURLErrorCancelled:
        return nullptr;
      case NSURLErrorCannotFindHost:
      case NSURLErrorDNSLookupFailed:
        return "dns";
      case NSURLErrorTimedOut:
      case NSURLErrorCannotConnectToHost:
      case NSURLErrorNetworkConnectionLost:
      case NSURLErrorNotConnectedToInternet:
      case NSURLErrorInternationalRoamingOff:
      case NSURLErrorDataNotAllowed:
        return "network";
      case NSURLErrorSecureConnectionFailed:
      case NSURLErrorServerCertificateHasBadDate:
      case NSURLErrorServerCertificateUntrusted:
      case NSURLErrorServerCertificateHasUnknownRoot:
      case NSURLErrorServerCertificateNotYetValid:
      case NSURLErrorClientCertificateRejected:
      case NSURLErrorClientCertificateRequired:
        return "tls";
      case NSURLErrorAppTransportSecurityRequiresSecureConnection:
        return "blocked";
      default:
        return "failed";
    }
  }
  if ([error.domain isEqualToString:@"WebKitErrorDomain"]) {
    // 102: frame load interrupted by a policy change (a refused navigation,
    // a download); 204: plug-in handled the load.
    if (error.code == 102 || error.code == 204)
      return nullptr;
    if (error.code == 103)  // blocked by content filtering / a URL policy
      return "blocked";
  }
  return "failed";
}

// Data stores by partition name: one WKWebsiteDataStore per name, so views
// with the same partition share cookies and storage.
NSMutableDictionary<NSString*, WKWebsiteDataStore*>* Stores() {
  static NSMutableDictionary* stores = [NSMutableDictionary dictionary];
  return stores;
}

bool HasPersistentStores() {
  if (@available(macOS 14.0, *))
    return true;
  return false;
}

WKWebsiteDataStore* StoreFor(const std::string& partition) {
  NSString* key = NS(partition);
  WKWebsiteDataStore* store = Stores()[key];
  if (store)
    return store;
  if (!laufey_common::IsEphemeralPartition(partition)) {
    if (@available(macOS 14.0, *)) {
      NSUUID* uuid = [[NSUUID alloc]
          initWithUUIDString:NS(laufey_common::PartitionUuidString(partition))];
      if (uuid)
        store = [WKWebsiteDataStore dataStoreForIdentifier:uuid];
    }
  }
  if (!store)
    store = [WKWebsiteDataStore nonPersistentDataStore];
  Stores()[key] = store;
  return store;
}

NSRect FrameFor(NSView* parent, const laufey_common::WebViewBounds& b) {
  if (parent.isFlipped)
    return NSMakeRect(b.x, b.y, b.width, b.height);
  return NSMakeRect(b.x, NSHeight(parent.bounds) - b.y - b.height, b.width,
                    b.height);
}

void ApplyFrame(ChildView* v) {
  NSView* parent = v->view.superview;
  if (!parent)
    return;
  v->view.frame = FrameFor(parent, v->bounds);
  // Keep the top-left corner where it is when the window resizes.
  v->view.autoresizingMask =
      NSViewMaxXMargin |
      (parent.isFlipped ? NSViewMaxYMargin : NSViewMinYMargin);
}

void NoteFocus(uint32_t id, bool focused) {
  ChildView* v = Find(id);
  if (!v || v->focused == focused)
    return;
  v->focused = focused;
  Emit(id, LAUFEY_WEBVIEW_EVENT_FOCUS,
       laufey_common::WebViewFocusJson(focused));
}

bool IsAttachment(NSURLResponse* response) {
  if (![response isKindOfClass:[NSHTTPURLResponse class]])
    return false;
  NSDictionary* headers = [(NSHTTPURLResponse*)response allHeaderFields];
  for (NSString* key in headers) {
    if ([key caseInsensitiveCompare:@"Content-Disposition"] == NSOrderedSame) {
      NSString* value = [headers[key] lowercaseString];
      return [value hasPrefix:@"attachment"];
    }
  }
  return false;
}

}  // namespace

@implementation LaufeyChildWebView

- (BOOL)becomeFirstResponder {
  BOOL ok = [super becomeFirstResponder];
  if (ok)
    NoteFocus(self.laufeyViewId, true);
  return ok;
}

- (BOOL)resignFirstResponder {
  BOOL ok = [super resignFirstResponder];
  if (ok)
    NoteFocus(self.laufeyViewId, false);
  return ok;
}

@end

API_AVAILABLE(macos(11.3))
@interface LaufeyChildDownloadDelegate : NSObject <WKDownloadDelegate>
@end

@implementation LaufeyChildDownloadDelegate

- (void)download:(WKDownload*)download
    decideDestinationUsingResponse:(NSURLResponse*)response
                 suggestedFilename:(NSString*)suggestedFilename
                 completionHandler:(void (^)(NSURL*))completionHandler {
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.nameFieldStringValue = suggestedFilename ?: @"download";
  NSWindow* window = download.webView.window;
  void (^done)(NSModalResponse) = ^(NSModalResponse result) {
    completionHandler(result == NSModalResponseOK ? panel.URL : nil);
  };
  if (window)
    [panel beginSheetModalForWindow:window completionHandler:done];
  else
    done([panel runModal]);
}

@end

@interface LaufeyChildWebViewDelegate
    : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler>
@property(nonatomic, assign) uint32_t viewId;
@property(nonatomic, strong) id downloadDelegate;
@end

@implementation LaufeyChildWebViewDelegate

- (void)observeValueForKeyPath:(NSString*)keyPath
                      ofObject:(id)object
                        change:(NSDictionary*)change
                       context:(void*)context {
  ChildView* v = Find(self.viewId);
  if (!v)
    return;
  if ([keyPath isEqualToString:@"title"]) {
    Emit(v->id, LAUFEY_WEBVIEW_EVENT_TITLE,
         laufey_common::WebViewTitleJson(Utf8(v->view.title)));
  } else {
    ScheduleUrlEvent(v->id);
  }
}

// --- WKNavigationDelegate ---

- (void)downloadPolicyFor:(NSURL*)url
                  allowed:(void (^)(void))allowDownload
                   cancel:(void (^)(void))cancel {
  ChildView* v = Find(self.viewId);
  if (!v) {
    cancel();
    return;
  }
  switch (v->options.downloads) {
    case laufey_common::WebViewDownloads::kAsk:
      if (@available(macOS 11.3, *)) {
        allowDownload();
        return;
      }
      cancel();
      return;
    case laufey_common::WebViewDownloads::kBrowser:
      cancel();
      OpenInBrowser(Utf8(url.absoluteString));
      return;
    case laufey_common::WebViewDownloads::kDeny:
      cancel();
      return;
  }
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:
                        (void (^)(WKNavigationActionPolicy))decisionHandler {
  std::string url = Utf8(action.request.URL.absoluteString);
  if (!laufey_common::IsAllowedWebViewNavigation(url)) {
    decisionHandler(WKNavigationActionPolicyCancel);
    return;
  }
  if (@available(macOS 11.3, *)) {
    if (action.shouldPerformDownload) {
      [self downloadPolicyFor:action.request.URL
          allowed:^{
            decisionHandler(WKNavigationActionPolicyDownload);
          }
          cancel:^{
            decisionHandler(WKNavigationActionPolicyCancel);
          }];
      return;
    }
  }
  decisionHandler(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse*)response
                      decisionHandler:(void (^)(WKNavigationResponsePolicy))
                                          decisionHandler {
  if (response.canShowMIMEType && !IsAttachment(response.response)) {
    decisionHandler(WKNavigationResponsePolicyAllow);
    return;
  }
  [self downloadPolicyFor:response.response.URL
      allowed:^{
        if (@available(macOS 11.3, *))
          decisionHandler(WKNavigationResponsePolicyDownload);
        else
          decisionHandler(WKNavigationResponsePolicyCancel);
      }
      cancel:^{
        decisionHandler(WKNavigationResponsePolicyCancel);
      }];
}

- (void)webView:(WKWebView*)webView
     navigationAction:(WKNavigationAction*)navigationAction
    didBecomeDownload:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
  if (!self.downloadDelegate)
    self.downloadDelegate = [[LaufeyChildDownloadDelegate alloc] init];
  download.delegate = self.downloadDelegate;
}

- (void)webView:(WKWebView*)webView
    navigationResponse:(WKNavigationResponse*)navigationResponse
     didBecomeDownload:(WKDownload*)download API_AVAILABLE(macos(11.3)) {
  if (!self.downloadDelegate)
    self.downloadDelegate = [[LaufeyChildDownloadDelegate alloc] init];
  download.delegate = self.downloadDelegate;
}

- (void)webView:(WKWebView*)webView
    didStartProvisionalNavigation:(WKNavigation*)navigation {
  if (ChildView* v = Find(self.viewId))
    SetLoad(v, "loading", Utf8(webView.URL.absoluteString));
}

- (void)webView:(WKWebView*)webView didCommitNavigation:(WKNavigation*)nav {
  if (ChildView* v = Find(self.viewId))
    SetLoad(v, "interactive", Utf8(webView.URL.absoluteString));
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)nav {
  if (ChildView* v = Find(self.viewId))
    SetLoad(v, "complete", Utf8(webView.URL.absoluteString));
}

- (void)failed:(WKWebView*)webView error:(NSError*)error {
  ChildView* v = Find(self.viewId);
  if (!v)
    return;
  const char* code = LoadErrorCode(error);
  if (!code) {
    // Stopped, replaced or refused: no "failed"; the view is not loading
    // this navigation any more.
    if (!webView.isLoading)
      v->load = "complete";
    return;
  }
  NSString* failing = error.userInfo[NSURLErrorFailingURLStringErrorKey];
  std::string url = failing ? Utf8(failing) : Utf8(webView.URL.absoluteString);
  SetLoad(v, "failed", url, code, Utf8(error.localizedDescription));
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
  [self failed:webView error:error];
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
            withError:(NSError*)error {
  [self failed:webView error:error];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView {
  if (ChildView* v = Find(self.viewId))
    SetLoad(v, "failed", Utf8(webView.URL.absoluteString), "failed",
            "the page's web content process ended");
}

// --- WKUIDelegate ---

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features {
  ChildView* v = Find(self.viewId);
  if (!v)
    return nil;
  std::string url = Utf8(action.request.URL.absoluteString);
  if (url.empty())
    url = "about:blank";
  bool popup = features.width != nil || features.height != nil ||
               features.x != nil || features.y != nil;
  switch (v->options.popups) {
    case laufey_common::WebViewPopups::kSameView:
      if (laufey_common::IsAllowedWebViewUrl(url))
        [webView loadRequest:action.request];
      break;
    case laufey_common::WebViewPopups::kBrowser:
      OpenInBrowser(url);
      break;
    case laufey_common::WebViewPopups::kDeny:
      Emit(v->id, LAUFEY_WEBVIEW_EVENT_NEW_WINDOW,
           laufey_common::WebViewNewWindowJson(url, popup));
      break;
  }
  return nil;  // no second web view: the page's window.open() gets null
}

- (void)webView:(WKWebView*)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin*)origin
                          initiatedByFrame:(WKFrameInfo*)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:
                               (void (^)(WKPermissionDecision))decisionHandler
    API_AVAILABLE(macos(12.0)) {
  ChildView* v = Find(self.viewId);
  bool ask =
      v && v->options.permissions == laufey_common::WebViewPermissions::kAsk;
  decisionHandler(ask ? WKPermissionDecisionPrompt : WKPermissionDecisionDeny);
}

- (void)alertFor:(WKWebView*)webView
         message:(NSString*)message
         buttons:(NSArray<NSString*>*)buttons
           input:(NSString*)input
            done:(void (^)(NSModalResponse, NSString*))done {
  NSAlert* alert = [[NSAlert alloc] init];
  alert.messageText = webView.URL.host ?: @"";
  alert.informativeText = message ?: @"";
  for (NSString* b in buttons)
    [alert addButtonWithTitle:b];
  NSTextField* field = nil;
  if (input) {
    field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
    field.stringValue = input;
    alert.accessoryView = field;
  }
  NSWindow* window = webView.window;
  if (!window) {
    NSModalResponse r = [alert runModal];
    done(r, field.stringValue);
    return;
  }
  [alert beginSheetModalForWindow:window
                completionHandler:^(NSModalResponse r) {
                  done(r, field.stringValue);
                }];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))completionHandler {
  [self alertFor:webView
         message:message
         buttons:@[ @"OK" ]
           input:nil
            done:^(NSModalResponse, NSString*) {
              completionHandler();
            }];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:(void (^)(BOOL))completionHandler {
  [self alertFor:webView
         message:message
         buttons:@[ @"OK", @"Cancel" ]
           input:nil
            done:^(NSModalResponse r, NSString*) {
              completionHandler(r == NSAlertFirstButtonReturn);
            }];
}

- (void)webView:(WKWebView*)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
                              defaultText:(NSString*)defaultText
                         initiatedByFrame:(WKFrameInfo*)frame
                        completionHandler:
                            (void (^)(NSString*))completionHandler {
  [self alertFor:webView
         message:prompt
         buttons:@[ @"OK", @"Cancel" ]
           input:defaultText ?: @""
            done:^(NSModalResponse r, NSString* text) {
              completionHandler(r == NSAlertFirstButtonReturn ? text : nil);
            }];
}

// --- Console shim ---

- (void)userContentController:(WKUserContentController*)controller
      didReceiveScriptMessage:(WKScriptMessage*)message {
  ChildView* v = Find(self.viewId);
  if (!v || !message.frameInfo.isMainFrame ||
      ![message.body isKindOfClass:[NSDictionary class]])
    return;
  int64_t second = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  if (second != v->console_second) {
    v->console_second = second;
    v->console_count = 0;
  }
  if (++v->console_count > kConsolePerSecond)
    return;
  NSDictionary* body = message.body;
  auto str = [&](NSString* key) {
    id value = body[key];
    return [value isKindOfClass:[NSString class]] ? Utf8(value) : std::string();
  };
  std::string level = str(@"level");
  std::string text = str(@"message");
  if (text.size() > kConsoleMaxBytes)
    text.resize(kConsoleMaxBytes);
  id line = body[@"line"];
  int line_number = [line isKindOfClass:[NSNumber class]] ? [line intValue] : 0;
  Emit(v->id, LAUFEY_WEBVIEW_EVENT_CONSOLE,
       laufey_common::WebViewConsoleJson(level.c_str(), text, str(@"source"),
                                         line_number));
}

@end

namespace laufey_child_webview_mac {

uint32_t Capabilities() {
  uint32_t caps =
      LAUFEY_WEBVIEW_CAP_SUPPORTED | LAUFEY_WEBVIEW_CAP_CAPTURE |
      (LAUFEY_WEBVIEW_ENGINE_WKWEBVIEW << LAUFEY_WEBVIEW_ENGINE_SHIFT);
  if (@available(macOS 11.0, *))
    caps |= LAUFEY_WEBVIEW_CAP_EVALUATE;
  if (HasPersistentStores())
    caps |= LAUFEY_WEBVIEW_CAP_PERSISTENT_PARTITIONS;
  if (@available(macOS 12.0, *))
    caps |= LAUFEY_WEBVIEW_CAP_PERMISSION_PROMPTS;
  return caps;
}

uint32_t Create(uint32_t window_id, void* page_view_ptr,
                const WebViewOptions& options, laufey_webview_event_fn on_event,
                void* user_data, int32_t* status_out) {
  NSView* page_view = (__bridge NSView*)page_view_ptr;
  if (!page_view) {
    *status_out = LAUFEY_WEBVIEW_NO_WINDOW;
    return 0;
  }
  uint32_t id = WebViewSinks::Instance().Add(window_id, on_event, user_data);
  if (!id) {
    *status_out = LAUFEY_WEBVIEW_LIMIT;
    return 0;
  }
  @autoreleasepool {
    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    config.websiteDataStore = StoreFor(options.partition);
    LaufeyChildWebViewDelegate* delegate =
        [[LaufeyChildWebViewDelegate alloc] init];
    delegate.viewId = id;
    WKUserScript* shim = [[WKUserScript alloc]
          initWithSource:kConsoleShim
           injectionTime:WKUserScriptInjectionTimeAtDocumentStart
        forMainFrameOnly:YES];
    [config.userContentController addUserScript:shim];
    [config.userContentController addScriptMessageHandler:delegate
                                                     name:kConsoleHandler];

    LaufeyChildWebView* view =
        [[LaufeyChildWebView alloc] initWithFrame:NSZeroRect
                                    configuration:config];
    view.laufeyViewId = id;
    view.navigationDelegate = delegate;
    view.UIDelegate = delegate;
    if (!options.user_agent.empty())
      view.customUserAgent = NS(options.user_agent);
    // Web Inspector follows the app's own page (LAUFEY_INSPECTABLE).
    if (@available(macOS 13.3, *))
      view.inspectable = laufey_common::LaunchInspectable();
    for (NSString* key in @[ @"title", @"URL", @"canGoBack", @"canGoForward" ])
      [view addObserver:delegate forKeyPath:key options:0 context:nullptr];

    ChildView& v = Views()[id];
    v.id = id;
    v.window_id = window_id;
    v.view = view;
    v.delegate = delegate;
    v.options = options;
    v.bounds = options.bounds;
    v.visible = options.visible;
    [page_view addSubview:view];
    ApplyFrame(&v);
    view.hidden = !options.visible;

    NSURL* url = [NSURL URLWithString:NS(options.url)];
    if (url)
      [view loadRequest:[NSURLRequest requestWithURL:url]];
  }
  *status_out = LAUFEY_WEBVIEW_OK;
  return id;
}

bool SetBounds(uint32_t id, double x, double y, double width, double height) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  v->bounds = {x, y, width, height};
  ApplyFrame(v);
  return true;
}

bool SetVisible(uint32_t id, bool visible) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  v->visible = visible;
  if (!visible && v->view.window.firstResponder == v->view)
    [v->view.window makeFirstResponder:v->view.superview];
  v->view.hidden = !visible;
  return true;
}

int32_t Navigate(uint32_t id, const std::string& url) {
  ChildView* v = Find(id);
  if (!v)
    return LAUFEY_WEBVIEW_CLOSED;
  NSURL* nsurl = [NSURL URLWithString:NS(url)];
  if (!nsurl)
    return LAUFEY_WEBVIEW_NOT_ALLOWED_URL;
  [v->view loadRequest:[NSURLRequest requestWithURL:nsurl]];
  return LAUFEY_WEBVIEW_OK;
}

bool Command(uint32_t id, int32_t command) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  switch (command) {
    case LAUFEY_WEBVIEW_COMMAND_BACK:
      if (v->view.canGoBack)
        [v->view goBack];
      return true;
    case LAUFEY_WEBVIEW_COMMAND_FORWARD:
      if (v->view.canGoForward)
        [v->view goForward];
      return true;
    case LAUFEY_WEBVIEW_COMMAND_RELOAD:
      [v->view reload];
      return true;
    case LAUFEY_WEBVIEW_COMMAND_RELOAD_IGNORE_CACHE:
      [v->view reloadFromOrigin];
      return true;
    case LAUFEY_WEBVIEW_COMMAND_STOP:
      [v->view stopLoading];
      return true;
    case LAUFEY_WEBVIEW_COMMAND_FOCUS:
      if (!v->visible || !v->view.window)
        return false;
      return [v->view.window makeFirstResponder:v->view];
  }
  return false;
}

std::string State(uint32_t id) {
  ChildView* v = Find(id);
  if (!v)
    return "";
  laufey_common::WebViewStateFields s;
  s.url = Utf8(v->view.URL.absoluteString);
  s.title = Utf8(v->view.title);
  s.load = v->load;
  s.can_go_back = v->view.canGoBack;
  s.can_go_forward = v->view.canGoForward;
  s.visible = v->visible;
  s.bounds = v->bounds;
  return laufey_common::WebViewStateJson(s);
}

void Evaluate(uint32_t view_id, const std::string& script,
              laufey_webview_eval_fn callback, void* user_data) {
  ChildView* v = Find(view_id);
  if (!v) {
    callback(user_data, LAUFEY_WEBVIEW_CLOSED, "the web view is closed");
    return;
  }
  if (@available(macOS 11.0, *)) {
    NSString* body = NS(laufey_common::WebViewEvalScript(script));
    WKContentWorld* world = [WKContentWorld worldWithName:@"laufey-webview"];
    [v->view
        callAsyncJavaScript:body
                  arguments:nil
                    inFrame:nil
             inContentWorld:world
          completionHandler:^(id result, NSError* error) {
            if (error || ![result isKindOfClass:[NSString class]]) {
              std::string message =
                  error ? Utf8(error.localizedDescription)
                        : std::string("the script's result could not be "
                                      "read");
              callback(user_data, LAUFEY_WEBVIEW_SCRIPT_ERROR, message.c_str());
              return;
            }
            std::string text;
            int32_t status =
                laufey_common::ParseWebViewEvalEnvelope(Utf8(result), &text);
            callback(user_data, status, text.c_str());
          }];
    return;
  }
  callback(user_data, LAUFEY_WEBVIEW_UNSUPPORTED,
           "evaluate needs macOS 11 or later");
}

void Capture(uint32_t id, int32_t format, int32_t quality,
             laufey_webview_capture_fn callback, void* user_data) {
  ChildView* v = Find(id);
  if (!v) {
    callback(user_data, LAUFEY_WEBVIEW_CLOSED, nullptr, 0,
             "the web view is closed");
    return;
  }
  WKSnapshotConfiguration* config = [[WKSnapshotConfiguration alloc] init];
  [v->view
      takeSnapshotWithConfiguration:config
                  completionHandler:^(NSImage* image, NSError* error) {
                    if (!image) {
                      std::string message =
                          error ? Utf8(error.localizedDescription)
                                : std::string("nothing to capture");
                      callback(user_data, LAUFEY_WEBVIEW_FAILED, nullptr, 0,
                               message.c_str());
                      return;
                    }
                    CGImageRef cg = [image CGImageForProposedRect:nullptr
                                                          context:nil
                                                            hints:nil];
                    NSData* data = nil;
                    if (cg) {
                      NSBitmapImageRep* rep =
                          [[NSBitmapImageRep alloc] initWithCGImage:cg];
                      if (format == LAUFEY_WEBVIEW_CAPTURE_JPEG) {
                        data = [rep
                            representationUsingType:NSBitmapImageFileTypeJPEG
                                         properties:@{
                                           NSImageCompressionFactor :
                                               @(quality / 100.0)
                                         }];
                      } else {
                        data = [rep
                            representationUsingType:NSBitmapImageFileTypePNG
                                         properties:@{}];
                      }
                    }
                    if (!data) {
                      callback(user_data, LAUFEY_WEBVIEW_FAILED, nullptr, 0,
                               "the image could not be encoded");
                      return;
                    }
                    callback(user_data, LAUFEY_WEBVIEW_OK,
                             static_cast<const uint8_t*>(data.bytes),
                             data.length, nullptr);
                  }];
}

bool Close(uint32_t id) {
  auto it = Views().find(id);
  if (it == Views().end())
    return false;
  ChildView v = it->second;
  Views().erase(it);
  @autoreleasepool {
    for (NSString* key in @[ @"title", @"URL", @"canGoBack", @"canGoForward" ])
      [v.view removeObserver:v.delegate forKeyPath:key];
    [v.view.configuration.userContentController
        removeScriptMessageHandlerForName:kConsoleHandler];
    [v.view stopLoading];
    v.view.navigationDelegate = nil;
    v.view.UIDelegate = nil;
    if (v.view.window.firstResponder == v.view)
      [v.view.window makeFirstResponder:v.view.superview];
    [v.view removeFromSuperview];
  }
  WebViewSinks::Instance().Close(id);
  return true;
}

void CloseAllForWindow(uint32_t window_id) {
  std::vector<uint32_t> ids;
  for (const auto& kv : Views()) {
    if (kv.second.window_id == window_id)
      ids.push_back(kv.first);
  }
  for (uint32_t id : ids)
    Close(id);
}

}  // namespace laufey_child_webview_mac
