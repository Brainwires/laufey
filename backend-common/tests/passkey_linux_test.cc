// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Passkeys on Linux (passkey_linux.cc): the canonical options documents the
// library gets, which allow lists a USB key can meet, the envelopes made
// from the library's result documents, the library missing (no capability,
// not_supported with the reason), and, with liblaufey_webauthn.so built
// with its `virt-test` feature (its path in LAUFEY_TEST_WEBAUTHN_LIBRARY),
// whole ceremonies through PasskeyBegin + PasskeyStartLinux against the
// library's virtual security key: registration then authentication, a PIN
// (asked through the UI hook, wrong then right), a cancelled PIN entry, the
// anchor window closing while the key is awaited, the timeout, and the
// requests Linux refuses up front (a platform authenticator; an allow list
// no USB key can meet). Without the library those cases are skipped, unless
// LAUFEY_TEST_REQUIRE_WEBAUTHN=1 (CI), which makes that a failure.

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/json_reader.h"
#include "../src/laufey_webauthn_abi.h"
#include "laufey_passkey.h"

using namespace laufey_common;
using laufey_common::json::JsonReader;
using laufey_common::json::JsonValue;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::fflush(stderr);                                                   \
      std::_Exit(1);                                                         \
    }                                                                        \
  } while (0)

namespace {

using Clock = std::chrono::steady_clock;

bool Contains(const std::string& s, const std::string& part) {
  return s.find(part) != std::string::npos;
}

JsonValue Parse(const std::string& text) {
  JsonValue v;
  std::string error;
  EXPECT(JsonReader(text).ParseDocument(&v, &error));
  return v;
}

const JsonValue* Member(const JsonValue& v, const char* name) {
  for (const auto& [k, m] : v.object) {
    if (k == name)
      return &m;
  }
  return nullptr;
}

std::string Text(const JsonValue& v, const char* name) {
  const JsonValue* m = Member(v, name);
  return m && m->type == JsonValue::Type::kString ? m->string : "";
}

// --- Pure pieces
// -------------------------------------------------------------------

void TestCanonicalOptions() {
  PasskeyCreationOptions c;
  PasskeyError error;
  EXPECT(ParsePasskeyCreationOptions(
      R"({"rp":{"id":"example.com","name":"Ex \"ample\""},"user":{"id":"dXNlcl8x","name":"ada","displayName":"Ada"},"challenge":"AAECAw","pubKeyCredParams":[{"type":"public-key","alg":-8},{"type":"public-key","alg":-7}],"timeout":30000,"authenticatorSelection":{"residentKey":"required","userVerification":"required"},"attestation":"direct","excludeCredentials":[{"type":"public-key","id":"AQID","transports":["usb","nfc"]}],"extensions":{"x":1},"hints":["security-key"]})",
      &c, &error));
  std::string json = PasskeyLinuxCreationJson(c, 30000);
  EXPECT(
      json ==
      R"({"rp":{"id":"example.com","name":"Ex \"ample\""},"user":{"id":"dXNlcl8x","name":"ada","displayName":"Ada"},"challenge":"AAECAw","pubKeyCredParams":[{"type":"public-key","alg":-8},{"type":"public-key","alg":-7}],"timeout":30000,"authenticatorSelection":{"residentKey":"required","userVerification":"required"},"attestation":"direct","excludeCredentials":[{"type":"public-key","id":"AQID","transports":["usb","nfc"]}]})");
  // Defaults where the options say nothing: ES256 + RS256, preferred UV, no
  // resident key, no attestation; the RP's name falls back to its id.
  PasskeyCreationOptions bare;
  EXPECT(ParsePasskeyCreationOptions(
      R"({"rp":{"id":"example.com"},"user":{"id":"dXNlcl8x"},"challenge":"AAECAw"})",
      &bare, &error));
  json = PasskeyLinuxCreationJson(bare, 60000);
  EXPECT(Contains(json, R"("name":"example.com")"));
  EXPECT(Contains(
      json,
      R"("pubKeyCredParams":[{"type":"public-key","alg":-7},{"type":"public-key","alg":-257}])"));
  EXPECT(Contains(
      json,
      R"("authenticatorSelection":{"residentKey":"discouraged","userVerification":"preferred"})"));
  EXPECT(Contains(json, R"("attestation":"none")"));
  EXPECT(Contains(json, R"("timeout":60000)"));
  // A control character in a name is escaped.
  bare.user_display_name = std::string("A\x01") + "da";
  EXPECT(Contains(PasskeyLinuxCreationJson(bare, 1000), "A\\u0001da"));

  PasskeyRequestOptions r;
  EXPECT(ParsePasskeyRequestOptions(
      R"({"challenge":"AAECAw","rpId":"example.com","userVerification":"discouraged","allowCredentials":[{"type":"public-key","id":"AQID"}]})",
      &r, &error));
  EXPECT(
      PasskeyLinuxRequestJson(r, 5000) ==
      R"({"challenge":"AAECAw","rpId":"example.com","timeout":5000,"userVerification":"discouraged","allowCredentials":[{"type":"public-key","id":"AQID"}]})");
}

void TestReachable() {
  std::string why;
  EXPECT(PasskeyLinuxReachable({}, &why));  // discoverable
  PasskeyCredentialDescriptor any;
  any.id = {1};
  EXPECT(PasskeyLinuxReachable({any}, &why));  // no transports: try
  PasskeyCredentialDescriptor phone;
  phone.id = {2};
  phone.transports = {"internal", "hybrid"};
  EXPECT(!PasskeyLinuxReachable({phone}, &why));
  EXPECT(Contains(why, "USB"));
  PasskeyCredentialDescriptor key;
  key.id = {3};
  key.transports = {"usb"};
  EXPECT(PasskeyLinuxReachable({phone, key}, &why));
}

void TestEnvelopes() {
  std::string e = PasskeyLinuxEnvelope(
      true,
      R"({"ok":true,"credentialId":"AQID","clientDataJSON":"e30","attestationObject":"oA","transports":["usb","nfc"],"attachment":null})");
  EXPECT(Contains(e, R"("ok":true)"));
  EXPECT(Contains(e, R"("id":"AQID")"));
  EXPECT(Contains(e, R"("authenticatorAttachment":"cross-platform")"));
  EXPECT(Contains(e, R"("transports":["usb","nfc"])"));
  EXPECT(Contains(e, R"("attestationObject":"oA")"));

  e = PasskeyLinuxEnvelope(
      false,
      R"({"ok":true,"credentialId":"AQID","clientDataJSON":"e30","authenticatorData":"AA","signature":"AQ","userHandle":"dXNlcl8x"})");
  EXPECT(Contains(e, R"("userHandle":"dXNlcl8x")"));
  EXPECT(Contains(e, R"("signature":"AQ")"));
  // No user handle: left out.
  e = PasskeyLinuxEnvelope(
      false,
      R"({"ok":true,"credentialId":"AQID","clientDataJSON":"e30","authenticatorData":"AA","signature":"AQ","userHandle":null})");
  EXPECT(!Contains(e, "userHandle"));
  // Incomplete answers and nonsense are `unknown`, never a credential.
  EXPECT(Contains(PasskeyLinuxEnvelope(false, R"({"ok":true})"),
                  R"("code":"unknown")"));
  EXPECT(
      Contains(PasskeyLinuxEnvelope(true, "not json"), R"("code":"unknown")"));
  EXPECT(
      Contains(PasskeyLinuxEnvelope(true, R"({"ok":true,"credentialId":"!"})"),
               R"("code":"unknown")"));
  // Errors keep the library's code when it is one of the five.
  for (const char* code :
       {"cancelled", "invalid_rp", "not_supported", "timeout", "unknown"}) {
    std::string doc =
        std::string(R"({"ok":false,"code":")") + code + R"(","message":"why"})";
    std::string env = PasskeyLinuxEnvelope(true, doc);
    EXPECT(Contains(env, std::string(R"("code":")") + code + "\""));
    EXPECT(Contains(env, R"("message":"why")"));
  }
  EXPECT(
      Contains(PasskeyLinuxEnvelope(true, R"({"ok":false,"code":"made_up"})"),
               R"("code":"unknown")"));
}

// --- Ceremonies
// --------------------------------------------------------------------

struct Result {
  std::mutex mutex;
  std::condition_variable cv;
  std::string envelope;
  int calls = 0;
};

void OnResult(void* user_data, const char* json) {
  auto* r = static_cast<Result*>(user_data);
  std::lock_guard<std::mutex> lock(r->mutex);
  r->envelope = json;
  r->calls++;
  r->cv.notify_all();
}

std::string Wait(Result& r, int seconds = 30) {
  std::unique_lock<std::mutex> lock(r.mutex);
  EXPECT(r.cv.wait_for(lock, std::chrono::seconds(seconds),
                       [&] { return r.calls > 0; }));
  return r.envelope;
}

// Starts a ceremony as a backend does; its result, once.
std::string Run(uint32_t kind, const std::string& options,
                const void* window_key = nullptr,
                std::function<void()> during = nullptr) {
  Result r;
  auto ceremony = PasskeyBegin(kind, options.c_str(), OnResult, &r);
  if (!ceremony)
    return Wait(r);
  PasskeyStartLinux(ceremony, nullptr, window_key);
  if (during)
    during();
  std::string envelope = Wait(r);
  // The slot is free again once the library is done.
  auto deadline = Clock::now() + std::chrono::seconds(10);
  while (PasskeyBusyForTesting() && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT(!PasskeyBusyForTesting());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT(r.calls == 1);
  return envelope;
}

std::string CreateOptions(const char* resident, const char* uv,
                          const char* attachment = nullptr,
                          int timeout = 30000) {
  std::string selection = std::string(R"({"residentKey":")") + resident +
                          R"(","userVerification":")" + uv + "\"";
  if (attachment)
    selection +=
        std::string(R"(,"authenticatorAttachment":")") + attachment + "\"";
  selection += "}";
  return std::string(
             R"({"rp":{"id":"example.com","name":"Example"},"user":{"id":"dXNlcl8x","name":"ada","displayName":"Ada"},"challenge":"AAECAwQFBgcICQoLDA0ODw","pubKeyCredParams":[{"type":"public-key","alg":-7}],"timeout":)") +
         std::to_string(timeout) + R"(,"authenticatorSelection":)" + selection +
         R"(,"attestation":"none"})";
}

std::string GetOptions(const std::string& allow_id, const char* uv,
                       const char* transports = nullptr) {
  std::string allow = "[]";
  if (!allow_id.empty()) {
    allow = R"([{"type":"public-key","id":")" + allow_id + "\"";
    if (transports)
      allow += std::string(R"(,"transports":)") + transports;
    allow += "}]";
  }
  return std::string(
             R"({"challenge":"CQgHBgUEAwIBAP7__T4_QA","rpId":"example.com","timeout":30000,"userVerification":")") +
         uv + R"(","allowCredentials":)" + allow + "}";
}

std::string CredentialId(const std::string& envelope) {
  JsonValue v = Parse(envelope);
  const JsonValue* cred = Member(v, "credential");
  EXPECT(cred);
  return Text(*cred, "id");
}

laufey_webauthn_test_use_virtual_key_fn g_use_key = nullptr;
laufey_webauthn_test_set_virtual_key_flags_fn g_set_flags = nullptr;

void TestWithoutLibrary() {
  SetPasskeyLinuxLibraryForTesting("/nonexistent/liblaufey_webauthn.so");
  std::string reason;
  EXPECT(PasskeyCapabilitiesLinux(&reason) == 0);
  EXPECT(Contains(reason, "not installed"));
  std::string env = Run(LAUFEY_PASSKEY_GET, GetOptions("AQID", "discouraged"));
  EXPECT(Contains(env, R"("code":"not_supported")"));
  EXPECT(Contains(env, "not installed"));
}

void TestCeremonies() {
  std::atomic<int> statuses[8] = {};
  std::atomic<int> pin_asks{0};
  std::vector<std::string> pins;  // answers, in order ("" cancels)
  std::mutex pins_mutex;
  std::atomic<int> closed{0};
  PasskeyLinuxUi ui;
  ui.status = [&](uint32_t s) {
    if (s < 8)
      statuses[s]++;
  };
  ui.pin = [&](int32_t, std::function<void(const char*)> reply) {
    pin_asks++;
    std::string answer;
    {
      std::lock_guard<std::mutex> lock(pins_mutex);
      if (!pins.empty()) {
        answer = pins.front();
        pins.erase(pins.begin());
      }
    }
    // Answered from another thread, as a dialog would be.
    std::thread([reply, answer] {
      reply(answer.empty() ? nullptr : answer.c_str());
    }).detach();
  };
  ui.closed = [&] { closed++; };
  SetPasskeyLinuxUiForTesting(&ui);

  std::string reason;
  g_use_key(nullptr, 0);
  EXPECT(PasskeyCapabilitiesLinux(&reason) == LAUFEY_PASSKEY_SECURITY_KEYS);
  EXPECT(reason.empty());

  // Register, then sign in with it.
  std::string env =
      Run(LAUFEY_PASSKEY_CREATE, CreateOptions("discouraged", "discouraged"));
  std::printf("create: %.160s...\n", env.c_str());
  EXPECT(Contains(env, R"("ok":true)"));
  EXPECT(Contains(env, R"("authenticatorAttachment":"cross-platform")"));
  EXPECT(Contains(env, R"("attestationObject":")"));
  EXPECT(Contains(env, "\"usb\""));
  EXPECT(statuses[LAUFEY_WEBAUTHN_STATUS_TOUCH_KEY] > 0);
  EXPECT(closed == 1);
  std::string id = CredentialId(env);
  EXPECT(!id.empty());
  JsonValue created = Parse(env);
  const JsonValue* response =
      Member(*Member(created, "credential"), "response");
  std::vector<uint8_t> client_data;
  EXPECT(Base64UrlDecode(Text(*response, "clientDataJSON"), &client_data));
  std::string client(client_data.begin(), client_data.end());
  EXPECT(client == BuildPasskeyClientDataJson("webauthn.create",
                                              "AAECAwQFBgcICQoLDA0ODw",
                                              "example.com"));

  env = Run(LAUFEY_PASSKEY_GET, GetOptions(id, "discouraged", R"(["usb"])"));
  EXPECT(Contains(env, R"("ok":true)"));
  EXPECT(CredentialId(env) == id);
  EXPECT(Contains(env, R"("authenticatorData":")"));
  EXPECT(Contains(env, R"("signature":")"));
  JsonValue got = Parse(env);
  response = Member(*Member(got, "credential"), "response");
  EXPECT(Base64UrlDecode(Text(*response, "clientDataJSON"), &client_data));
  EXPECT(std::string(client_data.begin(), client_data.end()) ==
         BuildPasskeyClientDataJson("webauthn.get", "CQgHBgUEAwIBAP7__T4_QA",
                                    "example.com"));

  // Requests Linux refuses before the library runs.
  env = Run(LAUFEY_PASSKEY_CREATE,
            CreateOptions("required", "required", "platform"));
  EXPECT(Contains(env, R"("code":"not_supported")"));
  EXPECT(Contains(env, "platform authenticator"));
  env = Run(LAUFEY_PASSKEY_GET,
            GetOptions(id, "discouraged", R"(["internal","hybrid"])"));
  EXPECT(Contains(env, R"("code":"not_supported")"));

  // A key with a PIN: asked through the UI, wrong first, then right; the
  // credential is discoverable (no allow list) with its user handle.
  g_use_key("4321", 0);
  pins = {"1111", "4321"};
  pin_asks = 0;
  env = Run(LAUFEY_PASSKEY_CREATE, CreateOptions("required", "required"));
  EXPECT(Contains(env, R"("ok":true)"));
  EXPECT(pin_asks == 2);
  pins = {"4321"};
  env = Run(LAUFEY_PASSKEY_GET, GetOptions("", "required"));
  EXPECT(Contains(env, R"("ok":true)"));
  EXPECT(Contains(env, R"("userHandle":"dXNlcl8x")"));
  // The PIN entry cancelled.
  pins = {""};
  env = Run(LAUFEY_PASSKEY_GET, GetOptions("", "required"));
  EXPECT(Contains(env, R"("code":"cancelled")"));

  // No key plugged in: the user is asked to insert one; the anchor window
  // closes meanwhile -> cancelled at once.
  g_use_key(nullptr, 2 /* absent */);
  int insert_before = statuses[LAUFEY_WEBAUTHN_STATUS_INSERT_KEY];
  static int window;
  auto start = Clock::now();
  env = Run(
      LAUFEY_PASSKEY_CREATE, CreateOptions("discouraged", "discouraged"),
      &window, [&] {
        auto deadline = Clock::now() + std::chrono::seconds(10);
        while (statuses[LAUFEY_WEBAUTHN_STATUS_INSERT_KEY] == insert_before &&
               Clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        PasskeyWindowClosing(&window);
      });
  double took = std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("window closed: %s in %.2f s\n", env.c_str(), took);
  EXPECT(Contains(env, R"("code":"cancelled")"));
  EXPECT(statuses[LAUFEY_WEBAUTHN_STATUS_INSERT_KEY] > insert_before);
  EXPECT(took < 5);

  // ... and nobody plugs one in: the options' timeout (1 s).
  start = Clock::now();
  env = Run(LAUFEY_PASSKEY_CREATE,
            CreateOptions("discouraged", "discouraged", nullptr, 1000));
  took = std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("timeout: %s in %.2f s\n", env.c_str(), took);
  EXPECT(Contains(env, R"("code":"timeout")"));
  EXPECT(took >= 0.9 && took < 5);

  SetPasskeyLinuxUiForTesting(nullptr);
  g_use_key(nullptr, 0xffffffffu);
}

}  // namespace

int main() {
  TestCanonicalOptions();
  TestReachable();
  TestEnvelopes();
  TestWithoutLibrary();
  std::printf("pure parts and the missing library: ok\n");

  const char* library = std::getenv("LAUFEY_TEST_WEBAUTHN_LIBRARY");
  bool require =
      std::getenv("LAUFEY_TEST_REQUIRE_WEBAUTHN") &&
      std::strcmp(std::getenv("LAUFEY_TEST_REQUIRE_WEBAUTHN"), "1") == 0;
  if (!library || !*library) {
    if (require) {
      std::fprintf(stderr, "LAUFEY_TEST_WEBAUTHN_LIBRARY is not set\n");
      return 1;
    }
    std::printf("ceremonies skipped: no LAUFEY_TEST_WEBAUTHN_LIBRARY\n");
    return 0;
  }
  SetPasskeyLinuxLibraryForTesting(library);
  std::string reason;
  PasskeyCapabilitiesLinux(&reason);
  void* handle = PasskeyLinuxLibraryHandleForTesting();
  EXPECT(handle);
  g_use_key = reinterpret_cast<laufey_webauthn_test_use_virtual_key_fn>(
      dlsym(handle, "laufey_webauthn_test_use_virtual_key"));
  g_set_flags = reinterpret_cast<laufey_webauthn_test_set_virtual_key_flags_fn>(
      dlsym(handle, "laufey_webauthn_test_set_virtual_key_flags"));
  if (!g_use_key || !g_set_flags) {
    std::fprintf(stderr, "%s was built without the virt-test feature\n",
                 library);
    return 1;
  }
  TestCeremonies();
  std::printf("laufey_passkey_linux_test: ok\n");
  std::fflush(stdout);
  std::_Exit(0);
}
