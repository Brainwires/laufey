// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The secure store on Linux (API 47): the Secret Service through libsecret,
// with the state around it read over plain D-Bus first so that a locked
// keyring or a missing provider is reported, never mistaken for "not found"
// and never waited on forever. See laufey_secret_store.h.

#include "laufey_secret_store.h"

#include <dlfcn.h>
#include <gio/gio.h>
#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "laufey_platform_features.h"

namespace laufey_common {

namespace {

constexpr char kSecretsName[] = "org.freedesktop.secrets";
constexpr char kSecretsPath[] = "/org/freedesktop/secrets";
constexpr char kServiceInterface[] = "org.freedesktop.Secret.Service";
constexpr char kDefaultCollection[] =
    "/org/freedesktop/secrets/aliases/default";
// Names KWallet runs under (Plasma 6 serves the Secret Service from
// ksecretd when that is enabled).
constexpr const char* kKWalletNames[] = {
    "org.kde.kwalletd6",
    "org.kde.kwalletd5",
    "org.kde.kwalletd",
    "org.kde.ksecretd",
};
// Bus reads that answer at once (never a prompt).
constexpr int kQuickTimeoutMs = 2000;

// --- libsecret, loaded at run time ------------------------------------------

// libsecret's public SecretSchema (secret-schema.h), laid out as it is.
enum SchemaFlags { kSchemaNone = 0, kSchemaDontMatchName = 1 << 1 };
struct SchemaAttribute {
  const gchar* name;
  int type;  // SECRET_SCHEMA_ATTRIBUTE_STRING = 0
};
struct Schema {
  const gchar* name;
  int flags;
  SchemaAttribute attributes[32];
  gint reserved;
  gpointer reserved1;
  gpointer reserved2;
  gpointer reserved3;
  gpointer reserved4;
  gpointer reserved5;
  gpointer reserved6;
  gpointer reserved7;
};

// service + account, matched whatever wrote the item (secret-tool writes no
// xdg:schema; DONT_MATCH_NAME neither matches nor writes one).
const Schema kSchema = {"dev.laufey.SecureStore",
                        kSchemaDontMatchName,
                        {{"service", 0}, {"account", 0}, {nullptr, 0}},
                        0,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr};

struct Libsecret {
  gchar* (*lookupv_sync)(const Schema*, GHashTable*, GCancellable*, GError**);
  gboolean (*storev_sync)(const Schema*, GHashTable*, const gchar*,
                          const gchar*, const gchar*, GCancellable*, GError**);
  gboolean (*clearv_sync)(const Schema*, GHashTable*, GCancellable*, GError**);
  void (*password_free)(gchar*);
};

std::string g_libsecret_path = "libsecret-1.so.0";

const Libsecret* LoadLibsecret() {
  static Libsecret api;
  static const Libsecret* loaded = []() -> const Libsecret* {
    void* lib = dlopen(g_libsecret_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      return nullptr;
    api.lookupv_sync = reinterpret_cast<decltype(api.lookupv_sync)>(
        dlsym(lib, "secret_password_lookupv_sync"));
    api.storev_sync = reinterpret_cast<decltype(api.storev_sync)>(
        dlsym(lib, "secret_password_storev_sync"));
    api.clearv_sync = reinterpret_cast<decltype(api.clearv_sync)>(
        dlsym(lib, "secret_password_clearv_sync"));
    api.password_free = reinterpret_cast<decltype(api.password_free)>(
        dlsym(lib, "secret_password_free"));
    if (!api.lookupv_sync || !api.storev_sync || !api.clearv_sync ||
        !api.password_free) {
      return nullptr;
    }
    return &api;
  }();
  return loaded;
}

// libsecret's shared service object isn't meant for concurrent sync calls
// from several threads: one call at a time.
std::mutex& CallMutex() {
  static std::mutex m;
  return m;
}

// --- The session bus ---------------------------------------------------------

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

GDBusConnection* SessionBus() {
  bool have = !Env("DBUS_SESSION_BUS_ADDRESS").empty();
  if (!have && !Env("XDG_RUNTIME_DIR").empty()) {
    struct stat st;
    std::string bus = Env("XDG_RUNTIME_DIR") + "/bus";
    have = stat(bus.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
  }
  if (!have)
    return nullptr;  // never autolaunch a bus
  return g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
}

GVariant* BusCall(GDBusConnection* bus, const char* method, GVariant* args,
                  const char* type) {
  return g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", method, args, G_VARIANT_TYPE(type),
      G_DBUS_CALL_FLAGS_NONE, kQuickTimeoutMs, nullptr, nullptr);
}

bool HasOwner(GDBusConnection* bus, const char* name) {
  GVariant* r = BusCall(bus, "NameHasOwner", g_variant_new("(s)", name), "(b)");
  if (!r)
    return false;
  gboolean owned = FALSE;
  g_variant_get(r, "(b)", &owned);
  g_variant_unref(r);
  return owned;
}

std::set<std::string> Activatable(GDBusConnection* bus) {
  std::set<std::string> names;
  GVariant* r = BusCall(bus, "ListActivatableNames", nullptr, "(as)");
  if (!r)
    return names;
  GVariantIter* iter = nullptr;
  const gchar* name = nullptr;
  g_variant_get(r, "(as)", &iter);
  while (g_variant_iter_next(iter, "&s", &name))
    names.insert(name);
  g_variant_iter_free(iter);
  g_variant_unref(r);
  return names;
}

// The process that owns org.freedesktop.secrets, as a provider name.
std::string OwnerProvider(GDBusConnection* bus) {
  GVariant* r = BusCall(bus, "GetConnectionUnixProcessID",
                        g_variant_new("(s)", kSecretsName), "(u)");
  if (!r)
    return "";
  guint32 pid = 0;
  g_variant_get(r, "(u)", &pid);
  g_variant_unref(r);
  std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
  std::string name;
  std::getline(comm, name);
  return SecretProviderFromComm(name);
}

struct Provider {
  bool running = false;      // owns org.freedesktop.secrets
  bool activatable = false;  // D-Bus can start one
  std::string name;          // SecretProviderFromComm's (running only)
  bool kwallet = false;      // KWallet runs or can start
};

Provider FindProvider(GDBusConnection* bus) {
  Provider p;
  std::set<std::string> activatable = Activatable(bus);
  p.running = HasOwner(bus, kSecretsName);
  p.activatable = activatable.count(kSecretsName) > 0;
  if (p.running)
    p.name = OwnerProvider(bus);
  for (const char* name : kKWalletNames)
    p.kwallet = p.kwallet || HasOwner(bus, name) || activatable.count(name);
  return p;
}

// SearchItems: how many matching items are unlocked / locked. Never prompts
// (it may start an activatable provider). False when the service didn't
// answer in time.
bool SearchItems(GDBusConnection* bus, const std::string& service,
                 const std::string& account, int timeout_ms, int* unlocked,
                 int* locked, std::string* error_text) {
  GVariantBuilder attrs;
  g_variant_builder_init(&attrs, G_VARIANT_TYPE("a{ss}"));
  g_variant_builder_add(&attrs, "{ss}", "service", service.c_str());
  g_variant_builder_add(&attrs, "{ss}", "account", account.c_str());
  GError* error = nullptr;
  GVariant* r = g_dbus_connection_call_sync(
      bus, kSecretsName, kSecretsPath, kServiceInterface, "SearchItems",
      g_variant_new("(a{ss})", &attrs), G_VARIANT_TYPE("(aoao)"),
      G_DBUS_CALL_FLAGS_NONE, timeout_ms, nullptr, &error);
  if (!r) {
    if (error_text)
      *error_text = error ? error->message : "no answer";
    g_clear_error(&error);
    return false;
  }
  GVariant* u = g_variant_get_child_value(r, 0);
  GVariant* l = g_variant_get_child_value(r, 1);
  *unlocked = static_cast<int>(g_variant_n_children(u));
  *locked = static_cast<int>(g_variant_n_children(l));
  g_variant_unref(u);
  g_variant_unref(l);
  g_variant_unref(r);
  return true;
}

// The default collection's Locked property: 1 locked, 0 unlocked, -1 none
// (creating one means a prompt too) or no answer.
int DefaultCollectionLocked(GDBusConnection* bus, int timeout_ms) {
  GVariant* r = g_dbus_connection_call_sync(
      bus, kSecretsName, kDefaultCollection, "org.freedesktop.DBus.Properties",
      "Get",
      g_variant_new("(ss)", "org.freedesktop.Secret.Collection", "Locked"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, timeout_ms, nullptr,
      nullptr);
  if (!r)
    return -1;
  GVariant* inner = nullptr;
  g_variant_get(r, "(v)", &inner);
  int locked = -1;
  if (inner && g_variant_is_of_type(inner, G_VARIANT_TYPE_BOOLEAN))
    locked = g_variant_get_boolean(inner) ? 1 : 0;
  if (inner)
    g_variant_unref(inner);
  g_variant_unref(r);
  return locked;
}

// Someone could answer an unlock prompt here (the platform probe's answer).
bool PrompterHere() {
  PlatformFeatures f;
  ProbeSecretService(&f);
  return f.secret_prompter;
}

GHashTable* Attributes(const std::string& service, const std::string& account) {
  GHashTable* t = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_insert(t, const_cast<char*>("service"),
                      const_cast<char*>(service.c_str()));
  g_hash_table_insert(t, const_cast<char*>("account"),
                      const_cast<char*>(account.c_str()));
  return t;
}

// Runs `op` with a cancellable that is cancelled after `timeout_ms`.
// Returns whether the deadline cancelled it.
template <typename Op>
bool WithDeadline(uint32_t timeout_ms, Op op) {
  GCancellable* cancel = g_cancellable_new();
  std::mutex m;
  std::condition_variable cv;
  bool finished = false;
  bool fired = false;
  std::thread timer([&] {
    std::unique_lock<std::mutex> lock(m);
    if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [&] { return finished; })) {
      fired = true;
      g_cancellable_cancel(cancel);
    }
  });
  op(cancel);
  {
    std::lock_guard<std::mutex> lock(m);
    finished = true;
  }
  cv.notify_all();
  timer.join();
  g_object_unref(cancel);
  return fired;
}

uint32_t Elapsed(std::chrono::steady_clock::time_point start) {
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

std::string NoAnswerReason(const Provider& p, const std::string& detail) {
  std::string who = p.name.empty() ? "the Secret Service"
                                   : "the Secret Service (" + p.name + ")";
  return who + " did not answer (" + detail + ")";
}

// What every call checks first: a bus, libsecret, a provider. Returns the
// bus (owned) or null with `reason`.
GDBusConnection* Prepare(Provider* provider, std::string* reason) {
  if (!LoadLibsecret()) {
    *reason =
        "libsecret isn't installed (libsecret-1.so.0: the libsecret-1-0 "
        "package on Debian / Ubuntu, libsecret on Fedora)";
    return nullptr;
  }
  GDBusConnection* bus = SessionBus();
  if (!bus) {
    *reason = "no D-Bus session bus, so no Secret Service can answer";
    return nullptr;
  }
  *provider = FindProvider(bus);
  if (!provider->running && !provider->activatable) {
    *reason = NoSecretProviderReason(provider->kwallet);
    g_object_unref(bus);
    return nullptr;
  }
  return bus;
}

bool Valid(const std::string& service, const std::string& account,
           std::string* reason) {
  if (service.empty() || account.empty()) {
    *reason = "the service and the account must not be empty";
    return false;
  }
  if (!g_utf8_validate(service.c_str(), -1, nullptr) ||
      !g_utf8_validate(account.c_str(), -1, nullptr)) {
    *reason = "the service and the account must be UTF-8";
    return false;
  }
  return true;
}

uint32_t TimeoutOrDefault(uint32_t timeout_ms) {
  return timeout_ms ? timeout_ms : kSecretDefaultTimeoutMs;
}

int Remaining(uint32_t timeout_ms,
              std::chrono::steady_clock::time_point start) {
  uint32_t used = Elapsed(start);
  return used >= timeout_ms ? 1 : static_cast<int>(timeout_ms - used);
}

}  // namespace

std::string SecretProviderFromComm(const std::string& comm) {
  if (comm.empty())
    return "";
  if (comm.rfind("gnome-keyring", 0) == 0)
    return "gnome-keyring";
  if (comm == "ksecretd" || comm.rfind("kwalletd", 0) == 0)
    return "KWallet";
  if (comm.rfind("keepassxc", 0) == 0)
    return "KeePassXC";
  return comm;
}

std::string NoSecretProviderReason(bool kwallet_present) {
  if (kwallet_present) {
    return "no Secret Service provider: KWallet runs but doesn't serve the "
           "Secret Service (org.freedesktop.secrets); enable KWallet's Secret "
           "Service (System Settings > KWallet: \"Use KWallet for the Secret "
           "Service interface\"), or install gnome-keyring";
  }
  return "no Secret Service provider on the session bus "
         "(org.freedesktop.secrets): install gnome-keyring, or enable "
         "KWallet's Secret Service";
}

std::string LockedSecretReason(const std::string& provider, bool prompter,
                               bool timed_out, uint32_t waited_ms) {
  bool kwallet = provider == "KWallet";
  std::string what = kwallet ? "the KWallet wallet is closed"
                     : provider.empty()
                         ? std::string("the keyring is locked")
                         : "the " + provider + " keyring is locked";
  if (!prompter) {
    return what +
           " and no one here can answer its unlock prompt (no graphical "
           "session, or no prompter): " +
           (kwallet ? std::string("open the wallet")
                    : std::string("unlock it, or sign in to the desktop with "
                                  "a password so it unlocks at login"));
  }
  if (!timed_out) {
    return what + " and the unlock prompt was dismissed (or couldn't be shown)";
  }
  char secs[32];
  std::snprintf(secs, sizeof(secs), "%.0f", waited_ms / 1000.0);
  return what + " and its unlock prompt went unanswered for " + secs + " s";
}

void SetLibsecretPathForTesting(const char* path) {
  g_libsecret_path = path ? path : "libsecret-1.so.0";
}

SecretStatus SecretLookup(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* value, std::string* reason) {
  if (!Valid(service, account, reason))
    return SecretStatus::kFailed;
  timeout_ms = TimeoutOrDefault(timeout_ms);
  auto start = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> serial(CallMutex());
  Provider provider;
  GDBusConnection* bus = Prepare(&provider, reason);
  if (!bus)
    return SecretStatus::kUnavailable;
  int unlocked = 0, locked = 0;
  std::string error_text;
  bool answered =
      SearchItems(bus, service, account, Remaining(timeout_ms, start),
                  &unlocked, &locked, &error_text);
  g_object_unref(bus);
  if (!answered) {
    *reason = NoAnswerReason(provider, error_text);
    return SecretStatus::kUnavailable;
  }
  if (unlocked == 0 && locked == 0)
    return SecretStatus::kNotFound;
  bool prompter = PrompterHere();
  if (unlocked == 0 && !prompter) {
    *reason = LockedSecretReason(provider.name, false, false, 0);
    return SecretStatus::kUnavailable;
  }
  const Libsecret* lib = LoadLibsecret();
  gchar* secret = nullptr;
  GError* error = nullptr;
  GHashTable* attrs = Attributes(service, account);
  bool timed_out =
      WithDeadline(static_cast<uint32_t>(Remaining(timeout_ms, start)),
                   [&](GCancellable* c) {
                     secret = lib->lookupv_sync(&kSchema, attrs, c, &error);
                   });
  g_hash_table_unref(attrs);
  if (secret) {
    *value = secret;
    lib->password_free(secret);
    g_clear_error(&error);
    return SecretStatus::kOk;
  }
  SecretStatus status = SecretStatus::kUnavailable;
  if (timed_out && unlocked > 0) {
    *reason = NoAnswerReason(provider, "no answer in time");
  } else if (timed_out || (!error && locked > 0)) {
    // The unlock prompt was dismissed or never answered.
    *reason =
        LockedSecretReason(provider.name, true, timed_out, Elapsed(start));
  } else if (error) {
    *reason = std::string("the Secret Service failed: ") + error->message;
  } else {
    status = SecretStatus::kNotFound;  // gone in between
  }
  g_clear_error(&error);
  return status;
}

SecretStatus SecretStore(const std::string& service, const std::string& account,
                         const std::string& label, const std::string& value,
                         uint32_t timeout_ms, std::string* reason) {
  if (!Valid(service, account, reason))
    return SecretStatus::kFailed;
  if (!g_utf8_validate(value.c_str(), static_cast<gssize>(value.size()),
                       nullptr) ||
      value.find('\0') != std::string::npos) {
    *reason = "the value must be UTF-8 text";
    return SecretStatus::kFailed;
  }
  timeout_ms = TimeoutOrDefault(timeout_ms);
  auto start = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> serial(CallMutex());
  Provider provider;
  GDBusConnection* bus = Prepare(&provider, reason);
  if (!bus)
    return SecretStatus::kUnavailable;
  int unlocked = 0, locked = 0;
  std::string error_text;
  bool answered =
      SearchItems(bus, service, account, Remaining(timeout_ms, start),
                  &unlocked, &locked, &error_text);
  int collection_locked =
      answered ? DefaultCollectionLocked(bus, kQuickTimeoutMs) : -1;
  g_object_unref(bus);
  if (!answered) {
    *reason = NoAnswerReason(provider, error_text);
    return SecretStatus::kUnavailable;
  }
  // A locked default collection, or none (creating one asks for a
  // password), means a prompt.
  bool prompter = PrompterHere();
  if (collection_locked != 0 && !prompter) {
    *reason = LockedSecretReason(provider.name, false, false, 0);
    return SecretStatus::kUnavailable;
  }
  const Libsecret* lib = LoadLibsecret();
  gboolean stored = FALSE;
  GError* error = nullptr;
  GHashTable* attrs = Attributes(service, account);
  bool timed_out =
      WithDeadline(static_cast<uint32_t>(Remaining(timeout_ms, start)),
                   [&](GCancellable* c) {
                     // Several items with these attributes (written before, in
                     // other collections): clear them, so the next read can't
                     // find an old one.
                     if (unlocked + locked > 1)
                       lib->clearv_sync(&kSchema, attrs, c, nullptr);
                     stored = lib->storev_sync(
                         &kSchema, attrs, nullptr,
                         label.empty() ? service.c_str() : label.c_str(),
                         value.c_str(), c, &error);
                   });
  g_hash_table_unref(attrs);
  if (stored) {
    g_clear_error(&error);
    return SecretStatus::kOk;
  }
  if (timed_out && collection_locked == 0) {
    *reason = NoAnswerReason(provider, "no answer in time");
  } else if (timed_out || (!error && collection_locked != 0)) {
    *reason =
        LockedSecretReason(provider.name, true, timed_out, Elapsed(start));
  } else {
    *reason = std::string("the Secret Service refused the write: ") +
              (error ? error->message : "no reason given");
  }
  g_clear_error(&error);
  return SecretStatus::kUnavailable;
}

SecretStatus SecretDelete(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* reason) {
  if (!Valid(service, account, reason))
    return SecretStatus::kFailed;
  timeout_ms = TimeoutOrDefault(timeout_ms);
  auto start = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> serial(CallMutex());
  Provider provider;
  GDBusConnection* bus = Prepare(&provider, reason);
  if (!bus)
    return SecretStatus::kUnavailable;
  int unlocked = 0, locked = 0;
  std::string error_text;
  bool answered =
      SearchItems(bus, service, account, Remaining(timeout_ms, start),
                  &unlocked, &locked, &error_text);
  if (!answered) {
    g_object_unref(bus);
    *reason = NoAnswerReason(provider, error_text);
    return SecretStatus::kUnavailable;
  }
  if (unlocked == 0 && locked == 0) {
    g_object_unref(bus);
    return SecretStatus::kOk;  // nothing to delete
  }
  bool prompter = PrompterHere();
  if (locked > 0 && !prompter) {
    g_object_unref(bus);
    *reason = LockedSecretReason(provider.name, false, false, 0);
    return SecretStatus::kUnavailable;
  }
  const Libsecret* lib = LoadLibsecret();
  GError* error = nullptr;
  GHashTable* attrs = Attributes(service, account);
  bool timed_out = WithDeadline(
      static_cast<uint32_t>(Remaining(timeout_ms, start)),
      [&](GCancellable* c) { lib->clearv_sync(&kSchema, attrs, c, &error); });
  g_hash_table_unref(attrs);
  // Whatever libsecret says, the store decides: anything left (a locked item
  // whose unlock was dismissed) is not deleted.
  int left_unlocked = 0, left_locked = 0;
  bool checked = SearchItems(bus, service, account, kQuickTimeoutMs,
                             &left_unlocked, &left_locked, nullptr);
  g_object_unref(bus);
  if (checked && left_unlocked == 0 && left_locked == 0) {
    g_clear_error(&error);
    return SecretStatus::kOk;
  }
  if (timed_out || left_locked > 0) {
    *reason =
        LockedSecretReason(provider.name, true, timed_out, Elapsed(start));
  } else {
    *reason = std::string("the Secret Service didn't delete it") +
              (error ? std::string(": ") + error->message : std::string());
  }
  g_clear_error(&error);
  return SecretStatus::kUnavailable;
}

namespace {

char* Dup(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out)
    std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

int Answer(SecretStatus status, const std::string& reason, char** reason_out) {
  if (reason_out)
    *reason_out = reason.empty() ? nullptr : Dup(reason);
  return static_cast<int>(status);
}

}  // namespace

int SecretLookupForAbi(const char* service, const char* account,
                       uint32_t timeout_ms, char** value, char** reason) {
  if (value)
    *value = nullptr;
  std::string v, r;
  SecretStatus status = SecretLookup(
      service ? service : "", account ? account : "", timeout_ms, &v, &r);
  if (status == SecretStatus::kOk && value)
    *value = Dup(v);
  return Answer(status, r, reason);
}

int SecretStoreForAbi(const char* service, const char* account,
                      const char* label, const char* value, uint32_t timeout_ms,
                      char** reason) {
  std::string r;
  if (!value) {
    return Answer(SecretStatus::kFailed, "the value must not be NULL", reason);
  }
  SecretStatus status =
      SecretStore(service ? service : "", account ? account : "",
                  label ? label : "", value, timeout_ms, &r);
  return Answer(status, r, reason);
}

int SecretDeleteForAbi(const char* service, const char* account,
                       uint32_t timeout_ms, char** reason) {
  std::string r;
  SecretStatus status = SecretDelete(service ? service : "",
                                     account ? account : "", timeout_ms, &r);
  return Answer(status, r, reason);
}

}  // namespace laufey_common
