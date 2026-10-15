// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// User activities (API 49): Handoff and Spotlight through NSUserActivity on
// macOS. See laufey.h `set_user_activity` and docs/user-activity.md.

#ifndef LAUFEY_USER_ACTIVITY_H_
#define LAUFEY_USER_ACTIVITY_H_

#include <map>
#include <string>
#include <vector>

#include "laufey.h"

namespace laufey_common {

struct UserActivity {
  std::string type;
  std::string title;
  std::string url;
  std::map<std::string, std::string> user_info;
  std::vector<std::string> keywords;
  bool handoff = true;
  bool searchable = false;
};

// Parses and checks an activity: "type" a reverse-DNS string of letters,
// digits, '.', '-' and '_' (at most 255 bytes) with a dot; "url" empty or
// http(s); "userInfo" an object of strings; "keywords" strings; "handoff"
// and "searchable" booleans. False with `error` otherwise.
bool ParseUserActivity(const std::string& json, UserActivity* out,
                       std::string* error);

// The JSON a continued activity is reported with.
std::string UserActivityJson(const UserActivity& activity);

// Delivers a continued activity to the handler, or holds it (at most 16)
// until one is registered. Any thread.
void DispatchContinuedUserActivity(const UserActivity& activity);
void SetUserActivityHandler(laufey_user_activity_fn handler, void* user_data);

// The C ABI entry: false where there are no user activities.
bool SetUserActivityForAbi(const char* activity_json);

}  // namespace laufey_common

#if defined(__APPLE__) && defined(__OBJC__)
@class NSUserActivity;
// For the app delegates' application:continueUserActivity:
// restorationHandler: (macOS).
void LaufeyContinueUserActivityMac(NSUserActivity* activity);
#endif

#endif  // LAUFEY_USER_ACTIVITY_H_
