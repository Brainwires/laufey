// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Text to speech on Windows (API 50): SAPI's ISpVoice on a worker thread of
// its own (COM, multithreaded apartment), which speaks one utterance at a
// time and polls for a stop while it waits. See laufey_overlay.h.

#include "laufey_overlay.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sapi.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace laufey_common {

std::wstring Utf8ToWide(const std::string& s);  // strings_win.cc

namespace {

struct Job {
  uint32_t id = 0;
  SpeechRequest request;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<Job> g_jobs;
std::atomic<bool> g_stop{false};
bool g_started = false;
std::atomic<int> g_available{-1};  // -1 unknown, 0 no, 1 yes

std::wstring XmlEscape(const std::wstring& s) {
  std::wstring out;
  for (wchar_t c : s) {
    switch (c) {
      case L'<':
        out += L"&lt;";
        break;
      case L'>':
        out += L"&gt;";
        break;
      case L'&':
        out += L"&amp;";
        break;
      case L'"':
        out += L"&quot;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

std::string WideToUtf8(const wchar_t* w) {
  if (!w)
    return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1)
    return "";
  std::string out(n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::string JsonString(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
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

// The voice token whose id is `id`, else NULL.
ISpObjectToken* FindVoice(const std::wstring& id) {
  ISpObjectToken* token = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&token))))
    return nullptr;
  if (FAILED(token->SetId(nullptr, id.c_str(), FALSE))) {
    token->Release();
    return nullptr;
  }
  return token;
}

void SpeakJob(ISpVoice* voice, const Job& job) {
  const SpeechRequest& r = job.request;
  if (!r.voice.empty()) {
    if (ISpObjectToken* token = FindVoice(Utf8ToWide(r.voice))) {
      voice->SetVoice(token);
      token->Release();
    }
  } else {
    voice->SetVoice(nullptr);  // the default voice
  }
  // rate 0.1..10 -> SAPI -10..10 on a log scale (1 -> 0).
  long rate = (long)std::lround(std::log10(r.rate) * 10.0);
  voice->SetRate(rate < -10 ? -10 : (rate > 10 ? 10 : rate));
  voice->SetVolume((USHORT)std::lround(r.volume * 100.0));
  // pitch 0..2 -> SAPI XML's absmiddle -10..10.
  long pitch = (long)std::lround((r.pitch - 1.0) * 10.0);
  std::wstring xml = L"<pitch absmiddle=\"" + std::to_wstring(pitch) + L"\">" +
                     XmlEscape(Utf8ToWide(r.text)) + L"</pitch>";
  HRESULT hr = voice->Speak(
      xml.c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_XML, nullptr);
  if (FAILED(hr)) {
    char buf[96];
    // SPERR_NOT_FOUND from Speak: SAPI found no audio output device (a
    // machine with no sound card, a service session).
    if (hr == (HRESULT)0x8004503A)
      snprintf(buf, sizeof buf,
               "no audio output device (SAPI SPERR_NOT_FOUND 0x%08lx)",
               (long)hr);
    else
      snprintf(buf, sizeof buf, "SAPI refused the text (0x%08lx)", (long)hr);
    Speech().Finish(job.id, LAUFEY_SPEECH_FAILED, buf);
    return;
  }
  for (;;) {
    if (g_stop.exchange(false)) {
      voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
      Speech().Finish(job.id, LAUFEY_SPEECH_CANCELLED, "");
      return;
    }
    if (voice->WaitUntilDone(50) == S_OK) {
      Speech().Finish(job.id, LAUFEY_SPEECH_DONE, "");
      return;
    }
  }
}

void Worker() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ISpVoice* voice = nullptr;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&voice))) &&
            voice;
  g_available = ok ? 1 : 0;
  g_cv.notify_all();
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait(lock, [] { return !g_jobs.empty(); });
      job = g_jobs.front();
      g_jobs.pop_front();
    }
    if (!ok) {
      Speech().Finish(job.id, LAUFEY_SPEECH_UNSUPPORTED, "SAPI is missing");
      continue;
    }
    // A stop asked for before this job began was for an earlier one.
    g_stop = false;
    SpeakJob(voice, job);
  }
}

void EnsureWorker() {
  std::unique_lock<std::mutex> lock(g_mutex);
  if (!g_started) {
    g_started = true;
    std::thread(Worker).detach();
  }
  g_cv.wait(lock, [] { return g_available.load() >= 0; });
}

}  // namespace

bool SpeechAvailableNative() {
  EnsureWorker();
  return g_available.load() == 1;
}

void SpeakNative(uint32_t id, const SpeechRequest& request) {
  EnsureWorker();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_jobs.push_back(Job{id, request});
  }
  g_cv.notify_all();
}

void StopSpeakingNative() {
  std::lock_guard<std::mutex> lock(g_mutex);
  // Queued jobs were superseded: the tracker already cancelled their ids, so
  // dropping them reports nothing twice.
  g_jobs.clear();
  g_stop = true;
}

std::string SpeechVoicesJsonNative() {
  HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::string out = "[";
  ISpObjectTokenCategory* category = nullptr;
  IEnumSpObjectTokens* tokens = nullptr;
  std::wstring default_id;
  if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr,
                                 CLSCTX_ALL, IID_PPV_ARGS(&category))) &&
      SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE))) {
    LPWSTR def = nullptr;
    if (SUCCEEDED(category->GetDefaultTokenId(&def)) && def) {
      default_id = def;
      CoTaskMemFree(def);
    }
    if (SUCCEEDED(category->EnumTokens(nullptr, nullptr, &tokens)) && tokens) {
      ISpObjectToken* token = nullptr;
      bool first = true;
      while (tokens->Next(1, &token, nullptr) == S_OK && token) {
        LPWSTR id = nullptr, name = nullptr, lang_hex = nullptr;
        token->GetId(&id);
        token->GetStringValue(nullptr, &name);
        ISpDataKey* attrs = nullptr;
        if (SUCCEEDED(token->OpenKey(L"Attributes", &attrs)) && attrs) {
          attrs->GetStringValue(L"Language", &lang_hex);
          attrs->Release();
        }
        // "Language" is a hex LCID list ("409;9"); take the first.
        std::string lang;
        if (lang_hex) {
          LCID lcid = (LCID)wcstoul(lang_hex, nullptr, 16);
          wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
          if (LCIDToLocaleName(lcid, locale, LOCALE_NAME_MAX_LENGTH, 0) > 0)
            lang = WideToUtf8(locale);
        }
        if (!first)
          out += ",";
        first = false;
        bool is_default = id && default_id == id;
        out += "{\"id\":" + JsonString(WideToUtf8(id)) +
               ",\"name\":" + JsonString(WideToUtf8(name)) +
               ",\"lang\":" + JsonString(lang) +
               ",\"default\":" + (is_default ? "true" : "false") + "}";
        if (id)
          CoTaskMemFree(id);
        if (name)
          CoTaskMemFree(name);
        if (lang_hex)
          CoTaskMemFree(lang_hex);
        token->Release();
        token = nullptr;
      }
      tokens->Release();
    }
  }
  if (category)
    category->Release();
  if (SUCCEEDED(init))
    CoUninitialize();
  return out + "]";
}

}  // namespace laufey_common
