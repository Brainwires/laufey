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

// The Chromium switches the Windows CEF host always puts on its browser
// process's command line. Its command line hook (OnBeforeCommandLineProcessing)
// appends each one the command line lacks, after it dropped a deep-link
// launch's switches (DeepLinkSwitchesToStrip), so a link can't take them away
// and a launch that already carries one keeps it.
//
//  * "do-not-de-elevate" (Chromium's kDoNotDeElevateOnLaunch). A Chrome
//    browser process that starts elevated (the full token of an administrator
//    with UAC on: "Run as administrator", an elevated shell, a task with the
//    highest privileges) relaunches itself through the desktop shell's token
//    and ends at once (ChromeBrowserMainPartsWin::MaybeAutoDeElevate, feature
//    AutoDeElevate, on by default). For a laufey app that relaunch is a
//    different process with the shell's default environment, Chromium's own
//    switches in its argv and nobody waiting for it: the launch the user
//    started exits 1 (CefInitialize fails with the de-elevation result) at
//    once, the variables its launcher set are gone, and a deep link's
//    relaunch drops CEF's own --disable-features defaults with every other
//    switch (DeepLinkSwitchesToStrip) and crashes in Chrome's tab code. With
//    the switch the app runs at the level it was started at, as every other
//    backend's does (the executable's manifest asks for asInvoker).
//    Chromium 149 and 150 read it in PreEarlyInitialization, after the hook.
//    From Chromium 156 (main@{#1700144}) Chromium checks in
//    ChromeMainDelegate::BasicStartupComplete, which CEF calls before the
//    hook, so the switch must already be on the process's own command line:
//    an elevated launch without it starts itself again with it first
//    (NeedsDeElevationRelaunch). The hook still appends it, so the command
//    line the browser process runs with always carries it.
const std::vector<std::string>& CefWindowsBrowserSwitches();

// --- Elevated launches on Windows (CEF, Chromium 156+) ----------------------
//
// Chromium 156 checks for auto de-elevation before the host's command line
// hook (CefWindowsBrowserSwitches), so a Windows CEF browser process that
// starts elevated without --do-not-de-elevate on its command line starts
// itself again first: the same executable, the same command line with
// kDoNotDeElevateArgument after the program name, the same environment
// (kDeElevationRelaunchEnv added), working directory and standard handles,
// still elevated. The first process waits for it and exits with its exit
// code. The relaunched process drops the argument it was given from the
// arguments the app sees (DropDeElevationRelaunchArgument), so an elevated
// launch shows the app the same arguments as any other.

// The argument the relaunch adds after the program name.
inline constexpr char kDoNotDeElevateArgument[] = "--do-not-de-elevate";

// Set (to "1") in the relaunched process's environment; the host removes it
// at once (it reaches no process the app starts).
inline constexpr char kDeElevationRelaunchEnv[] =
    "LAUFEY_DE_ELEVATION_RELAUNCH";

// Whether a browser process started with `args` (argv without the program)
// must start itself again with kDoNotDeElevateArgument: its token is
// elevated with UAC on (`full_token`: TokenElevationTypeFull, Chromium's
// base::win::UserAccountIsUnnecessarilyElevated) and Chromium would
// de-elevate it: no switch before "--" that skips that (do-not-de-elevate,
// enable-automation, isolated, test-type; Chromium's MaybeAutoDeElevate).
bool NeedsDeElevationRelaunch(const std::vector<std::string>& args,
                              bool full_token);

// `command_line` (a process's raw command line, GetCommandLineW) with " " and
// `argument` inserted right after the program name, where
// CommandLineToArgvW ends argv[0]: at the closing '"' of a quoted name, else
// at the first space or tab. The rest of the command line is kept as it is,
// so every other argument reaches the new process exactly as it was given.
// An empty (or all-blank) `command_line` gives `program` in quotes followed
// by the argument.
std::wstring InsertArgumentAfterProgram(const std::wstring& command_line,
                                        const std::wstring& program,
                                        const std::wstring& argument);

// Marks this process as the relaunch (it found kDeElevationRelaunchEnv).
// Process-wide; set once, before any argument is read.
void SetDeElevationRelaunched(bool relaunched);
bool DeElevationRelaunched();

// In the relaunched process (DeElevationRelaunched), removes the leading
// kDoNotDeElevateArgument the relaunch added from `args` (argv without the
// program). Anywhere else, and when `args` doesn't start with it, `args` is
// left as it is.
void DropDeElevationRelaunchArgument(std::vector<std::string>* args);

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

// Whether the host must not start: a packaged app that ships no runtime
// library has nothing to run (ChooseRuntimePath already warned). Every host
// then prints why and exits with kMissingRuntimeExitCode at once, before any
// window, dialog or web engine: a packaged app never falls back to a
// runtime from elsewhere, and a host without one must not sit there.
inline bool IsMissingPackagedRuntime(const RuntimeChoice& choice) {
  return choice.packaged && choice.path.empty();
}
inline constexpr int kMissingRuntimeExitCode = 3;
// Prints the one-line reason on stderr (any thread, no UI).
void ReportMissingPackagedRuntime();

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

// Whether a launch is a headless worker rather than the app: the runtime runs
// a script with no web engine and no window. Two shapes:
//
//  * `<exe> run [flags] <script> ...`: the embedder's CLI worker command
//    (Deno Desktop's update helper is `<exe> run denext-update-helper ...`).
//    `args` is argv without the program; IsCliWorkerCommand holds when
//    args[0] is "run" and a non-flag argument follows it.
//  * a forked Node-style worker: NODE_CHANNEL_FD or NEXT_PRIVATE_WORKER is
//    set (IsForkedWorkerEnvironment; `is_set` reports whether a variable is
//    present in the environment).
//
// Every host checks IsHeadlessWorkerLaunch before SingleInstanceStartup: a
// worker is never forwarded to a running instance as a second launch (it
// would never run) and never takes the single-instance lock itself.
bool IsCliWorkerCommand(const std::vector<std::string>& args);
bool IsForkedWorkerEnvironment(const std::function<bool(const char*)>& is_set);
// Both, for this process's environment.
bool IsHeadlessWorkerLaunch(const std::vector<std::string>& args);

// The process's own arguments (argv without the program, UTF-8), recorded by
// the host's main() before anything parses them (SetProcessArgs) and read by
// the CEF command line hook (ProcessArgs). Not thread-safe to set: main()
// sets them once, first.
void SetProcessArgs(std::vector<std::string> args);
const std::vector<std::string>& ProcessArgs();

}  // namespace laufey_common

#endif  // LAUFEY_LAUNCH_ARGS_H_
