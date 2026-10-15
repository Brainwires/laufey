// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// User activities (laufey_user_activity.h): parsing, the continued-activity
// dispatch, and the entry point where the OS has none.

#include "laufey_user_activity.h"

#include <deque>
#include <mutex>

#include "json_reader.h"
#include "laufey_backend_common.h"

namespace laufey_common {

using json::JsonReader;
using json::JsonValue;

namespace {

bool ValidType(const std::string& type) {
  if (type.empty() || type.size() > 255 || type.find('.') == std::string::npos)
    return false;
  for (char c : type) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!ok)
      return false;
  }
  return type.front() != '.' && type.back() != '.';
}

bool HttpUrl(const std::string& url) {
  for (unsigned char c : url) {
    if (c <= 0x20 || c == 0x7f)
      return false;
  }
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

std::string Quote(const std::string& s) {
  static const char kHex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c < 0x20) {
      out += "\\u00";
      out += kHex[c >> 4];
      out += kHex[c & 15];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out + "\"";
}

std::mutex g_mutex;
laufey_user_activity_fn g_handler = nullptr;
void* g_user_data = nullptr;
std::deque<std::string> g_pending;
constexpr size_t kMaxPending = 16;

}  // namespace

bool ParseUserActivity(const std::string& text, UserActivity* out,
                       std::string* error) {
  *out = UserActivity();
  JsonValue root;
  JsonReader reader(text);
  if (!reader.ParseDocument(&root, error))
    return false;
  if (root.type != JsonValue::Type::kObject) {
    *error = "the activity must be an object";
    return false;
  }
  for (const auto& [key, value] : root.object) {
    if (key == "type" || key == "title" || key == "url") {
      if (value.type != JsonValue::Type::kString) {
        *error = "\"" + key + "\" must be a string";
        return false;
      }
      (key == "type" ? out->type : key == "title" ? out->title : out->url) =
          value.string;
    } else if (key == "handoff" || key == "searchable") {
      if (value.type != JsonValue::Type::kBool) {
        *error = "\"" + key + "\" must be a boolean";
        return false;
      }
      (key == "handoff" ? out->handoff : out->searchable) = value.boolean;
    } else if (key == "userInfo") {
      if (value.type != JsonValue::Type::kObject) {
        *error = "\"userInfo\" must be an object of strings";
        return false;
      }
      for (const auto& [k, v] : value.object) {
        if (v.type != JsonValue::Type::kString) {
          *error = "\"userInfo\" must be an object of strings";
          return false;
        }
        out->user_info[k] = v.string;
      }
    } else if (key == "keywords") {
      if (value.type != JsonValue::Type::kArray) {
        *error = "\"keywords\" must be an array of strings";
        return false;
      }
      for (const JsonValue& k : value.array) {
        if (k.type != JsonValue::Type::kString) {
          *error = "\"keywords\" must be an array of strings";
          return false;
        }
        out->keywords.push_back(k.string);
      }
    }
  }
  if (!ValidType(out->type)) {
    *error = "\"type\" must be a reverse-DNS activity type";
    return false;
  }
  if (!out->url.empty() && !HttpUrl(out->url)) {
    *error = "\"url\" must be an http(s) URL";
    return false;
  }
  return true;
}

std::string UserActivityJson(const UserActivity& a) {
  std::string out = "{\"type\":" + Quote(a.type) + ",\"title\":" +
                    Quote(a.title) + ",\"url\":" +
                    (a.url.empty() ? std::string("null") : Quote(a.url)) +
                    ",\"userInfo\":{";
  bool first = true;
  for (const auto& [k, v] : a.user_info) {
    if (!first)
      out += ",";
    first = false;
    out += Quote(k) + ":" + Quote(v);
  }
  return out + "}}";
}

void DispatchContinuedUserActivity(const UserActivity& activity) {
  std::string json = UserActivityJson(activity);
  laufey_user_activity_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    handler = g_handler;
    user_data = g_user_data;
    if (!handler) {
      if (g_pending.size() >= kMaxPending)
        g_pending.pop_front();
      g_pending.push_back(json);
      return;
    }
  }
  handler(user_data, json.c_str());
}

void SetUserActivityHandler(laufey_user_activity_fn handler, void* user_data) {
  std::deque<std::string> pending;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handler = handler;
    g_user_data = user_data;
    if (handler)
      pending.swap(g_pending);
  }
  for (const std::string& json : pending)
    handler(user_data, json.c_str());
}

#if !defined(__APPLE__)
bool SetUserActivityForAbi(const char* /*activity_json*/) {
  return false;
}
#endif

}  // namespace laufey_common
