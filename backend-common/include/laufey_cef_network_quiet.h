// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The CEF backend makes no network request of its own: every request a
// laufey app sends is one its pages (or its runtime) asked for. See
// docs/backends.md, "No network requests of its own".
//
// laufey's browsers are Chrome-style, so Chrome's profile services run in
// them, and several of those contact Google at startup even with
// --disable-background-networking. Found from net logs of a fresh launch
// (each request's traffic annotation names the feature):
//
//   clients2.google.com/time/1/current   network_time_component
//                                        (NetworkTimeServiceQuerying)
//   www.google.com/async/folae           aim_eligibility_fetch
//                                        (AimEnabled: AI Mode eligibility)
//   www.google.com/ (preconnects)        the default search engine
//                                        preconnector (PreconnectToSearch)
//   accounts.google.com/ListAccounts     gaia_auth_list_accounts: the
//                                        Google accounts in the cookie jar,
//                                        asked for by signin metrics
//                                        services at profile start
//   redirector.gvt1.com/edgedl/chrome/   spellcheck_hunspell_dictionary
//     dict/*.bdic                        (Linux: Hunspell dictionaries are
//                                        downloaded on first use)
//   update.googleapis.com,               update_client: the component
//     edgedl.me.gvt1.com (after 60 s)    updater's checks and downloads
//   update.googleapis.com,               update_client (Chromium 156+): the
//     edgedl.me.gvt1.com (at once)       on-device model manifest broker's
//                                        manifest component, registered
//                                        past --disable-component-update
//
// The host turns the first three features off (MergeQuietDisabledFeatures),
// points Chrome's Google-accounts origin at a port Chromium refuses to
// connect to (kCefQuietGaiaUrl: the requests fail before any DNS lookup or
// socket), passes --disable-component-update and an empty
// --optimization-guide-manifest-override, and on Linux keeps only the
// spellcheck dictionaries whose file is already on disk
// (LocalHunspellDictionaries).

#ifndef LAUFEY_CEF_NETWORK_QUIET_H_
#define LAUFEY_CEF_NETWORK_QUIET_H_

#include <string>
#include <vector>

namespace laufey_common {

// The Chromium features the CEF host disables by default, in the order it
// appends them to --disable-features.
const std::vector<std::string>& CefQuietDisabledFeatures();

// The Chromium features CEF itself adds to --disable-features before the
// host's command line hook runs (CEF 156, branch 8078,
// libcef/common/chrome/chrome_main_delegate_cef.cc,
// ChromeMainDelegateCef::BasicStartupComplete, lines 360-400; the same list
// as CEF 149 and 150, branches 7827 and 7871): GlicActorUi,
// AutofillActorMode and LensOverlay crash Chrome's browser initialization
// under CEF, KillOnInvalidNavigationHeaders breaks CefFrame::LoadRequest with
// custom headers. A deep-link launch whose command line carries
// --disable-features drops that switch, CEF's entries included
// (LaufeyStripDeepLinkSwitches), and the browser then crashes at startup in
// tabs::TabInterface::GetFromContents, so MergeQuietDisabledFeatures adds
// them back. Not mirrored: EnableHangWatcher (CEF disables it only with
// multi_threaded_message_loop, which laufey doesn't use) and Windows'
// TcpSocketIoCompletionPortWin (off by default in Chromium 149 to 156, so
// CEF adds nothing). Re-check the list at every CEF update.
const std::vector<std::string>& CefOwnDisabledFeatures();

// The feature names in a --enable-features / --disable-features value: each
// comma-separated entry without a leading "*" and without a "<Trial" or
// ":param/value" suffix. Empty entries are skipped.
std::vector<std::string> FeatureListNames(const std::string& value);

// The --disable-features value with the quiet defaults added: `disable` (the
// value already on the command line, possibly empty) followed by each
// default (CefQuietDisabledFeatures, then CefOwnDisabledFeatures) that is
// neither already in it nor named in `enable`. A feature
// the app's own command line enables stays enabled. Returns `disable`
// unchanged when there is nothing to add.
std::string MergeQuietDisabledFeatures(const std::string& disable,
                                       const std::string& enable);

// The --gaia-url value the host sets when the command line has none: the
// origin of Chrome's Google-accounts requests (ListAccounts and the rest of
// the GAIA cookie checks). Port 9 is on Chromium's restricted-port list, so
// each such request ends with ERR_UNSAFE_PORT before a DNS lookup or a
// socket; "localhost" never leaves the machine even if that changed.
inline constexpr char kCefQuietGaiaUrl[] = "https://localhost:9";

// The switch the host sets, with an empty value, wherever it turns the
// component updater off (no --component-updater, and the app sets no override
// of its own): Chromium 156's on-device model manifest broker then reads its
// manifest from that (missing) file instead of registering and installing
// its manifest component with the component updater, which it does past
// --disable-component-update (optimization_guide_global_state.cc,
// CreateManifestDelegate).
inline constexpr char kCefQuietManifestOverrideSwitch[] =
    "optimization-guide-manifest-override";

// Of the spellcheck languages in `languages` (the profile's
// "spellcheck.dictionaries"), the ones whose Hunspell dictionary is among
// `file_names` (the names in the profile's Dictionaries folder): a file
// named "<language>-<major>-<minor>.bdic", compared without regard to case,
// as Chromium names them (en-US-10-2.bdic). Chromium downloads a missing
// one from Google, so the Linux host keeps only these.
std::vector<std::string> LocalHunspellDictionaries(
    const std::vector<std::string>& languages,
    const std::vector<std::string>& file_names);

// The Hunspell files to copy into the profile's Dictionaries folder from a
// folder of bundled ones (`source_files`, the names in that folder): for
// each language in `languages` that no name in `existing_files` serves (the
// test LocalHunspellDictionaries uses), the first source file that does.
// Only the languages in use, so a profile holds just the dictionaries it
// checks with. Call once per source folder, the app's own first, adding
// what it returned to `existing_files` before the next.
std::vector<std::string> DictionariesToSeed(
    const std::vector<std::string>& languages,
    const std::vector<std::string>& source_files,
    const std::vector<std::string>& existing_files);

}  // namespace laufey_common

#endif  // LAUFEY_CEF_NETWORK_QUIET_H_
