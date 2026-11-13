// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views (API 58): the shared rules (laufey_webview.h).

#include "laufey_webview.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>

#include "json_reader.h"
#include "laufey_external_links.h"

namespace laufey_common {

namespace {

using json::JsonReader;
using json::JsonValue;

const JsonValue* Field(const JsonValue& object, const char* key) {
  for (const auto& kv : object.object) {
    if (kv.first == key)
      return &kv.second;
  }
  return nullptr;
}

bool ReadNumber(const JsonValue& object, const char* key, double* out,
                std::string* error) {
  const JsonValue* v = Field(object, key);
  if (!v || v->type != JsonValue::Type::kNumber) {
    *error = std::string("\"") + key + "\" must be a number";
    return false;
  }
  *out = v->number;
  return true;
}

// An optional string field: absent leaves `out` alone.
bool ReadOptionalString(const JsonValue& object, const char* key,
                        std::string* out, bool* present, std::string* error) {
  const JsonValue* v = Field(object, key);
  *present = v != nullptr && v->type != JsonValue::Type::kNull;
  if (!*present)
    return true;
  if (v->type != JsonValue::Type::kString) {
    *error = std::string("\"") + key + "\" must be a string";
    return false;
  }
  *out = v->string;
  return true;
}

bool StartsWith(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// A JSON string literal (also a valid JS string literal: U+2028 / U+2029
// are escaped too).
std::string JsonQuote(const std::string& s) {
  std::string out = "\"";
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20 || c == 0x7f) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else if (c == 0xe2 && i + 2 < s.size() &&
                   static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) == 0xa8 ||
                    static_cast<unsigned char>(s[i + 2]) == 0xa9)) {
          out += static_cast<unsigned char>(s[i + 2]) == 0xa8 ? "\\u2028"
                                                              : "\\u2029";
          i += 2;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}

uint64_t Fnv1a64(const std::string& s, uint64_t seed) {
  uint64_t h = seed;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  return h;
}

#ifdef __APPLE__
constexpr bool kForMac = true;
#else
constexpr bool kForMac = false;
#endif

// "forwardKeys" (an array of accelerator strings) into canonical forms.
bool ReadForwardKeys(const JsonValue& value, std::vector<std::string>* out,
                     std::string* error) {
  if (value.type != JsonValue::Type::kArray) {
    *error = "\"forwardKeys\" must be an array of strings";
    return false;
  }
  if (value.array.size() > kMaxWebViewForwardKeys) {
    *error = "\"forwardKeys\" must have at most " +
             std::to_string(kMaxWebViewForwardKeys) + " entries";
    return false;
  }
  std::vector<std::string> keys;
  for (const JsonValue& entry : value.array) {
    if (entry.type != JsonValue::Type::kString) {
      *error = "\"forwardKeys\" must be an array of strings";
      return false;
    }
    if (entry.string.size() > kMaxWebViewForwardKeyLength) {
      *error = "a \"forwardKeys\" entry must be at most " +
               std::to_string(kMaxWebViewForwardKeyLength) + " characters";
      return false;
    }
    Accelerator accel;
    std::string why;
    if (!ParseAccelerator(entry.string, kForMac, &accel, &why)) {
      *error = "\"forwardKeys\": " + why;
      return false;
    }
    std::string canonical = CanonicalAccelerator(accel);
    if (std::find(keys.begin(), keys.end(), canonical) == keys.end())
      keys.push_back(std::move(canonical));
  }
  *out = std::move(keys);
  return true;
}

// A DOM `code` as an accelerator key (KeyKind / NamedKey).
bool KeyFromCode(const std::string& code, Accelerator* a) {
  if (code.size() == 4 && StartsWith(code, "Key") && code[3] >= 'A' &&
      code[3] <= 'Z') {
    a->kind = KeyKind::kLetter;
    a->ch = code[3];
    return true;
  }
  if (code.size() == 6 && StartsWith(code, "Digit") && code[5] >= '0' &&
      code[5] <= '9') {
    a->kind = KeyKind::kDigit;
    a->ch = code[5];
    return true;
  }
  if (code.size() == 7 && StartsWith(code, "Numpad") && code[6] >= '0' &&
      code[6] <= '9') {
    a->kind = KeyKind::kNumpad;
    a->number = code[6] - '0';
    a->named = NamedKey::kNone;
    return true;
  }
  if (code.size() >= 2 && code.size() <= 3 && code[0] == 'F' &&
      code[1] >= '1' && code[1] <= '9') {
    int n = code[1] - '0';
    if (code.size() == 3) {
      if (code[2] < '0' || code[2] > '9')
        return false;
      n = n * 10 + (code[2] - '0');
    }
    if (n > 24)
      return false;
    a->kind = KeyKind::kFunction;
    a->number = n;
    return true;
  }
  static const struct {
    const char* code;
    char ch;
  } kPunct[] = {
      {"Minus", '-'},        {"Equal", '='},      {"BracketLeft", '['},
      {"BracketRight", ']'}, {"Backslash", '\\'}, {"Semicolon", ';'},
      {"Quote", '\''},       {"Comma", ','},      {"Period", '.'},
      {"Slash", '/'},        {"Backquote", '`'}};
  for (const auto& p : kPunct) {
    if (code == p.code) {
      a->kind = KeyKind::kPunct;
      a->ch = p.ch;
      return true;
    }
  }
  static const struct {
    const char* code;
    KeyKind kind;
    NamedKey key;
  } kNamed[] = {
      {"Space", KeyKind::kNamed, NamedKey::kSpace},
      {"Tab", KeyKind::kNamed, NamedKey::kTab},
      {"Backspace", KeyKind::kNamed, NamedKey::kBackspace},
      {"Delete", KeyKind::kNamed, NamedKey::kDelete},
      {"Insert", KeyKind::kNamed, NamedKey::kInsert},
      {"Enter", KeyKind::kNamed, NamedKey::kEnter},
      {"NumpadEnter", KeyKind::kNamed, NamedKey::kEnter},
      {"Escape", KeyKind::kNamed, NamedKey::kEscape},
      {"ArrowUp", KeyKind::kNamed, NamedKey::kUp},
      {"ArrowDown", KeyKind::kNamed, NamedKey::kDown},
      {"ArrowLeft", KeyKind::kNamed, NamedKey::kLeft},
      {"ArrowRight", KeyKind::kNamed, NamedKey::kRight},
      {"Home", KeyKind::kNamed, NamedKey::kHome},
      {"End", KeyKind::kNamed, NamedKey::kEnd},
      {"PageUp", KeyKind::kNamed, NamedKey::kPageUp},
      {"PageDown", KeyKind::kNamed, NamedKey::kPageDown},
      {"PrintScreen", KeyKind::kNamed, NamedKey::kPrintScreen},
      {"MediaPlayPause", KeyKind::kNamed, NamedKey::kMediaPlayPause},
      {"MediaTrackNext", KeyKind::kNamed, NamedKey::kMediaNextTrack},
      {"MediaTrackPrevious", KeyKind::kNamed, NamedKey::kMediaPreviousTrack},
      {"MediaStop", KeyKind::kNamed, NamedKey::kMediaStop},
      {"AudioVolumeUp", KeyKind::kNamed, NamedKey::kVolumeUp},
      {"AudioVolumeDown", KeyKind::kNamed, NamedKey::kVolumeDown},
      {"AudioVolumeMute", KeyKind::kNamed, NamedKey::kVolumeMute},
      {"NumpadDecimal", KeyKind::kNumpad, NamedKey::kNumDecimal},
      {"NumpadAdd", KeyKind::kNumpad, NamedKey::kNumAdd},
      {"NumpadSubtract", KeyKind::kNumpad, NamedKey::kNumSubtract},
      {"NumpadMultiply", KeyKind::kNumpad, NamedKey::kNumMultiply},
      {"NumpadDivide", KeyKind::kNumpad, NamedKey::kNumDivide},
  };
  for (const auto& n : kNamed) {
    if (code == n.code) {
      a->kind = n.kind;
      a->named = n.key;
      return true;
    }
  }
  return false;
}

}  // namespace

bool IsValidWebViewBounds(double x, double y, double width, double height) {
  const double kMax = 1e6;
  for (double v : {x, y, width, height}) {
    if (!std::isfinite(v) || v < -kMax || v > kMax)
      return false;
  }
  return width >= 0 && height >= 0;
}

bool IsAllowedWebViewUrl(const std::string& url) {
  return IsAllowedExternalLinkUrl(url);
}

bool IsAllowedWebViewNavigation(const std::string& url) {
  if (IsAllowedWebViewUrl(url))
    return true;
  std::string lower = Lower(url);
  return lower == "about:blank" || lower == "about:srcdoc";
}

bool IsValidPartitionName(const std::string& name) {
  if (name.empty() || name.size() > 64)
    return false;
  auto alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
  };
  if (!alnum(name[0]))
    return false;
  for (char c : name) {
    if (!alnum(c) && c != '.' && c != '_' && c != ':' && c != '-')
      return false;
  }
  return true;
}

bool IsEphemeralPartition(const std::string& name) {
  return name == "ephemeral" || StartsWith(name, "ephemeral:");
}

std::string PartitionDirName(const std::string& name) {
  std::string out = name;
  for (char& c : out) {
    if (c == ':')
      c = '~';
  }
  return out;
}

std::array<uint8_t, 16> PartitionUuid(const std::string& name) {
  const std::string key = "laufey-webview-partition:" + name;
  uint64_t hi = Fnv1a64(key, 0xcbf29ce484222325ULL);
  uint64_t lo = Fnv1a64(key, 0x84222325cbf29ce4ULL ^ hi);
  std::array<uint8_t, 16> out{};
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>(hi >> (56 - 8 * i));
    out[8 + i] = static_cast<uint8_t>(lo >> (56 - 8 * i));
  }
  out[6] = static_cast<uint8_t>((out[6] & 0x0f) | 0x50);  // version 5
  out[8] = static_cast<uint8_t>((out[8] & 0x3f) | 0x80);  // RFC 4122 variant
  return out;
}

std::string PartitionUuidString(const std::string& name) {
  std::array<uint8_t, 16> b = PartitionUuid(name);
  char buf[37];
  std::snprintf(buf, sizeof(buf),
                "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X"
                "%02X%02X",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                b[10], b[11], b[12], b[13], b[14], b[15]);
  return buf;
}

int32_t ParseWebViewOptions(const std::string& text, WebViewOptions* out,
                            std::string* error) {
  JsonValue root;
  std::string parse_error;
  JsonReader reader(text);
  if (!reader.ParseDocument(&root, &parse_error)) {
    *error = "the options are not JSON: " + parse_error;
    return LAUFEY_WEBVIEW_INVALID;
  }
  if (root.type != JsonValue::Type::kObject) {
    *error = "the options must be an object";
    return LAUFEY_WEBVIEW_INVALID;
  }
  WebViewOptions o;
  const JsonValue* url = Field(root, "url");
  if (!url || url->type != JsonValue::Type::kString) {
    *error = "\"url\" must be a string";
    return LAUFEY_WEBVIEW_INVALID;
  }
  o.url = url->string;
  if (!ReadNumber(root, "x", &o.bounds.x, error) ||
      !ReadNumber(root, "y", &o.bounds.y, error) ||
      !ReadNumber(root, "width", &o.bounds.width, error) ||
      !ReadNumber(root, "height", &o.bounds.height, error))
    return LAUFEY_WEBVIEW_INVALID;
  if (!IsValidWebViewBounds(o.bounds.x, o.bounds.y, o.bounds.width,
                            o.bounds.height)) {
    *error =
        "the bounds must be finite, within 1e6, with a width and height "
        "of 0 or more";
    return LAUFEY_WEBVIEW_INVALID;
  }

  bool present = false;
  if (!ReadOptionalString(root, "partition", &o.partition, &present, error))
    return LAUFEY_WEBVIEW_INVALID;
  if (!IsValidPartitionName(o.partition)) {
    *error =
        "\"partition\" must be 1 to 64 characters of A-Z a-z 0-9 . _ : -, "
        "starting with a letter or digit";
    return LAUFEY_WEBVIEW_INVALID;
  }

  if (const JsonValue* v = Field(root, "visible")) {
    if (v->type != JsonValue::Type::kBool) {
      *error = "\"visible\" must be a boolean";
      return LAUFEY_WEBVIEW_INVALID;
    }
    o.visible = v->boolean;
  }

  std::string value;
  if (!ReadOptionalString(root, "allowPopups", &value, &present, error))
    return LAUFEY_WEBVIEW_INVALID;
  if (present) {
    if (value == "same-view") {
      o.popups = WebViewPopups::kSameView;
    } else if (value == "browser") {
      o.popups = WebViewPopups::kBrowser;
    } else if (value == "deny") {
      o.popups = WebViewPopups::kDeny;
    } else {
      *error = "\"allowPopups\" must be \"deny\", \"same-view\" or \"browser\"";
      return LAUFEY_WEBVIEW_INVALID;
    }
  }
  if (!ReadOptionalString(root, "downloads", &value, &present, error))
    return LAUFEY_WEBVIEW_INVALID;
  if (present) {
    if (value == "deny") {
      o.downloads = WebViewDownloads::kDeny;
    } else if (value == "ask") {
      o.downloads = WebViewDownloads::kAsk;
    } else if (value == "browser") {
      o.downloads = WebViewDownloads::kBrowser;
    } else {
      *error = "\"downloads\" must be \"deny\", \"ask\" or \"browser\"";
      return LAUFEY_WEBVIEW_INVALID;
    }
  }
  if (!ReadOptionalString(root, "permissions", &value, &present, error))
    return LAUFEY_WEBVIEW_INVALID;
  if (present) {
    if (value == "deny") {
      o.permissions = WebViewPermissions::kDeny;
    } else if (value == "ask") {
      o.permissions = WebViewPermissions::kAsk;
    } else {
      *error = "\"permissions\" must be \"deny\" or \"ask\"";
      return LAUFEY_WEBVIEW_INVALID;
    }
  }
  if (!ReadOptionalString(root, "contextMenu", &value, &present, error))
    return LAUFEY_WEBVIEW_INVALID;
  if (present) {
    if (value == "app") {
      o.app_context_menu = true;
    } else if (value != "native") {
      *error = "\"contextMenu\" must be \"native\" or \"app\"";
      return LAUFEY_WEBVIEW_INVALID;
    }
  }
  for (auto [key, field] :
       {std::pair<const char*, bool*>{"clipboardRead", &o.clipboard_read},
        {"clipboardWrite", &o.clipboard_write}}) {
    if (const JsonValue* v = Field(root, key)) {
      if (v->type != JsonValue::Type::kBool) {
        *error = std::string("\"") + key + "\" must be a boolean";
        return LAUFEY_WEBVIEW_INVALID;
      }
      *field = v->boolean;
    }
  }
  if (const JsonValue* v = Field(root, "forwardKeys")) {
    if (v->type != JsonValue::Type::kNull &&
        !ReadForwardKeys(*v, &o.forward_keys, error))
      return LAUFEY_WEBVIEW_INVALID;
  }
  if (!ReadOptionalString(root, "userAgent", &o.user_agent, &present, error))
    return LAUFEY_WEBVIEW_INVALID;
  if (present) {
    if (o.user_agent.size() > 1024) {
      *error = "\"userAgent\" must be at most 1024 characters";
      return LAUFEY_WEBVIEW_INVALID;
    }
    for (unsigned char c : o.user_agent) {
      if (c < 0x20 || c == 0x7f) {
        *error = "\"userAgent\" must not contain control characters";
        return LAUFEY_WEBVIEW_INVALID;
      }
    }
  }

  // The URL last: a malformed option is INVALID whatever the URL.
  if (!IsAllowedWebViewUrl(o.url)) {
    *error = "only http(s) URLs with a host can be loaded";
    return LAUFEY_WEBVIEW_NOT_ALLOWED_URL;
  }
  *out = std::move(o);
  return LAUFEY_WEBVIEW_OK;
}

// --- JSON -------------------------------------------------------------------

namespace {
const char* Bool(bool b) {
  return b ? "true" : "false";
}

std::string Number(double v) {
  if (!std::isfinite(v))
    return "0";
  // Bounds are within 1e6 CSS pixels: 4 decimals is finer than any device
  // pixel, then trailing zeros go.
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.4f", v);
  std::string s = buf;
  // Under a comma-decimal locale (setlocale in a GTK process) snprintf writes
  // ','; JSON needs '.'.
  for (char& c : s) {
    if (c == ',')
      c = '.';
  }
  size_t dot = s.find('.');
  if (dot != std::string::npos) {
    while (!s.empty() && s.back() == '0')
      s.pop_back();
    if (!s.empty() && s.back() == '.')
      s.pop_back();
  }
  if (s == "-0")
    s = "0";
  return s;
}
}  // namespace

std::string WebViewTitleJson(const std::string& title) {
  return "{\"title\":" + JsonQuote(title) + "}";
}

std::string WebViewUrlJson(const std::string& url, bool can_go_back,
                           bool can_go_forward) {
  return "{\"url\":" + JsonQuote(url) + ",\"canGoBack\":" + Bool(can_go_back) +
         ",\"canGoForward\":" + Bool(can_go_forward) + "}";
}

std::string WebViewLoadJson(const char* state, const std::string& url,
                            const char* error_code,
                            const std::string& error_message) {
  std::string s = state ? state : "complete";
  std::string out = "{\"state\":" + JsonQuote(s) + ",\"url\":" + JsonQuote(url);
  if (s == "failed") {
    out += ",\"error\":{\"code\":" +
           JsonQuote(error_code ? error_code : "failed") +
           ",\"message\":" + JsonQuote(error_message) + "}";
  }
  return out + "}";
}

std::string WebViewConsoleJson(const char* level, const std::string& message,
                               const std::string& source, int line) {
  std::string l = level ? level : "log";
  if (l != "log" && l != "info" && l != "warn" && l != "error" && l != "debug")
    l = "log";
  std::string out =
      "{\"level\":" + JsonQuote(l) + ",\"message\":" + JsonQuote(message);
  if (!source.empty())
    out += ",\"source\":" + JsonQuote(source);
  if (line > 0)
    out += ",\"line\":" + std::to_string(line);
  return out + "}";
}

std::string WebViewFocusJson(bool focused) {
  return std::string("{\"focused\":") + Bool(focused) + "}";
}

std::string WebViewNewWindowJson(const std::string& url, bool popup) {
  return "{\"url\":" + JsonQuote(url) +
         ",\"disposition\":" + (popup ? "\"popup\"" : "\"tab\"") + "}";
}

std::string WebViewFindResultJson(int matches, int active_match) {
  if (matches < 0)
    matches = 0;
  if (active_match < 0 || matches == 0)
    active_match = 0;
  return "{\"matches\":" + std::to_string(matches) +
         ",\"activeMatch\":" + std::to_string(active_match) + "}";
}

std::string WebViewContextMenuJson(const WebViewContextMenuFields& f) {
  std::string out = "{\"x\":" + Number(f.x) + ",\"y\":" + Number(f.y);
  if (!f.link_url.empty())
    out += ",\"linkUrl\":" + JsonQuote(f.link_url);
  if (!f.image_url.empty())
    out += ",\"imageUrl\":" + JsonQuote(f.image_url);
  if (!f.selection_text.empty())
    out += ",\"selectionText\":" + JsonQuote(f.selection_text);
  out += std::string(",\"isEditable\":") + Bool(f.is_editable);
  if (f.media_type)
    out += ",\"mediaType\":" + JsonQuote(f.media_type);
  return out + "}";
}

int32_t ParseWebViewForwardKeys(const std::string& text,
                                std::vector<std::string>* out,
                                std::string* error) {
  JsonValue root;
  std::string parse_error;
  JsonReader reader(text);
  if (!reader.ParseDocument(&root, &parse_error)) {
    *error = "the keys are not JSON: " + parse_error;
    return LAUFEY_WEBVIEW_INVALID;
  }
  return ReadForwardKeys(root, out, error) ? LAUFEY_WEBVIEW_OK
                                           : LAUFEY_WEBVIEW_INVALID;
}

std::string WebViewKeyJson(const WebViewKeyFields& f) {
  return "{\"type\":\"keydown\",\"key\":" + JsonQuote(f.key) +
         ",\"code\":" + JsonQuote(f.code) + ",\"altKey\":" + Bool(f.alt_key) +
         ",\"ctrlKey\":" + Bool(f.ctrl_key) +
         ",\"metaKey\":" + Bool(f.meta_key) +
         ",\"shiftKey\":" + Bool(f.shift_key) +
         ",\"repeat\":" + Bool(f.repeat) +
         ",\"forwarded\":" + Bool(f.forwarded) + "}";
}

bool WebViewKeyAccelerator(const std::string& code, bool alt, bool ctrl,
                           bool meta, bool shift, Accelerator* out) {
  Accelerator a;
  if (!KeyFromCode(code, &a))
    return false;
  a.mods = (ctrl ? kModCtrl : 0u) | (alt ? kModAlt : 0u) |
           (shift ? kModShift : 0u) | (meta ? kModSuper : 0u);
  *out = a;
  return true;
}

bool WebViewKeyMatches(const std::vector<std::string>& forward_keys,
                       const std::string& code, bool alt, bool ctrl, bool meta,
                       bool shift) {
  if (forward_keys.empty())
    return false;
  Accelerator a;
  if (!WebViewKeyAccelerator(code, alt, ctrl, meta, shift, &a))
    return false;
  const std::string canonical = CanonicalAccelerator(a);
  return std::find(forward_keys.begin(), forward_keys.end(), canonical) !=
         forward_keys.end();
}

bool IsValidWebViewZoom(double factor) {
  return std::isfinite(factor) && factor >= 0.25 && factor <= 5;
}

std::string WebViewStateJson(const WebViewStateFields& s) {
  return "{\"url\":" + JsonQuote(s.url) + ",\"title\":" + JsonQuote(s.title) +
         ",\"load\":" + JsonQuote(s.load ? s.load : "complete") +
         ",\"canGoBack\":" + Bool(s.can_go_back) +
         ",\"canGoForward\":" + Bool(s.can_go_forward) +
         ",\"visible\":" + Bool(s.visible) +
         ",\"bounds\":{\"x\":" + Number(s.bounds.x) +
         ",\"y\":" + Number(s.bounds.y) +
         ",\"width\":" + Number(s.bounds.width) +
         ",\"height\":" + Number(s.bounds.height) +
         "},\"zOrder\":" + std::to_string(s.z_order) + "}";
}

// --- Evaluate ---------------------------------------------------------------

const char kWebViewSerializeFunction[] =
    "(value) => {"
    " const bad = (what) => { throw new TypeError('not_serializable: ' + "
    "what); };"
    " const out = JSON.stringify(value === undefined ? null : value,"
    " function (key, v) {"
    "  const t = typeof v;"
    "  if (t === 'function') bad('a function');"
    "  if (t === 'symbol') bad('a symbol');"
    "  if (t === 'bigint') bad('a bigint');"
    "  if (v !== null && t === 'object') {"
    "   if (typeof Node === 'function' && v instanceof Node) bad('a DOM "
    "node');"
    "   if (typeof Window === 'function' && v instanceof Window) bad('a "
    "window');"
    "  }"
    "  return v;"
    " });"
    " return out === undefined ? 'null' : out;"
    "}";

std::string WebViewEvalScript(const std::string& script) {
  std::string body;
  body += "const __laufeySerialize = ";
  body += kWebViewSerializeFunction;
  body +=
      ";\n"
      "const __laufeyMessage = (e) => (e !== null && typeof e === 'object' && "
      "'message' in e) ? String(e.name || 'Error') + ': ' + String(e.message) "
      ": String(e);\n"
      "let __laufeyValue;\n"
      "try {\n"
      "  __laufeyValue = await (0, eval)(";
  body += JsonQuote(script);
  body +=
      ");\n"
      "} catch (e) {\n"
      "  return JSON.stringify({ ok: false, kind: 'error', message: "
      "__laufeyMessage(e) });\n"
      "}\n"
      "try {\n"
      "  return JSON.stringify({ ok: true, json: "
      "__laufeySerialize(__laufeyValue) });\n"
      "} catch (e) {\n"
      "  return JSON.stringify({ ok: false, kind: 'not_serializable', "
      "message: String(e && e.message || e).replace(/^not_serializable: /, "
      "'') });\n"
      "}\n";
  return body;
}

int32_t ParseWebViewEvalEnvelope(const std::string& envelope,
                                 std::string* text) {
  JsonValue root;
  std::string error;
  JsonReader reader(envelope);
  if (!reader.ParseDocument(&root, &error) ||
      root.type != JsonValue::Type::kObject) {
    *text = "the script's result could not be read";
    return LAUFEY_WEBVIEW_FAILED;
  }
  const JsonValue* ok = Field(root, "ok");
  if (!ok || ok->type != JsonValue::Type::kBool) {
    *text = "the script's result could not be read";
    return LAUFEY_WEBVIEW_FAILED;
  }
  if (ok->boolean) {
    const JsonValue* j = Field(root, "json");
    if (!j || j->type != JsonValue::Type::kString) {
      *text = "the script's result could not be read";
      return LAUFEY_WEBVIEW_FAILED;
    }
    *text = j->string;
    return LAUFEY_WEBVIEW_OK;
  }
  const JsonValue* kind = Field(root, "kind");
  const JsonValue* message = Field(root, "message");
  *text = message && message->type == JsonValue::Type::kString
              ? message->string
              : std::string("the script failed");
  if (kind && kind->type == JsonValue::Type::kString &&
      kind->string == "not_serializable")
    return LAUFEY_WEBVIEW_NOT_SERIALIZABLE;
  return LAUFEY_WEBVIEW_SCRIPT_ERROR;
}

// --- Registry ---------------------------------------------------------------

WebViewSinks& WebViewSinks::Instance() {
  static WebViewSinks* instance = new WebViewSinks();
  return *instance;
}

uint32_t WebViewSinks::Add(uint32_t window_id, laufey_webview_event_fn fn,
                           void* user_data) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  size_t count = 0;
  int32_t top = 0;
  for (const auto& kv : sinks_) {
    if (kv.second.window_id == window_id) {
      if (count == 0 || kv.second.z_order > top)
        top = kv.second.z_order;
      ++count;
    }
  }
  if (count >= LAUFEY_WEBVIEW_MAX_PER_WINDOW)
    return 0;
  uint32_t id = next_id_++;
  if (next_id_ == 0)
    next_id_ = 1;
  sinks_[id] = Sink{window_id, fn, user_data, count == 0 ? 0 : top + 1};
  return id;
}

bool WebViewSinks::Has(uint32_t webview_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  return sinks_.count(webview_id) != 0;
}

uint32_t WebViewSinks::WindowOf(uint32_t webview_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = sinks_.find(webview_id);
  return it == sinks_.end() ? 0 : it->second.window_id;
}

size_t WebViewSinks::CountForWindow(uint32_t window_id) {
  return ViewsOfWindow(window_id).size();
}

std::vector<uint32_t> WebViewSinks::ViewsOfWindow(uint32_t window_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  std::vector<uint32_t> out;
  for (const auto& kv : sinks_) {
    if (kv.second.window_id == window_id)
      out.push_back(kv.first);
  }
  return out;
}

void WebViewSinks::Emit(uint32_t webview_id, int32_t kind,
                        const std::string& detail) {
  if (kind == LAUFEY_WEBVIEW_EVENT_CLOSED) {
    Close(webview_id);
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = sinks_.find(webview_id);
  if (it == sinks_.end() || !it->second.fn)
    return;
  it->second.fn(it->second.user_data, webview_id, kind, detail.c_str());
}

bool WebViewSinks::Close(uint32_t webview_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = sinks_.find(webview_id);
  if (it == sinks_.end())
    return false;
  Sink sink = it->second;
  sinks_.erase(it);
  if (sink.fn)
    sink.fn(sink.user_data, webview_id, LAUFEY_WEBVIEW_EVENT_CLOSED, "{}");
  return true;
}

void WebViewSinks::Abandon(uint32_t webview_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  sinks_.erase(webview_id);
}

bool WebViewSinks::SetZOrder(uint32_t webview_id, int32_t z_order) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = sinks_.find(webview_id);
  if (it == sinks_.end())
    return false;
  it->second.z_order = z_order;
  return true;
}

bool WebViewSinks::Raise(uint32_t webview_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = sinks_.find(webview_id);
  if (it == sinks_.end())
    return false;
  int64_t top = it->second.z_order;
  for (const auto& kv : sinks_) {
    if (kv.first != webview_id && kv.second.window_id == it->second.window_id &&
        kv.second.z_order + int64_t{1} > top)
      top = kv.second.z_order + int64_t{1};
  }
  it->second.z_order = static_cast<int32_t>(top > INT32_MAX ? INT32_MAX : top);
  return true;
}

bool WebViewSinks::Lower(uint32_t webview_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = sinks_.find(webview_id);
  if (it == sinks_.end())
    return false;
  int64_t bottom = it->second.z_order;
  for (const auto& kv : sinks_) {
    if (kv.first != webview_id && kv.second.window_id == it->second.window_id &&
        kv.second.z_order - int64_t{1} < bottom)
      bottom = kv.second.z_order - int64_t{1};
  }
  it->second.z_order =
      static_cast<int32_t>(bottom < INT32_MIN ? INT32_MIN : bottom);
  return true;
}

int32_t WebViewSinks::ZOrderOf(uint32_t webview_id) {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  auto it = sinks_.find(webview_id);
  return it == sinks_.end() ? 0 : it->second.z_order;
}

std::vector<uint32_t> WebViewSinks::StackOrder(uint32_t window_id) {
  std::vector<std::pair<int32_t, uint32_t>> views;
  {
    std::lock_guard<std::recursive_mutex> lock(mu_);
    for (const auto& kv : sinks_) {
      if (kv.second.window_id == window_id)
        views.emplace_back(kv.second.z_order, kv.first);
    }
  }
  // Ids grow with creation, so (z, id) is z order then creation order.
  std::sort(views.begin(), views.end());
  std::vector<uint32_t> out;
  for (const auto& v : views)
    out.push_back(v.second);
  return out;
}

void WebViewSinks::ResetForTest() {
  std::lock_guard<std::recursive_mutex> lock(mu_);
  sinks_.clear();
  next_id_ = 1;
}

}  // namespace laufey_common
