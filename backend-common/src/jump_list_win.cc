// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The dock menu on Windows (laufey_jump_list.h): the jump list's "Tasks"
// category through ICustomDestinationList. Each task is an IShellLinkW for
// this executable whose arguments carry the item's id; a separator is a link
// with System.AppUserModel.IsDestListSeparator set.
//
// The list is written on a short-lived worker thread (its own STA), so
// set_dock_menu returns at once and no UI thread waits on the shell. Writes
// are serialised and a stale one (a newer set_dock_menu came in) is skipped,
// so the jump list always ends as the last menu set.
//
// No AppUserModelID is set here: the list goes to the process's own, the one
// its taskbar button has (the explicit one when something set it, else the
// one Windows derives from the executable). Setting one would split the
// running app from a pinned shortcut that doesn't carry the same id.

#include "laufey_jump_list.h"

#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "laufey_backend_common.h"

using Microsoft::WRL::ComPtr;

namespace laufey_common {

namespace {

// System.Title (FMTID_SummaryInformation, PID_TITLE): the task's text.
const PROPERTYKEY kPkeyTitle = {
    {0xF29F85E0,
     0x4FF9,
     0x1068,
     {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9}},
    2};
// System.AppUserModel.IsDestListSeparator: the entry is a separator.
const PROPERTYKEY kPkeyIsDestListSeparator = {
    {0x9F4C2855,
     0x9F79,
     0x4B39,
     {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}},
    6};

// The longest title written (characters); a longer label is cut.
constexpr size_t kMaxTitleChars = 259;

std::mutex g_write_mutex;
std::atomic<uint64_t> g_generation{0};

std::wstring ExecutablePath() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    DWORD n = GetModuleFileNameW(nullptr, path.data(),
                                 static_cast<DWORD>(path.size()));
    if (n == 0)
      return std::wstring();
    if (n < path.size()) {
      path.resize(n);
      return path;
    }
    if (path.size() >= 32768)
      return std::wstring();
    path.resize(path.size() * 2);
  }
}

bool SetStringProperty(IPropertyStore* store, const PROPERTYKEY& key,
                       const std::wstring& value) {
  PROPVARIANT pv;
  PropVariantInit(&pv);
  size_t bytes = (value.size() + 1) * sizeof(wchar_t);
  auto* text = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
  if (!text)
    return false;
  memcpy(text, value.c_str(), bytes);
  pv.vt = VT_LPWSTR;
  pv.pwszVal = text;
  HRESULT hr = store->SetValue(key, pv);
  PropVariantClear(&pv);
  return SUCCEEDED(hr);
}

bool SetBoolProperty(IPropertyStore* store, const PROPERTYKEY& key,
                     bool value) {
  PROPVARIANT pv;
  PropVariantInit(&pv);
  pv.vt = VT_BOOL;
  pv.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  return SUCCEEDED(store->SetValue(key, pv));
}

ComPtr<IShellLinkW> MakeTaskLink(const std::wstring& exe,
                                 const JumpListTask& task) {
  ComPtr<IShellLinkW> link;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&link))))
    return nullptr;
  ComPtr<IPropertyStore> store;
  if (FAILED(link.As(&store)))
    return nullptr;
  if (task.separator) {
    if (!SetBoolProperty(store.Get(), kPkeyIsDestListSeparator, true))
      return nullptr;
  } else {
    std::wstring args = Utf8ToWide(
        QuoteWindowsArgument(std::string(kDockItemArgPrefix) + task.id));
    std::wstring title = Utf8ToWide(task.label);
    if (title.size() > kMaxTitleChars)
      title.resize(kMaxTitleChars);
    if (FAILED(link->SetPath(exe.c_str())) ||
        FAILED(link->SetArguments(args.c_str())) ||
        FAILED(link->SetIconLocation(exe.c_str(), 0)) ||
        !SetStringProperty(store.Get(), kPkeyTitle, title))
      return nullptr;
  }
  if (FAILED(store->Commit()))
    return nullptr;
  return link;
}

// The process's explicit AppUserModelID, or "" (Windows derives one).
std::wstring ExplicitAppId() {
  PWSTR id = nullptr;
  std::wstring out;
  if (SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&id)) && id)
    out = id;
  if (id)
    CoTaskMemFree(id);
  return out;
}

// Writes `tasks` as the Tasks category (an empty list deletes the jump
// list). Runs with COM initialized on the calling thread.
void WriteJumpList(const std::vector<JumpListTask>& tasks) {
  ComPtr<ICustomDestinationList> list;
  HRESULT hr = CoCreateInstance(CLSID_DestinationList, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list));
  if (FAILED(hr)) {
    std::cerr << "laufey: jump list unavailable (0x" << std::hex << hr
              << std::dec << ")" << std::endl;
    return;
  }
  std::wstring app_id = ExplicitAppId();
  if (!app_id.empty())
    list->SetAppID(app_id.c_str());
  if (tasks.empty()) {
    list->DeleteList(app_id.empty() ? nullptr : app_id.c_str());
    return;
  }
  std::wstring exe = ExecutablePath();
  if (exe.empty())
    return;
  UINT min_slots = 0;
  ComPtr<IObjectArray> removed;
  if (FAILED(list->BeginList(&min_slots, IID_PPV_ARGS(&removed))))
    return;
  ComPtr<IObjectCollection> collection;
  if (FAILED(CoCreateInstance(CLSID_EnumerableObjectCollection, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&collection)))) {
    list->AbortList();
    return;
  }
  UINT added = 0;
  for (const JumpListTask& task : tasks) {
    ComPtr<IShellLinkW> link = MakeTaskLink(exe, task);
    if (!link) {
      if (!task.separator)
        std::cerr << "laufey: jump list: left out the dock menu item \""
                  << task.id << "\"" << std::endl;
      continue;
    }
    if (SUCCEEDED(collection->AddObject(link.Get())))
      ++added;
  }
  ComPtr<IObjectArray> array;
  if (added == 0 || FAILED(collection.As(&array)) ||
      FAILED(list->AddUserTasks(array.Get()))) {
    list->AbortList();
    list->DeleteList(app_id.empty() ? nullptr : app_id.c_str());
    return;
  }
  hr = list->CommitList();
  if (FAILED(hr)) {
    std::cerr << "laufey: jump list commit failed (0x" << std::hex << hr
              << std::dec << ")" << std::endl;
  }
}

}  // namespace

void SetDockMenuWin(laufey_value_t* menu_template,
                    const laufey_backend_api_t* api,
                    laufey_menu_click_fn on_click, void* on_click_data) {
  std::vector<JumpListTask> tasks;
  if (menu_template && api)
    tasks = JumpListTasksFromMenu(ParseMenuTemplate(menu_template, api, false));
  // Clicks route to the new menu right away; the list follows.
  SetDockItemHandler(tasks, menu_template ? on_click : nullptr, on_click_data);
  uint64_t generation = ++g_generation;
  std::thread([tasks = std::move(tasks), generation] {
    std::lock_guard<std::mutex> lock(g_write_mutex);
    if (generation != g_generation.load())
      return;  // a newer menu is on its way
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WriteJumpList(tasks);
    if (SUCCEEDED(init))
      CoUninitialize();
  }).detach();
}

}  // namespace laufey_common
