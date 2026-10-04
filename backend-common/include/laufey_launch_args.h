// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The host executable's own command line: laufey's options, the end of
// options, and what a deep-link launch may carry.
//
// A registered URL scheme starts the app with the link as an argument. On
// Windows the shell substitutes the link for "%1" in the registered command
// without escaping it, so a link containing a '"' can close the quotes and
// add arguments of its own (Electron's CVE-2018-1000006). Two defences live
// here:
//
//  * "--" ends the options. Registrations run `"<exe>" -- "%1"`, and nothing
//    after "--" is ever read as an option: not by laufey (ParseHostOptions),
//    not by Chromium (its command line parser has the same terminator), not
//    by the runtime.
//  * A packaged app loads only the runtime library it ships (next to the
//    executable, or in its macOS bundle): never one a command line
//    (`--runtime`), the environment (LAUFEY_RUNTIME_PATH), the working
//    directory or a system directory names. ChooseRuntimePath decides.
//
// For CEF there is a third: a launch with a URL positional argument (a deep
// link) drops every Chromium switch that came from the process's own
// command line (DeepLinkSwitchesToStrip), so an old registration without the
// "--" still can't pass Chromium switches through a link.

#ifndef LAUFEY_LAUNCH_ARGS_H_
#define LAUFEY_LAUNCH_ARGS_H_

#include <functional>
#include <string>
#include <vector>

namespace laufey_common {

// laufey's options, read from argv[1..].
struct HostOptions {
  // From `--runtime <path>` / `--runtime=<path>`; "" if not given (or
  // ignored).
  std::string runtime_path;
};

// Read laufey's options from `args` (argv without the program), stopping at
// the first "--". `packaged`: ignore `--runtime` (see IsPackagedLaunch).
HostOptions ParseHostOptions(const std::vector<std::string>& args,
                             bool packaged);

// Whether `arg` is an absolute URL with a scheme other than `file`: an RFC
// 3986 scheme (a letter, then letters, digits, '+', '-', '.') of at least two
// characters followed by ':'. A Windows drive path ("C:\x") is not one.
bool IsUrlArgument(const std::string& arg);

// Whether `args` (argv without the program) carry a deep link: a positional
// argument (anything after "--", or before it an argument not starting with
// '-') for which IsUrlArgument holds.
bool IsDeepLinkLaunch(const std::vector<std::string>& args);

// The names of the Chromium switches in `args` (argv without the program),
// as Chromium's parser would see them: arguments before "--" that start with
// "--" or "-" (and "/" on Windows), the name ending at '='; lower-cased on
// Windows, where Chromium lower-cases switch names. Each name once.
std::vector<std::string> CommandLineSwitchNames(
    const std::vector<std::string>& args);

// What a CEF browser process removes from its command line before Chromium
// reads it: empty unless IsDeepLinkLaunch(args), else every switch name from
// CommandLineSwitchNames(args) not in `allowed` (an allow-list: a link-started
// app needs no switch from its command line).
std::vector<std::string> DeepLinkSwitchesToStrip(
    const std::vector<std::string>& args,
    const std::vector<std::string>& allowed);

// Whether the running executable is a packaged app: its launch file
// (laufey-launch.json) exists, or `has_colocated_runtime` (the backend found
// a runtime library next to the executable).
bool IsPackagedLaunch(bool has_colocated_runtime);

// The runtime library a host loads, and why.
struct RuntimeChoice {
  // The library to load; "" when none was found.
  std::string path;
  // Whether the host runs as a packaged app (see ChooseRuntimePath).
  bool packaged = false;
  // What was ignored, for stderr (without the "laufey: " prefix).
  std::vector<std::string> warnings;
};

// The pure step behind ResolveRuntimePath, exposed for tests. Reads nothing
// from the environment or the filesystem; `exists` says whether a path names
// a file.
//
//  * `args`: argv without the program (`--runtime` is read up to "--").
//  * `env_runtime_path`: LAUFEY_RUNTIME_PATH ("" when unset).
//  * `bundled`: where the app ships its runtime, in order: the library next
//    to the executable (LaufeyFindColocatedRuntime), a macOS bundle's
//    Contents/Frameworks or Contents/MacOS. Empty entries are skipped.
//  * `development`: a development host's fallbacks, in order (paths relative
//    to the working directory, system library directories).
//  * `has_launch_file`: the executable has a laufey-launch.json.
//
// A packaged app (`has_launch_file`, or one of `bundled` exists) loads the
// first existing `bundled` path and nothing else: `--runtime`,
// `env_runtime_path` and `development` are ignored, the first two with a
// warning. Otherwise (a development host) the order is `--runtime`, then
// `env_runtime_path`, then the first existing `development` path.
RuntimeChoice ChooseRuntimePath(
    const std::vector<std::string>& args, const std::string& env_runtime_path,
    const std::vector<std::string>& bundled,
    const std::vector<std::string>& development, bool has_launch_file,
    const std::function<bool(const std::string&)>& exists);

// ChooseRuntimePath for this process: LAUFEY_RUNTIME_PATH from the
// environment, this executable's launch file, the filesystem. Warnings are
// printed on stderr.
RuntimeChoice ResolveRuntimePath(const std::vector<std::string>& args,
                                 const std::vector<std::string>& bundled,
                                 const std::vector<std::string>& development);

// The WebView2 loader's own environment variables that a packaged app
// (IsPackagedLaunch) with DevTools off (LaunchInspectable() false) clears
// from its environment before it creates a WebView2 environment, because
// WebView2 reads them itself and each one reopens what that app closed:
// extra Chromium switches (WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS, e.g. a
// remote-debugging port), another browser binary to load
// (WEBVIEW2_BROWSER_EXECUTABLE_FOLDER), another profile
// (WEBVIEW2_USER_DATA_FOLDER) and a script debugger on a named pipe
// (WEBVIEW2_PIPE_FOR_SCRIPT_DEBUGGER, WEBVIEW2_WAIT_FOR_SCRIPT_DEBUGGER).
// Empty otherwise: a development launch, or an inspectable app, honours them.
// WEBVIEW2_RELEASE_CHANNEL_PREFERENCE / WEBVIEW2_CHANNEL_SEARCH_KIND are left
// alone: they only choose among the signed WebView2 runtimes installed.
std::vector<std::string> WebView2EnvironmentOverridesToClear(bool packaged,
                                                             bool inspectable);

// The process's own arguments (argv without the program, UTF-8), recorded by
// the host's main() before anything parses them (SetProcessArgs) and read by
// the CEF command line hook (ProcessArgs). Not thread-safe to set: main()
// sets them once, first.
void SetProcessArgs(std::vector<std::string> args);
const std::vector<std::string>& ProcessArgs();

}  // namespace laufey_common

#endif  // LAUFEY_LAUNCH_ARGS_H_
