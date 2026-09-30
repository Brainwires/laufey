// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "custom_schemes.h"

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <set>

#include "laufey_scheme_registry.h"
#include "scheme_handler.h"

namespace laufey_schemes {

const char kSwitch[] = "laufey-custom-schemes";
const char kEnv[] = "LAUFEY_CUSTOM_SCHEMES";

namespace {

std::vector<std::string> ComputeDeclared() {
  std::vector<std::string> out;
  std::set<std::string> seen;
  auto append = [&](const std::string& list, const char* source) {
    std::vector<std::string> rejected;
    for (const std::string& scheme :
         laufey_common::ParseSchemeList(list, &rejected)) {
      if (seen.insert(scheme).second) {
        out.push_back(scheme);
      }
    }
    for (const std::string& bad : rejected) {
      std::cerr << "laufey: ignoring invalid URL scheme name \"" << bad
                << "\" in " << source << std::endl;
    }
  };

  // "app" is always served; it comes first so it stays the default.
  append(LAUFEY_APP_SCHEME, "the built-in scheme list");

  CefRefPtr<CefCommandLine> command_line =
      CefCommandLine::GetGlobalCommandLine();
  if (command_line && command_line->HasSwitch(kSwitch)) {
    append(command_line->GetSwitchValue(kSwitch).ToString(),
           "--laufey-custom-schemes");
  }
  if (const char* env = std::getenv(kEnv)) {
    append(env, "LAUFEY_CUSTOM_SCHEMES");
  }
  return out;
}

}  // namespace

std::vector<std::string> Declared() {
  // Computed once per process: the command line and environment are fixed by
  // the time OnRegisterCustomSchemes runs, and the warnings above should not
  // repeat on every call.
  static const std::vector<std::string> declared = ComputeDeclared();
  return declared;
}

bool IsDeclared(const std::string& scheme) {
  std::string normalized = laufey_common::NormalizeSchemeName(scheme);
  for (const std::string& declared : Declared()) {
    if (declared == normalized) {
      return true;
    }
  }
  return false;
}

void RegisterAll(CefRawPtr<CefSchemeRegistrar> registrar) {
  for (const std::string& scheme : Declared()) {
    registrar->AddCustomScheme(scheme, CEF_SCHEME_OPTION_STANDARD |
                                           CEF_SCHEME_OPTION_SECURE |
                                           CEF_SCHEME_OPTION_CORS_ENABLED |
                                           CEF_SCHEME_OPTION_FETCH_ENABLED);
  }
}

void ForwardToChild(CefRefPtr<CefCommandLine> command_line) {
  if (!command_line || command_line->HasSwitch(kSwitch)) {
    return;
  }
  std::string joined;
  for (const std::string& scheme : Declared()) {
    if (scheme == LAUFEY_APP_SCHEME) {
      continue;  // always declared; no need to forward it
    }
    if (!joined.empty()) {
      joined += ',';
    }
    joined += scheme;
  }
  if (!joined.empty()) {
    command_line->AppendSwitchWithValue(kSwitch, joined);
  }
}

}  // namespace laufey_schemes
