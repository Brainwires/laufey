// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The portable half of the Windows jump list (laufey_jump_list.h): the
// menu's flattening, the task's argument, and routing its launch to the dock
// menu's click. The jump list itself is jump_list_win.cc.

#include "laufey_jump_list.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace laufey_common {

namespace {

void AppendSeparator(std::vector<JumpListTask>* out) {
  if (out->empty() || out->back().separator)
    return;
  JumpListTask sep;
  sep.separator = true;
  out->push_back(std::move(sep));
}

void AppendEntries(const std::vector<MenuEntry>& entries,
                   std::vector<JumpListTask>* out) {
  for (const MenuEntry& entry : entries) {
    switch (entry.kind) {
      case MenuEntry::Kind::kSeparator:
        AppendSeparator(out);
        break;
      case MenuEntry::Kind::kSubmenu:
        AppendSeparator(out);
        AppendEntries(entry.children, out);
        AppendSeparator(out);
        break;
      case MenuEntry::Kind::kRole:
        // A role acts on the running app (quit, edit commands): a task
        // starts the executable, so none has a meaning there.
        break;
      case MenuEntry::Kind::kItem: {
        const std::string& key = entry.ClickKey();
        if (!entry.enabled || key.empty() || key.size() > kMaxDockItemIdBytes)
          break;
        JumpListTask task;
        task.id = key;
        task.label = entry.label.empty() ? key : entry.label;
        out->push_back(std::move(task));
        break;
      }
    }
  }
}

struct DockItemState {
  std::mutex mutex;
  std::vector<std::string> ids;
  laufey_menu_click_fn on_click = nullptr;
  void* on_click_data = nullptr;
};

DockItemState& DockItems() {
  static DockItemState* state = new DockItemState;
  return *state;
}

}  // namespace

std::vector<JumpListTask> JumpListTasksFromMenu(
    const std::vector<MenuEntry>& entries) {
  std::vector<JumpListTask> out;
  AppendEntries(entries, &out);
  if (!out.empty() && out.back().separator)
    out.pop_back();
  return out;
}

bool DockItemFromArgs(const std::vector<std::string>& args, std::string* id) {
  if (args.size() != 1)
    return false;
  const std::string& arg = args[0];
  const size_t prefix = sizeof(kDockItemArgPrefix) - 1;
  if (arg.size() <= prefix || arg.compare(0, prefix, kDockItemArgPrefix) != 0)
    return false;
  if (arg.size() - prefix > kMaxDockItemIdBytes)
    return false;
  if (id)
    *id = arg.substr(prefix);
  return true;
}

std::string QuoteWindowsArgument(const std::string& arg) {
  // The rules CommandLineToArgvW reads: backslashes are literal unless they
  // precede a '"', where 2n of them are n backslashes and 2n+1 are n plus a
  // literal quote.
  std::string out = "\"";
  size_t backslashes = 0;
  for (char c : arg) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      out.append(backslashes * 2 + 1, '\\');
    } else {
      out.append(backslashes, '\\');
    }
    backslashes = 0;
    out.push_back(c);
  }
  // Before the closing quote, every backslash is doubled.
  out.append(backslashes * 2, '\\');
  out.push_back('"');
  return out;
}

void SetDockItemHandler(const std::vector<JumpListTask>& tasks,
                        laufey_menu_click_fn on_click, void* on_click_data) {
  DockItemState& state = DockItems();
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    state.ids.clear();
    if (on_click) {
      for (const JumpListTask& task : tasks) {
        if (!task.separator)
          state.ids.push_back(task.id);
      }
    }
    state.on_click = on_click;
    state.on_click_data = on_click ? on_click_data : nullptr;
  }
  SetSecondInstanceInterceptor(on_click ? DeliverDockItemLaunch : nullptr);
}

bool DeliverDockItemLaunch(const SecondInstanceMessage& message) {
  std::string id;
  if (!DockItemFromArgs(message.args, &id))
    return false;
  DockItemState& state = DockItems();
  laufey_menu_click_fn on_click = nullptr;
  void* data = nullptr;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.on_click)
      return false;
    if (std::find(state.ids.begin(), state.ids.end(), id) == state.ids.end())
      return true;  // a task of an earlier menu: dropped
    on_click = state.on_click;
    data = state.on_click_data;
  }
  // Outside the lock: embedder code, which may set the menu again.
  on_click(data, 0, id.c_str());
  return true;
}

}  // namespace laufey_common
