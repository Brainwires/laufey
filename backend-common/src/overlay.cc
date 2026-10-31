// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The portable part of overlay windows, the activation policy and text to
// speech (API 50); see laufey_overlay.h.

#include "laufey_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "json_reader.h"

namespace laufey_common {

InputRegion MakeInputRegion(const laufey_rect_t* rects, size_t count) {
  InputRegion region;
  if (!rects)
    return region;
  region.active = true;
  for (size_t i = 0; i < count; i++) {
    if (rects[i].width > 0 && rects[i].height > 0)
      region.rects.push_back(rects[i]);
  }
  return region;
}

bool RegionTakesInput(const InputRegion& region, double x, double y) {
  if (!region.active)
    return true;
  for (const laufey_rect_t& r : region.rects) {
    if (x >= r.x && y >= r.y && x < (double)r.x + r.width &&
        y < (double)r.y + r.height)
      return true;
  }
  return false;
}

bool ShouldPassThrough(bool explicit_passthrough, const InputRegion& region,
                       bool inside_window, double x, double y) {
  if (explicit_passthrough)
    return true;
  if (!region.active)
    return false;
  if (!inside_window)
    return true;
  return !RegionTakesInput(region, x, y);
}

bool IsValidWindowLevel(int32_t level) {
  return level >= LAUFEY_WINDOW_LEVEL_NORMAL &&
         level <= LAUFEY_WINDOW_LEVEL_SCREEN_SAVER;
}

int32_t BackgroundThrottlingFromFlags(uint32_t flags) {
  if (flags & LAUFEY_WINDOW_FLAG_BACKGROUND_UNTHROTTLED)
    return LAUFEY_BACKGROUND_THROTTLING_NONE;
  if (flags & LAUFEY_WINDOW_FLAG_BACKGROUND_SUSPEND)
    return LAUFEY_BACKGROUND_THROTTLING_SUSPEND;
  return LAUFEY_BACKGROUND_THROTTLING_THROTTLE;
}

int32_t ParseActivationPolicy(const std::string& value) {
  if (value == "regular")
    return LAUFEY_ACTIVATION_POLICY_REGULAR;
  if (value == "accessory")
    return LAUFEY_ACTIVATION_POLICY_ACCESSORY;
  if (value == "prohibited")
    return LAUFEY_ACTIVATION_POLICY_PROHIBITED;
  return -1;
}

int32_t LaunchActivationPolicy(const std::string& launch_value,
                               bool ls_ui_element) {
  int32_t p = ParseActivationPolicy(launch_value);
  if (p >= 0)
    return p;
  return ls_ui_element ? LAUFEY_ACTIVATION_POLICY_ACCESSORY
                       : LAUFEY_ACTIVATION_POLICY_REGULAR;
}

// --- Text to speech ---------------------------------------------------------

namespace {

constexpr size_t kMaxSpeechTextBytes = 32 * 1024;

// The number a JSON value holds. The reader keeps only integers' values, so
// other numbers are parsed from their literal, which the reader does not
// keep either: fractional numbers are read back out of the original text by
// key (below).
bool FindNumberLiteral(const std::string& json, const std::string& key,
                       double* out) {
  // A strict reader validated the document already; find `"key"` followed
  // by `:` at the top level of the object.
  std::string needle = "\"" + key + "\"";
  size_t depth = 0;
  bool in_string = false;
  for (size_t i = 0; i < json.size(); i++) {
    char c = json[i];
    if (in_string) {
      if (c == '\\') {
        i++;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '{' || c == '[') {
      depth++;
    } else if (c == '}' || c == ']') {
      depth--;
    } else if (c == '"') {
      if (depth == 1 && json.compare(i, needle.size(), needle) == 0) {
        size_t j = i + needle.size();
        while (j < json.size() && (json[j] == ' ' || json[j] == '\t' ||
                                   json[j] == '\n' || json[j] == '\r'))
          j++;
        if (j < json.size() && json[j] == ':') {
          const char* start = json.c_str() + j + 1;
          char* end = nullptr;
          double v = std::strtod(start, &end);
          if (end != start && std::isfinite(v)) {
            *out = v;
            return true;
          }
          return false;
        }
      }
      in_string = true;
    }
  }
  return false;
}

double Clamp(double v, double lo, double hi) {
  return std::max(lo, std::min(hi, v));
}

}  // namespace

bool ParseSpeechRequest(const char* json, SpeechRequest* out) {
  if (!json || !out)
    return false;
  std::string text(json);
  json::JsonValue doc;
  std::string error;
  json::JsonReader parser(text);
  if (!parser.ParseDocument(&doc, &error) ||
      doc.type != json::JsonValue::Type::kObject)
    return false;
  SpeechRequest r;
  bool has_text = false;
  for (const auto& [key, value] : doc.object) {
    if (key == "text" && value.type == json::JsonValue::Type::kString) {
      r.text = value.string;
      has_text = true;
    } else if (key == "voice" && value.type == json::JsonValue::Type::kString) {
      r.voice = value.string;
    } else if (key == "lang" && value.type == json::JsonValue::Type::kString) {
      r.lang = value.string;
    }
  }
  if (!has_text || r.text.empty() || r.text.size() > kMaxSpeechTextBytes)
    return false;
  double v = 0;
  if (FindNumberLiteral(text, "rate", &v))
    r.rate = Clamp(v, 0.1, 10.0);
  if (FindNumberLiteral(text, "pitch", &v))
    r.pitch = Clamp(v, 0.0, 2.0);
  if (FindNumberLiteral(text, "volume", &v))
    r.volume = Clamp(v, 0.0, 1.0);
  *out = r;
  return true;
}

uint32_t SpeechTracker::Begin(laufey_speech_fn callback, void* user_data) {
  Pending previous;
  uint32_t id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    previous = current_;
    id = next_id_++;
    if (next_id_ == 0)
      next_id_ = 1;
    current_ = Pending{id, callback, user_data};
  }
  if (previous.id && previous.callback)
    previous.callback(previous.user_data, previous.id, LAUFEY_SPEECH_CANCELLED,
                      "");
  return id;
}

void SpeechTracker::Finish(uint32_t id, int32_t status,
                           const std::string& message) {
  Pending done;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id == 0 || current_.id != id)
      return;
    done = current_;
    current_ = Pending{};
  }
  if (done.callback)
    done.callback(done.user_data, done.id, status, message.c_str());
}

void SpeechTracker::CancelCurrent() {
  uint32_t id = current();
  if (id)
    Finish(id, LAUFEY_SPEECH_CANCELLED, "");
}

uint32_t SpeechTracker::current() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_.id;
}

SpeechTracker& Speech() {
  static SpeechTracker* tracker = new SpeechTracker();
  return *tracker;
}

uint32_t Speak(const char* request_json, laufey_speech_fn callback,
               void* user_data) {
  SpeechRequest request;
  if (!callback || !ParseSpeechRequest(request_json, &request))
    return 0;
  // Stop the engine before the tracker cancels its utterance, so the engine
  // never reports the old id after the new one began.
  if (Speech().current())
    StopSpeakingNative();
  uint32_t id = Speech().Begin(callback, user_data);
  if (!SpeechAvailableNative()) {
    Speech().Finish(id, LAUFEY_SPEECH_UNSUPPORTED,
                    "no text-to-speech engine here");
    return id;
  }
  SpeakNative(id, request);
  return id;
}

void StopSpeaking() {
  StopSpeakingNative();
  Speech().CancelCurrent();
}

char* SpeechVoicesJson() {
  std::string json = SpeechAvailableNative() ? SpeechVoicesJsonNative() : "[]";
  if (json.empty())
    json = "[]";
  char* out = static_cast<char*>(std::malloc(json.size() + 1));
  if (!out)
    return nullptr;
  std::memcpy(out, json.c_str(), json.size() + 1);
  return out;
}

}  // namespace laufey_common
