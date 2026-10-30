// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Text to speech on Linux (API 50): speech-dispatcher through a dlopen'd
// libspeechd (no build dependency; SpeechAvailableNative is false without
// it or without a reachable server). See laufey_overlay.h.

#include "laufey_overlay.h"

#include <dlfcn.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>

namespace laufey_common {

namespace {

// libspeechd.h, the parts used here. The SPDConnection fields up to the
// callbacks have kept this layout since speech-dispatcher 0.7.
typedef void (*SpdCallback)(size_t msg_id, size_t client_id, int state);
struct SpdConnectionPrefix {
  int mode;
  void* stream;
  SpdCallback callback_begin;
  SpdCallback callback_end;
  SpdCallback callback_cancel;
  SpdCallback callback_pause;
  SpdCallback callback_resume;
};
struct SpdVoice {
  char* name;
  char* language;
  char* variant;
};
constexpr int kSpdModeThreaded = 1;  // SPD_MODE_THREADED
constexpr int kSpdText = 3;          // SPD_TEXT
constexpr int kSpdEnd = 2;           // SPD_END
constexpr int kSpdCancel = 8;        // SPD_CANCEL
constexpr int kSpdEventEnd = 1;      // SPD_EVENT_END
constexpr int kSpdEventCancel = 2;   // SPD_EVENT_CANCEL

struct SpdApi {
  void* (*open)(const char*, const char*, const char*, int) = nullptr;
  int (*say)(void*, int, const char*) = nullptr;
  int (*cancel)(void*) = nullptr;
  int (*set_rate)(void*, int) = nullptr;
  int (*set_pitch)(void*, int) = nullptr;
  int (*set_volume)(void*, int) = nullptr;
  int (*set_language)(void*, const char*) = nullptr;
  int (*set_synthesis_voice)(void*, const char*) = nullptr;
  int (*set_notification_on)(void*, int) = nullptr;
  SpdVoice** (*list_voices)(void*) = nullptr;
  void (*free_voices)(SpdVoice**) = nullptr;
  bool loaded = false;
};

const SpdApi& Spd() {
  static SpdApi api = [] {
    SpdApi a;
    void* lib = dlopen("libspeechd.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      return a;
    auto sym = [lib](const char* n) { return dlsym(lib, n); };
    a.open = reinterpret_cast<decltype(a.open)>(sym("spd_open"));
    a.say = reinterpret_cast<decltype(a.say)>(sym("spd_say"));
    a.cancel = reinterpret_cast<decltype(a.cancel)>(sym("spd_cancel"));
    a.set_rate =
        reinterpret_cast<decltype(a.set_rate)>(sym("spd_set_voice_rate"));
    a.set_pitch =
        reinterpret_cast<decltype(a.set_pitch)>(sym("spd_set_voice_pitch"));
    a.set_volume =
        reinterpret_cast<decltype(a.set_volume)>(sym("spd_set_volume"));
    a.set_language =
        reinterpret_cast<decltype(a.set_language)>(sym("spd_set_language"));
    a.set_synthesis_voice = reinterpret_cast<decltype(a.set_synthesis_voice)>(
        sym("spd_set_synthesis_voice"));
    a.set_notification_on = reinterpret_cast<decltype(a.set_notification_on)>(
        sym("spd_set_notification_on"));
    a.list_voices = reinterpret_cast<decltype(a.list_voices)>(
        sym("spd_list_synthesis_voices"));
    a.free_voices =
        reinterpret_cast<decltype(a.free_voices)>(sym("free_spd_voices"));
    a.loaded = a.open && a.say && a.cancel && a.set_rate && a.set_pitch &&
               a.set_volume && a.set_notification_on;
    return a;
  }();
  return api;
}

std::mutex g_mutex;
void* g_conn = nullptr;
bool g_conn_tried = false;
// speech-dispatcher message id -> laufey utterance id.
std::map<size_t, uint32_t> g_messages;

void OnEvent(size_t msg_id, size_t, int state) {
  uint32_t id = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_messages.find(msg_id);
    if (it == g_messages.end())
      return;
    id = it->second;
    g_messages.erase(it);
  }
  Speech().Finish(
      id,
      state == kSpdEventCancel ? LAUFEY_SPEECH_CANCELLED : LAUFEY_SPEECH_DONE,
      "");
}

void OnEnd(size_t msg_id, size_t client_id, int) {
  OnEvent(msg_id, client_id, kSpdEventEnd);
}
void OnCancel(size_t msg_id, size_t client_id, int) {
  OnEvent(msg_id, client_id, kSpdEventCancel);
}

// The connection, opened on first use (with g_mutex held).
void* ConnectionLocked() {
  if (g_conn || g_conn_tried)
    return g_conn;
  g_conn_tried = true;
  const SpdApi& spd = Spd();
  if (!spd.loaded)
    return nullptr;
  g_conn = spd.open("laufey", "main", nullptr, kSpdModeThreaded);
  if (!g_conn)
    return nullptr;
  auto* prefix = static_cast<SpdConnectionPrefix*>(g_conn);
  prefix->callback_end = OnEnd;
  prefix->callback_cancel = OnCancel;
  spd.set_notification_on(g_conn, kSpdEnd);
  spd.set_notification_on(g_conn, kSpdCancel);
  return g_conn;
}

int Scale100(double v) {
  int n = (int)std::lround(v);
  return n < -100 ? -100 : (n > 100 ? 100 : n);
}

std::string JsonString(const char* s) {
  std::string out = "\"";
  for (const char* p = s ? s : ""; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else {
      out += (char)c;
    }
  }
  return out + "\"";
}

}  // namespace

bool SpeechAvailableNative() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return ConnectionLocked() != nullptr;
}

void SpeakNative(uint32_t id, const SpeechRequest& request) {
  const SpdApi& spd = Spd();
  int32_t failure = -1;
  const char* why = "";
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    void* conn = ConnectionLocked();
    if (!conn) {
      failure = LAUFEY_SPEECH_UNSUPPORTED;
      why = "no speech-dispatcher";
    } else {
      // rate 0.1..10 -> -100..100 on a log scale (1 -> 0).
      spd.set_rate(conn, Scale100(std::log10(request.rate) * 100.0));
      spd.set_pitch(conn, Scale100((request.pitch - 1.0) * 100.0));
      spd.set_volume(conn, Scale100(request.volume * 200.0 - 100.0));
      if (!request.lang.empty() && spd.set_language)
        spd.set_language(conn, request.lang.c_str());
      if (!request.voice.empty() && spd.set_synthesis_voice)
        spd.set_synthesis_voice(conn, request.voice.c_str());
      // The end / cancel callbacks wait for g_mutex, so the message is in
      // g_messages before either can look for it.
      int msg = spd.say(conn, kSpdText, request.text.c_str());
      if (msg <= 0) {
        failure = LAUFEY_SPEECH_FAILED;
        why = "speech-dispatcher refused the text";
      } else {
        g_messages[(size_t)msg] = id;
      }
    }
  }
  // The callback runs without our lock: it may speak again.
  if (failure >= 0)
    Speech().Finish(id, failure, why);
}

void StopSpeakingNative() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_conn)
    Spd().cancel(g_conn);
}

std::string SpeechVoicesJsonNative() {
  const SpdApi& spd = Spd();
  std::lock_guard<std::mutex> lock(g_mutex);
  void* conn = ConnectionLocked();
  if (!conn || !spd.list_voices)
    return "[]";
  SpdVoice** voices = spd.list_voices(conn);
  if (!voices)
    return "[]";
  std::string out = "[";
  for (int i = 0; voices[i]; i++) {
    if (i)
      out += ",";
    out += "{\"id\":" + JsonString(voices[i]->name) +
           ",\"name\":" + JsonString(voices[i]->name) +
           ",\"lang\":" + JsonString(voices[i]->language) +
           ",\"default\":" + (i == 0 ? "true" : "false") + "}";
  }
  if (spd.free_voices)
    spd.free_voices(voices);
  return out + "]";
}

}  // namespace laufey_common
