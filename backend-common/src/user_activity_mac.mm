// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// User activities on macOS (laufey_user_activity.h): the app's current
// NSUserActivity (Handoff, Spotlight), kept alive while current.

#include "laufey_user_activity.h"

#import <Foundation/Foundation.h>

#include "laufey_ui_tasks.h"

namespace laufey_common {

namespace {
// The current activity (AppKit holds it weakly once current).
NSUserActivity* g_current = nil;

NSString* NS(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}
}  // namespace

bool SetUserActivityForAbi(const char* activity_json) {
  if (!activity_json) {
    RunOnMainSync(^{
      [g_current invalidate];
      g_current = nil;
    });
    return true;
  }
  UserActivity a;
  std::string error;
  if (!ParseUserActivity(activity_json, &a, &error))
    return false;
  __block bool ok = false;
  RunOnMainSync(^{
    NSUserActivity* activity =
        [[NSUserActivity alloc] initWithActivityType:NS(a.type)];
    activity.title = NS(a.title);
    if (!a.url.empty())
      activity.webpageURL = [NSURL URLWithString:NS(a.url)];
    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    for (const auto& [k, v] : a.user_info)
      info[NS(k)] = NS(v);
    activity.userInfo = info;
    activity.requiredUserInfoKeys = [NSSet setWithArray:info.allKeys];
    NSMutableSet* keywords = [NSMutableSet set];
    for (const std::string& k : a.keywords)
      [keywords addObject:NS(k)];
    activity.keywords = keywords;
    activity.eligibleForHandoff = a.handoff;
    activity.eligibleForSearch = a.searchable;
    [g_current invalidate];
    g_current = activity;
    [activity becomeCurrent];
    ok = true;
  });
  return ok;
}

}  // namespace laufey_common

// Called by the app delegates (application:continueUserActivity:...).
void LaufeyContinueUserActivityMac(NSUserActivity* activity) {
  laufey_common::UserActivity a;
  a.type = activity.activityType.UTF8String ?: "";
  a.title = activity.title.UTF8String ?: "";
  if (activity.webpageURL.absoluteString)
    a.url = activity.webpageURL.absoluteString.UTF8String;
  for (id key in activity.userInfo) {
    id value = activity.userInfo[key];
    if ([key isKindOfClass:[NSString class]] &&
        [value isKindOfClass:[NSString class]])
      a.user_info[[key UTF8String]] = [value UTF8String];
  }
  laufey_common::DispatchContinuedUserActivity(a);
}
