// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The share sheet's request parsing (laufey_share.h); the OS halves are in
// share_mac.mm and share_win.cc.

#include "laufey_share.h"

#include "json_reader.h"

namespace laufey_common {

using json::JsonReader;
using json::JsonValue;

namespace {

bool IsAbsolutePathString(const std::string& p) {
  if (p.empty() || p.find('\0') != std::string::npos)
    return false;
#if defined(_WIN32)
  // C:\x, C:/x, \\server\share.
  if (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') ||
                        (p[0] >= 'a' && p[0] <= 'z')) &&
      p[1] == ':' && (p[2] == '\\' || p[2] == '/'))
    return true;
  return p.size() >= 3 && p[0] == '\\' && p[1] == '\\';
#else
  return p[0] == '/';
#endif
}

bool HasScheme(const std::string& url) {
  size_t colon = url.find(':');
  if (colon == std::string::npos || colon == 0)
    return false;
  for (size_t i = 0; i < colon; ++i) {
    char c = url[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' ||
                         c == '.'));
    if (!ok)
      return false;
  }
  for (unsigned char c : url) {
    if (c < 0x20 || c == 0x7f)
      return false;
  }
  return colon + 1 < url.size();
}

}  // namespace

bool ParseShareRequest(const std::string& json, ShareRequest* out,
                       std::string* error) {
  *out = ShareRequest();
  JsonValue root;
  JsonReader reader(json);
  if (!reader.ParseDocument(&root, error))
    return false;
  if (root.type != JsonValue::Type::kObject) {
    *error = "the share request must be an object";
    return false;
  }
  bool has_x = false, has_y = false;
  for (const auto& [key, value] : root.object) {
    if (key == "title" || key == "text" || key == "url") {
      if (value.type != JsonValue::Type::kString) {
        *error = "\"" + key + "\" must be a string";
        return false;
      }
      std::string& field =
          key == "title" ? out->title : key == "text" ? out->text : out->url;
      field = value.string;
    } else if (key == "files") {
      if (value.type != JsonValue::Type::kArray) {
        *error = "\"files\" must be an array of absolute paths";
        return false;
      }
      for (const JsonValue& item : value.array) {
        if (item.type != JsonValue::Type::kString ||
            !IsAbsolutePathString(item.string)) {
          *error = "\"files\" must be an array of absolute paths";
          return false;
        }
        out->files.push_back(item.string);
      }
    } else if (key == "x" || key == "y") {
      if (value.type != JsonValue::Type::kNumber || !value.is_integer) {
        *error = "\"" + key + "\" must be an integer (DIPs)";
        return false;
      }
      (key == "x" ? out->x : out->y) = static_cast<double>(value.integer);
      (key == "x" ? has_x : has_y) = true;
    }
    // Unknown keys are ignored (a newer runtime may send more).
  }
  if (has_x != has_y) {
    *error = "\"x\" and \"y\" go together";
    return false;
  }
  out->has_point = has_x;
  if (out->text.empty() && out->url.empty() && out->files.empty()) {
    *error = "nothing to share: give text, a url or files";
    return false;
  }
  if (!out->url.empty() && !HasScheme(out->url)) {
    *error = "\"url\" must be an absolute URL";
    return false;
  }
  return true;
}

void ShareReport(laufey_share_fn callback, void* user_data, int status,
                 const std::string& detail) {
  if (callback)
    callback(user_data, status, detail.empty() ? nullptr : detail.c_str());
}

}  // namespace laufey_common
