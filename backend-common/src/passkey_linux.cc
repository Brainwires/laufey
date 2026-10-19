// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Passkeys on Linux: security keys (USB / HID) through libwebauthn, in
// liblaufey_webauthn.so (webauthn-linux/), which this file dlopen()s from
// the backend's own directory on first use. Linux has no platform
// authenticator API, so this is the roaming half of what macOS and Windows
// offer: LAUFEY_PASSKEY_SECURITY_KEYS when the library loads and can list
// HID devices, nothing otherwise. See docs/passkeys.md.
//
// laufey's parser and the app's passkeyRpIds pin run first (PasskeyBegin);
// the library gets a canonical options document built from the parsed
// options, never the caller's text. Its UI requests (insert / touch the key,
// the PIN) are shown here with GTK, on the GTK thread: a dialog with a
// Cancel button while the key is wanted, and a PIN dialog, both transient
// for the anchor window when it is a GTK one (WebKitGTK).

#include <dlfcn.h>
#include <gtk/gtk.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json_reader.h"
#include "laufey_io.h"
#include "laufey_passkey.h"
#include "laufey_single_instance.h"
#include "laufey_webauthn_abi.h"

namespace laufey_common {

namespace {

using json::JsonReader;
using json::JsonValue;

// --- The library
// ----------------------------------------------------------------

struct Shim {
  void* handle = nullptr;
  laufey_webauthn_capabilities_fn capabilities = nullptr;
  laufey_webauthn_start_fn start = nullptr;
  laufey_webauthn_cancel_fn cancel = nullptr;
  laufey_webauthn_release_fn release = nullptr;
  laufey_webauthn_pin_reply_fn pin_reply = nullptr;
  std::string error;  // why it isn't there (empty when it is)
};

std::mutex g_shim_mutex;
std::string g_library_override;  // tests
Shim* g_shim = nullptr;

std::string ExecutableDirectory() {
  char path[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (n <= 0)
    return "";
  std::string exe(path, static_cast<size_t>(n));
  size_t slash = exe.rfind('/');
  return slash == std::string::npos ? "" : exe.substr(0, slash);
}

// Loaded once (again after a test changes the path). No environment
// variable picks the file: it is the one shipped beside the backend.
const Shim& LoadShim() {
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  if (g_shim)
    return *g_shim;
  auto* s = new Shim();  // never freed: ceremonies may outlive any caller
  g_shim = s;
  std::string path = g_library_override;
  if (path.empty()) {
    std::string dir = ExecutableDirectory();
    if (dir.empty()) {
      s->error = "the backend's directory is unknown";
      return *s;
    }
    path = dir + "/" + LAUFEY_WEBAUTHN_LIBRARY;
  }
  if (access(path.c_str(), F_OK) != 0) {
    s->error = std::string(LAUFEY_WEBAUTHN_LIBRARY) +
               " is not installed beside the backend";
    return *s;
  }
  s->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!s->handle) {
    const char* e = dlerror();
    s->error = std::string(LAUFEY_WEBAUTHN_LIBRARY) +
               " can't be loaded: " + (e ? e : "unknown error");
    return *s;
  }
  auto abi = reinterpret_cast<laufey_webauthn_abi_fn>(
      dlsym(s->handle, "laufey_webauthn_abi"));
  s->capabilities = reinterpret_cast<laufey_webauthn_capabilities_fn>(
      dlsym(s->handle, "laufey_webauthn_capabilities"));
  s->start = reinterpret_cast<laufey_webauthn_start_fn>(
      dlsym(s->handle, "laufey_webauthn_start"));
  s->cancel = reinterpret_cast<laufey_webauthn_cancel_fn>(
      dlsym(s->handle, "laufey_webauthn_cancel"));
  s->release = reinterpret_cast<laufey_webauthn_release_fn>(
      dlsym(s->handle, "laufey_webauthn_release"));
  s->pin_reply = reinterpret_cast<laufey_webauthn_pin_reply_fn>(
      dlsym(s->handle, "laufey_webauthn_pin_reply"));
  if (!abi || !s->capabilities || !s->start || !s->cancel || !s->release ||
      !s->pin_reply || abi() != LAUFEY_WEBAUTHN_ABI_VERSION) {
    s->error = std::string(LAUFEY_WEBAUTHN_LIBRARY) +
               " speaks another ABI than this backend";
    s->start = nullptr;
  }
  return *s;
}

// --- Canonical options
// -----------------------------------------------------------

void AppendString(std::string* out, const std::string& s) {
  out->push_back('"');
  for (unsigned char c : SanitizeUtf8(s)) {
    switch (c) {
      case '"':
        *out += "\\\"";
        break;
      case '\\':
        *out += "\\\\";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

void AppendMember(std::string* out, const char* name, const std::string& v,
                  bool* first) {
  if (!*first)
    out->push_back(',');
  *first = false;
  AppendString(out, name);
  out->push_back(':');
  AppendString(out, v);
}

void AppendDescriptors(std::string* out,
                       const std::vector<PasskeyCredentialDescriptor>& list) {
  out->push_back('[');
  for (size_t i = 0; i < list.size(); i++) {
    if (i)
      out->push_back(',');
    *out += "{\"type\":\"public-key\",\"id\":";
    AppendString(out, Base64UrlEncode(list[i].id));
    if (!list[i].transports.empty()) {
      *out += ",\"transports\":[";
      for (size_t t = 0; t < list[i].transports.size(); t++) {
        if (t)
          out->push_back(',');
        AppendString(out, list[i].transports[t]);
      }
      out->push_back(']');
    }
    out->push_back('}');
  }
  out->push_back(']');
}

}  // namespace

std::string PasskeyLinuxCreationJson(const PasskeyCreationOptions& o,
                                     uint32_t timeout_ms) {
  std::string out = "{\"rp\":{";
  bool first = true;
  AppendMember(&out, "id", o.rp_id, &first);
  AppendMember(&out, "name", o.rp_name.empty() ? o.rp_id : o.rp_name, &first);
  out += "},\"user\":{";
  first = true;
  AppendMember(&out, "id", Base64UrlEncode(o.user_id), &first);
  AppendMember(&out, "name", o.user_name, &first);
  AppendMember(&out, "displayName", o.user_display_name, &first);
  out += "},\"challenge\":";
  AppendString(&out, o.challenge_b64url);
  out += ",\"pubKeyCredParams\":[";
  // None listed: WebAuthn's defaults (ES256, RS256).
  std::vector<int32_t> algorithms = o.algorithms;
  if (algorithms.empty())
    algorithms = {-7, -257};
  for (size_t i = 0; i < algorithms.size(); i++) {
    if (i)
      out.push_back(',');
    out += "{\"type\":\"public-key\",\"alg\":" + std::to_string(algorithms[i]) +
           "}";
  }
  out += "],\"timeout\":" + std::to_string(timeout_ms);
  out += ",\"authenticatorSelection\":{";
  first = true;
  AppendMember(&out, "residentKey",
               o.resident_key.empty() ? "discouraged" : o.resident_key, &first);
  AppendMember(&out, "userVerification",
               o.user_verification.empty() ? "preferred" : o.user_verification,
               &first);
  out += "},\"attestation\":";
  AppendString(&out, o.attestation.empty() ? "none" : o.attestation);
  out += ",\"excludeCredentials\":";
  AppendDescriptors(&out, o.exclude_credentials);
  out += "}";
  return out;
}

std::string PasskeyLinuxRequestJson(const PasskeyRequestOptions& o,
                                    uint32_t timeout_ms) {
  std::string out = "{\"challenge\":";
  AppendString(&out, o.challenge_b64url);
  out += ",\"rpId\":";
  AppendString(&out, o.rp_id);
  out += ",\"timeout\":" + std::to_string(timeout_ms);
  out += ",\"userVerification\":";
  AppendString(&out,
               o.user_verification.empty() ? "preferred" : o.user_verification);
  out += ",\"allowCredentials\":";
  AppendDescriptors(&out, o.allow_credentials);
  out += "}";
  return out;
}

bool PasskeyLinuxReachable(const std::vector<PasskeyCredentialDescriptor>& list,
                           std::string* why) {
  if (list.empty())
    return true;
  for (const auto& d : list) {
    if (d.transports.empty())
      return true;
    for (const auto& t : d.transports) {
      if (t == "usb")
        return true;
    }
  }
  if (why) {
    *why =
        "none of the allowed credentials can be used with a USB security "
        "key (their transports name only others)";
  }
  return false;
}

std::string PasskeyLinuxEnvelope(bool create, const std::string& result) {
  JsonValue root;
  std::string error;
  if (!JsonReader(result).ParseDocument(&root, &error) ||
      root.type != JsonValue::Type::kObject) {
    return PasskeyErrorEnvelope(kPasskeyUnknown,
                                "the security key library answered nonsense");
  }
  auto member = [&](const char* name) -> const JsonValue* {
    for (const auto& [k, v] : root.object) {
      if (k == name)
        return &v;
    }
    return nullptr;
  };
  auto text = [&](const char* name) {
    const JsonValue* v = member(name);
    return v && v->type == JsonValue::Type::kString ? v->string : std::string();
  };
  auto bytes = [&](const char* name, std::vector<uint8_t>* out) {
    std::string s = text(name);
    return !s.empty() && Base64UrlDecode(s, out);
  };
  const JsonValue* ok = member("ok");
  if (!ok || ok->type != JsonValue::Type::kBool || !ok->boolean) {
    std::string code = text("code");
    const char* known = kPasskeyUnknown;
    for (const char* c : {kPasskeyCancelled, kPasskeyInvalidRp,
                          kPasskeyNotSupported, kPasskeyTimeout}) {
      if (code == c)
        known = c;
    }
    std::string message = text("message");
    return PasskeyErrorEnvelope(
        known, message.empty() ? "the security key request failed" : message);
  }
  std::string attachment = text("attachment");
  if (attachment.empty())
    attachment = "cross-platform";
  if (create) {
    PasskeyRegistrationResult r;
    if (!bytes("credentialId", &r.credential_id) ||
        !bytes("clientDataJSON", &r.client_data_json) ||
        !bytes("attestationObject", &r.attestation_object)) {
      return PasskeyErrorEnvelope(
          kPasskeyUnknown, "the security key's registration is incomplete");
    }
    r.attachment = attachment;
    if (const JsonValue* t = member("transports")) {
      if (t->type == JsonValue::Type::kArray) {
        for (const auto& v : t->array) {
          if (v.type == JsonValue::Type::kString)
            r.transports.push_back(v.string);
        }
      }
    }
    if (r.transports.empty())
      r.transports.push_back("usb");
    return PasskeyRegistrationEnvelope(r);
  }
  PasskeyAssertionResult r;
  if (!bytes("credentialId", &r.credential_id) ||
      !bytes("clientDataJSON", &r.client_data_json) ||
      !bytes("authenticatorData", &r.authenticator_data) ||
      !bytes("signature", &r.signature)) {
    return PasskeyErrorEnvelope(kPasskeyUnknown,
                                "the security key's assertion is incomplete");
  }
  bytes("userHandle", &r.user_handle);
  r.attachment = attachment;
  return PasskeyAssertionEnvelope(r);
}

namespace {

// --- The UI
// ------------------------------------------------------------------------

const PasskeyLinuxUi* g_test_ui = nullptr;

// One ceremony's library side and UI. Kept alive by the library's callbacks
// (a reference taken at start, dropped when `done` has been handled).
struct LinuxCeremony : std::enable_shared_from_this<LinuxCeremony> {
  std::shared_ptr<PasskeyCeremony> ceremony;
  laufey_webauthn_callbacks_t callbacks = {};
  laufey_webauthn_handle_t* handle = nullptr;
  std::string rp_id;
  GtkWidget* parent = nullptr;  // a GtkWindow, or null (GTK thread only)
  // GTK thread only.
  GtkWidget* status_dialog = nullptr;
  GtkWidget* pin_dialog = nullptr;
  laufey_webauthn_pin_reply_t* pending_pin = nullptr;
  bool pin_retry = false;
  bool closed = false;
  std::shared_ptr<LinuxCeremony> self;  // until `done`
  std::mutex mutex;
  bool done = false;
};

const char* StatusText(uint32_t status) {
  switch (status) {
    case LAUFEY_WEBAUTHN_STATUS_INSERT_KEY:
      return "Insert your security key";
    case LAUFEY_WEBAUTHN_STATUS_SELECT_KEY:
      return "Touch the security key you want to use";
    default:
      return "Touch your security key";
  }
}

void AnswerPin(LinuxCeremony* lc, const char* pin) {
  if (!lc->pending_pin)
    return;
  laufey_webauthn_pin_reply_t* reply = lc->pending_pin;
  lc->pending_pin = nullptr;
  LoadShim().pin_reply(reply, pin);
}

void ClosePinDialog(LinuxCeremony* lc) {
  if (lc->pin_dialog) {
    GtkWidget* d = lc->pin_dialog;
    lc->pin_dialog = nullptr;
    gtk_widget_destroy(d);
  }
  AnswerPin(lc, nullptr);
}

void CloseDialogs(LinuxCeremony* lc) {
  lc->closed = true;
  ClosePinDialog(lc);
  if (lc->status_dialog) {
    GtkWidget* d = lc->status_dialog;
    lc->status_dialog = nullptr;
    gtk_widget_destroy(d);
  }
}

// A dialog destroyed by GTK (with its parent window, DESTROY_WITH_PARENT):
// forgotten, and a PIN request it held is answered with a cancel.
void OnDialogDestroyed(GtkWidget* widget, gpointer data) {
  auto* lc = static_cast<LinuxCeremony*>(data);
  if (lc->status_dialog == widget)
    lc->status_dialog = nullptr;
  if (lc->pin_dialog == widget) {
    lc->pin_dialog = nullptr;
    AnswerPin(lc, nullptr);
  }
}

void OnStatusResponse(GtkDialog*, gint, gpointer data) {
  auto* lc = static_cast<LinuxCeremony*>(data);
  // Cancel (or the dialog closed): the request ends as the user's.
  lc->ceremony->Abort(kPasskeyCancelled, "the user cancelled the request");
  CloseDialogs(lc);
}

void ShowStatus(LinuxCeremony* lc, uint32_t status) {
  if (lc->closed)
    return;
  if (status == LAUFEY_WEBAUTHN_STATUS_PIN_RETRY) {
    lc->pin_retry = true;
    return;
  }
  const char* secondary = lc->rp_id.c_str();
  if (!lc->status_dialog) {
    GtkWidget* d =
        gtk_message_dialog_new(lc->parent ? GTK_WINDOW(lc->parent) : nullptr,
                               GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_INFO,
                               GTK_BUTTONS_CANCEL, "%s", StatusText(status));
    gtk_window_set_title(GTK_WINDOW(d), "Security key");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s",
                                             secondary);
    g_signal_connect(d, "response", G_CALLBACK(OnStatusResponse), lc);
    g_signal_connect(d, "destroy", G_CALLBACK(OnDialogDestroyed), lc);
    lc->status_dialog = d;
    gtk_widget_show_all(d);
  } else {
    g_object_set(lc->status_dialog, "text", StatusText(status), nullptr);
  }
}

void OnPinResponse(GtkDialog* dialog, gint response, gpointer data) {
  auto* lc = static_cast<LinuxCeremony*>(data);
  if (dialog != GTK_DIALOG(lc->pin_dialog))
    return;
  GtkWidget* entry = GTK_WIDGET(g_object_get_data(G_OBJECT(dialog), "entry"));
  if (response == GTK_RESPONSE_OK && entry) {
    std::string pin = gtk_entry_get_text(GTK_ENTRY(entry));
    AnswerPin(lc, pin.c_str());
    // Not kept: the entry and our copy are wiped.
    std::fill(pin.begin(), pin.end(), '\0');
    gtk_entry_set_text(GTK_ENTRY(entry), "");
  } else {
    AnswerPin(lc, nullptr);
  }
  GtkWidget* d = lc->pin_dialog;
  lc->pin_dialog = nullptr;
  gtk_widget_destroy(d);
}

void ShowPin(LinuxCeremony* lc, int32_t attempts_left) {
  if (lc->closed) {
    AnswerPin(lc, nullptr);
    return;
  }
  GtkWindow* parent = lc->parent ? GTK_WINDOW(lc->parent) : nullptr;
  GtkWidget* d = gtk_dialog_new_with_buttons(
      "Security key PIN", parent, GTK_DIALOG_DESTROY_WITH_PARENT, "_Cancel",
      GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, nullptr);
  gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
  GtkWidget* box = gtk_dialog_get_content_area(GTK_DIALOG(d));
  gtk_container_set_border_width(GTK_CONTAINER(box), 12);
  gtk_box_set_spacing(GTK_BOX(box), 8);
  std::string label = lc->pin_retry ? "The PIN was wrong. Enter your security "
                                      "key's PIN"
                                    : "Enter your security key's PIN";
  if (attempts_left >= 0 && attempts_left <= 3)
    label += " (" + std::to_string(attempts_left) + " attempts left)";
  label += "\nfor " + lc->rp_id;
  lc->pin_retry = false;
  gtk_container_add(GTK_CONTAINER(box), gtk_label_new(label.c_str()));
  GtkWidget* entry = gtk_entry_new();
  gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
  gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PIN);
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_container_add(GTK_CONTAINER(box), entry);
  g_object_set_data(G_OBJECT(d), "entry", entry);
  g_signal_connect(d, "response", G_CALLBACK(OnPinResponse), lc);
  g_signal_connect(d, "destroy", G_CALLBACK(OnDialogDestroyed), lc);
  lc->pin_dialog = d;
  gtk_widget_show_all(d);
}

// --- The library's callbacks (its thread)
// -----------------------------------------

void OnStatus(void* user_data, uint32_t status) {
  auto* lc = static_cast<LinuxCeremony*>(user_data);
  std::shared_ptr<LinuxCeremony> keep = lc->shared_from_this();
  if (g_test_ui) {
    if (g_test_ui->status)
      g_test_ui->status(status);
    return;
  }
  GtkRunAsync([keep, status] { ShowStatus(keep.get(), status); });
}

void OnPin(void* user_data, int32_t attempts_left,
           laufey_webauthn_pin_reply_t* reply) {
  auto* lc = static_cast<LinuxCeremony*>(user_data);
  std::shared_ptr<LinuxCeremony> keep = lc->shared_from_this();
  if (g_test_ui) {
    if (!g_test_ui->pin) {
      LoadShim().pin_reply(reply, nullptr);
      return;
    }
    // Exactly one answer, whatever the test does.
    auto answered = std::make_shared<std::atomic<bool>>(false);
    g_test_ui->pin(attempts_left, [reply, answered](const char* pin) {
      if (!answered->exchange(true))
        LoadShim().pin_reply(reply, pin);
    });
    return;
  }
  GtkRunAsync([keep, attempts_left, reply] {
    keep->pending_pin = reply;
    ShowPin(keep.get(), attempts_left);
  });
}

void OnDone(void* user_data, const char* result_json) {
  auto* lc = static_cast<LinuxCeremony*>(user_data);
  std::shared_ptr<LinuxCeremony> keep;
  {
    std::lock_guard<std::mutex> lock(lc->mutex);
    if (lc->done)
      return;
    lc->done = true;
    keep = std::move(lc->self);
  }
  std::string envelope = PasskeyLinuxEnvelope(lc->ceremony->is_create(),
                                              result_json ? result_json : "");
  lc->ceremony->Finish(envelope);
  if (g_test_ui) {
    if (g_test_ui->closed)
      g_test_ui->closed();
  } else {
    GtkRunAsync([keep] { CloseDialogs(keep.get()); });
  }
  // The handle (the library's thread holds its own reference).
  laufey_webauthn_handle_t* handle = nullptr;
  {
    std::lock_guard<std::mutex> lock(lc->mutex);
    handle = lc->handle;
    lc->handle = nullptr;
  }
  if (handle)
    LoadShim().release(handle);
}

}  // namespace

uint32_t PasskeyCapabilitiesLinux(std::string* reason) {
  const Shim& shim = LoadShim();
  if (!shim.start) {
    if (reason)
      *reason = shim.error;
    return 0;
  }
  char why[256] = {};
  uint32_t caps = shim.capabilities(why, sizeof(why));
  if (!(caps & LAUFEY_WEBAUTHN_CAP_SECURITY_KEYS)) {
    if (reason)
      *reason = why[0] ? why : "security keys can't be used here";
    return 0;
  }
  if (reason)
    reason->clear();
  return LAUFEY_PASSKEY_SECURITY_KEYS;
}

void PasskeyStartLinux(std::shared_ptr<PasskeyCeremony> ceremony,
                       void* gtk_window, const void* window_key) {
  const Shim& shim = LoadShim();
  if (!shim.start) {
    ceremony->Finish(PasskeyErrorEnvelope(kPasskeyNotSupported, shim.error));
    return;
  }
  bool create = ceremony->is_create();
  // Linux has no platform authenticator: a registration that asks for one
  // can't be met by a security key.
  if (create && ceremony->creation().authenticator_attachment == "platform") {
    ceremony->Finish(PasskeyErrorEnvelope(
        kPasskeyNotSupported,
        "Linux has no platform authenticator; only security keys "
        "(authenticatorAttachment \"cross-platform\" or none)"));
    return;
  }
  std::string why;
  if (!create &&
      !PasskeyLinuxReachable(ceremony->request().allow_credentials, &why)) {
    ceremony->Finish(PasskeyErrorEnvelope(kPasskeyNotSupported, why));
    return;
  }
  uint32_t timeout = ceremony->has_timeout() ? ceremony->timeout_ms()
                                             : kPasskeyDefaultTimeoutMs;
  std::string options =
      create ? PasskeyLinuxCreationJson(ceremony->creation(), timeout)
             : PasskeyLinuxRequestJson(ceremony->request(), timeout);
  auto lc = std::make_shared<LinuxCeremony>();
  lc->ceremony = ceremony;
  lc->rp_id = create ? ceremony->creation().rp_id : ceremony->request().rp_id;
  lc->parent = static_cast<GtkWidget*>(gtk_window);
  lc->self = lc;
  lc->callbacks.user_data = lc.get();
  lc->callbacks.status = OnStatus;
  lc->callbacks.pin = OnPin;
  lc->callbacks.done = OnDone;
  if (window_key)
    ceremony->SetWindowKey(window_key);
  // laufey's own timeout delivers `timeout` on time; the library gets a
  // backstop past it, so its answer is never a race with ours.
  ceremony->StartTimeout(timeout);
  laufey_webauthn_handle_t* handle = shim.start(
      create ? LAUFEY_PASSKEY_CREATE : LAUFEY_PASSKEY_GET, options.c_str(),
      lc->rp_id.c_str(), timeout + 5000, &lc->callbacks);
  if (!handle) {
    lc->self.reset();
    ceremony->Finish(PasskeyErrorEnvelope(
        kPasskeyUnknown, "the security key request couldn't start"));
    return;
  }
  bool finished;
  {
    std::lock_guard<std::mutex> lock(lc->mutex);
    finished = lc->done;
    if (!finished)
      lc->handle = handle;
  }
  // Already over (OnDone found no handle to free): free it here.
  if (finished)
    shim.release(handle);
  std::weak_ptr<LinuxCeremony> weak = lc;
  ceremony->SetCanceller([weak] {
    auto c = weak.lock();
    if (!c)
      return;
    std::lock_guard<std::mutex> lock(c->mutex);
    if (c->handle)
      LoadShim().cancel(c->handle);
  });
}

void SetPasskeyLinuxLibraryForTesting(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  g_library_override = path;
  g_shim = nullptr;  // the old one stays loaded (a ceremony may use it)
}

void SetPasskeyLinuxUiForTesting(const PasskeyLinuxUi* ui) {
  g_test_ui = ui;
}

void* PasskeyLinuxLibraryHandleForTesting() {
  return LoadShim().handle;
}

}  // namespace laufey_common
