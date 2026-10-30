// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Text to speech on macOS (API 50): AVSpeechSynthesizer. See
// laufey_overlay.h.

#include "laufey_overlay.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <string>

@interface LaufeySpeechDelegate : NSObject <AVSpeechSynthesizerDelegate>
@end

namespace {

AVSpeechSynthesizer* g_synth = nil;
LaufeySpeechDelegate* g_delegate = nil;
// The utterance object of each laufey id in flight (main queue only).
NSMapTable<AVSpeechUtterance*, NSNumber*>* g_ids = nil;

AVSpeechSynthesizer* Synth() {
  if (!g_synth) {
    g_synth = [[AVSpeechSynthesizer alloc] init];
    g_delegate = [[LaufeySpeechDelegate alloc] init];
    g_synth.delegate = g_delegate;
    g_ids = [NSMapTable strongToStrongObjectsMapTable];
  }
  return g_synth;
}

void Report(AVSpeechUtterance* u, int32_t status) {
  NSNumber* id_num = [g_ids objectForKey:u];
  if (!id_num)
    return;
  [g_ids removeObjectForKey:u];
  laufey_common::Speech().Finish([id_num unsignedIntValue], status, "");
}

std::string JsonString(NSString* s) {
  std::string out = "\"";
  for (const char* p = s ? [s UTF8String] : ""; p && *p; p++) {
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

@implementation LaufeySpeechDelegate
- (void)speechSynthesizer:(AVSpeechSynthesizer*)synthesizer
    didFinishSpeechUtterance:(AVSpeechUtterance*)utterance {
  Report(utterance, LAUFEY_SPEECH_DONE);
}
- (void)speechSynthesizer:(AVSpeechSynthesizer*)synthesizer
    didCancelSpeechUtterance:(AVSpeechUtterance*)utterance {
  Report(utterance, LAUFEY_SPEECH_CANCELLED);
}
@end

namespace laufey_common {

bool SpeechAvailableNative() {
  return true;
}

void SpeakNative(uint32_t id, const SpeechRequest& request) {
  NSString* text = [NSString stringWithUTF8String:request.text.c_str()];
  NSString* voice_id =
      request.voice.empty()
          ? nil
          : [NSString stringWithUTF8String:request.voice.c_str()];
  NSString* lang = request.lang.empty()
                       ? nil
                       : [NSString stringWithUTF8String:request.lang.c_str()];
  double rate = request.rate, pitch = request.pitch, volume = request.volume;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (!text) {
      Speech().Finish(id, LAUFEY_SPEECH_FAILED, "the text is not UTF-8");
      return;
    }
    AVSpeechUtterance* u = [AVSpeechUtterance speechUtteranceWithString:text];
    AVSpeechSynthesisVoice* voice = nil;
    if (voice_id)
      voice = [AVSpeechSynthesisVoice voiceWithIdentifier:voice_id];
    if (!voice && lang)
      voice = [AVSpeechSynthesisVoice voiceWithLanguage:lang];
    if (voice)
      u.voice = voice;
    // 1.0 is AVFoundation's default rate; scale it, within its range.
    float r = (float)(AVSpeechUtteranceDefaultSpeechRate * rate);
    u.rate = std::max(AVSpeechUtteranceMinimumSpeechRate,
                      std::min(AVSpeechUtteranceMaximumSpeechRate, r));
    u.pitchMultiplier = (float)std::max(0.5, std::min(2.0, pitch));
    u.volume = (float)volume;
    AVSpeechSynthesizer* synth = Synth();
    [g_ids setObject:@(id) forKey:u];
    [synth speakUtterance:u];
  });
}

void StopSpeakingNative() {
  dispatch_async(dispatch_get_main_queue(), ^{
    if (g_synth)
      [g_synth stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
  });
}

std::string SpeechVoicesJsonNative() {
  NSString* current = [AVSpeechSynthesisVoice currentLanguageCode];
  std::string out = "[";
  bool first = true;
  bool default_given = false;
  for (AVSpeechSynthesisVoice* v in [AVSpeechSynthesisVoice speechVoices]) {
    bool is_default =
        !default_given && current && [v.language isEqualToString:current];
    if (is_default)
      default_given = true;
    if (!first)
      out += ",";
    first = false;
    out += "{\"id\":" + JsonString(v.identifier) +
           ",\"name\":" + JsonString(v.name) +
           ",\"lang\":" + JsonString(v.language) +
           ",\"default\":" + (is_default ? "true" : "false") + "}";
  }
  return out + "]";
}

}  // namespace laufey_common
