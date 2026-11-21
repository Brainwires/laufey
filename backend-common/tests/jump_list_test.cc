// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the Windows jump list's portable half (src/jump_list.cc).
// No test framework: compile this file with src/jump_list.cc,
// src/single_instance.cc, the platform's single_instance transport,
// src/launch_config.cc and src/data_dir.cc, with backend-common/include and
// capi/include on the include path, and run it. CI does this in the `test`
// job and through ctest. Exits non-zero if any expectation fails.

#include "laufey_jump_list.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace laufey_common;

static int g_failures = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

static MenuEntry Item(const std::string& id, const std::string& label,
                      bool enabled = true) {
  MenuEntry e;
  e.kind = MenuEntry::Kind::kItem;
  e.id = id;
  e.label = label;
  e.enabled = enabled;
  return e;
}

static MenuEntry Separator() {
  MenuEntry e;
  e.kind = MenuEntry::Kind::kSeparator;
  return e;
}

static MenuEntry Role(const std::string& role) {
  MenuEntry e;
  e.kind = MenuEntry::Kind::kRole;
  e.role = role;
  return e;
}

static MenuEntry Submenu(const std::string& label,
                         std::vector<MenuEntry> children) {
  MenuEntry e;
  e.kind = MenuEntry::Kind::kSubmenu;
  e.label = label;
  e.children = std::move(children);
  return e;
}

// "id", "-" for a separator.
static std::vector<std::string> Shape(const std::vector<JumpListTask>& tasks) {
  std::vector<std::string> out;
  for (const JumpListTask& t : tasks)
    out.push_back(t.separator ? "-" : t.id);
  return out;
}

static void TestFlatten() {
  using S = std::vector<std::string>;
  // Items in order; a label-less item is titled with its id.
  auto tasks =
      JumpListTasksFromMenu({Item("new", "New Window"), Item("x", "")});
  EXPECT(Shape(tasks) == (S{"new", "x"}));
  EXPECT(tasks[0].label == "New Window");
  EXPECT(tasks[1].label == "x");

  // An id-less item reports its label (the Windows builders' fallback).
  tasks = JumpListTasksFromMenu({Item("", "Compose")});
  EXPECT(Shape(tasks) == (S{"Compose"}));

  // Disabled items and roles are left out; an item with neither id nor
  // label too.
  EXPECT(Shape(JumpListTasksFromMenu({Item("a", "A", false), Role("quit"),
                                      Item("", ""), Item("b", "B")})) ==
         (S{"b"}));

  // Separators never lead, trail or repeat.
  EXPECT(Shape(JumpListTasksFromMenu({Separator(), Item("a", "A"), Separator(),
                                      Separator(), Item("b", "B"),
                                      Separator()})) == (S{"a", "-", "b"}));
  EXPECT(JumpListTasksFromMenu({Separator(), Separator()}).empty());
  EXPECT(JumpListTasksFromMenu({}).empty());

  // A submenu's items take its place, between separators.
  EXPECT(Shape(JumpListTasksFromMenu(
             {Item("a", "A"),
              Submenu("More",
                      {Item("b", "B"), Submenu("Deeper", {Item("c", "C")})}),
              Item("d", "D")})) == (S{"a", "-", "b", "-", "c", "-", "d"}));
  EXPECT(Shape(JumpListTasksFromMenu({Submenu("Only", {Item("a", "A")})})) ==
         (S{"a"}));
  EXPECT(JumpListTasksFromMenu({Submenu("Empty", {})}).empty());

  // An id too long for a shell link's arguments is left out.
  EXPECT(JumpListTasksFromMenu(
             {Item(std::string(kMaxDockItemIdBytes + 1, 'a'), "Long")})
             .empty());
  EXPECT(JumpListTasksFromMenu(
             {Item(std::string(kMaxDockItemIdBytes, 'a'), "Long")})
             .size() == 1);
}

static void TestDockItemFromArgs() {
  std::string id;
  EXPECT(DockItemFromArgs({"--laufey-dock-item=new-window"}, &id));
  EXPECT(id == "new-window");
  EXPECT(DockItemFromArgs({"--laufey-dock-item=a b \"c\""}, &id));
  EXPECT(id == "a b \"c\"");
  EXPECT(DockItemFromArgs({"--laufey-dock-item=="}, &id));
  EXPECT(id == "=");
  // Not a task's launch: an empty id, another flag, more arguments, the flag
  // after "--" (a deep link closing its quotes), no arguments.
  EXPECT(!DockItemFromArgs({"--laufey-dock-item="}, &id));
  EXPECT(!DockItemFromArgs({"--laufey-dock-item"}, &id));
  EXPECT(!DockItemFromArgs({"--laufey-dock-items=a"}, &id));
  EXPECT(!DockItemFromArgs({"--laufey-dock-item=a", "b"}, &id));
  EXPECT(!DockItemFromArgs({"--", "--laufey-dock-item=a"}, &id));
  EXPECT(!DockItemFromArgs({"x", "--laufey-dock-item=a"}, &id));
  EXPECT(!DockItemFromArgs({}, &id));
  EXPECT(!DockItemFromArgs(
      {"--laufey-dock-item=" + std::string(kMaxDockItemIdBytes + 1, 'a')},
      &id));
  EXPECT(DockItemFromArgs(
      {"--laufey-dock-item=" + std::string(kMaxDockItemIdBytes, 'a')}, &id));
  EXPECT(DockItemFromArgs({"--laufey-dock-item=a"}, nullptr));
}

// CommandLineToArgvW's reading of one quoted argument (the subset its rules
// need for a string QuoteWindowsArgument produced): the inverse.
static std::string Unquote(const std::string& quoted, bool* one_arg) {
  std::string out;
  bool in_quotes = false;
  size_t backslashes = 0;
  *one_arg = true;
  for (char c : quoted) {
    if (c == '\\') {
      ++backslashes;
      continue;
    }
    if (c == '"') {
      out.append(backslashes / 2, '\\');
      if (backslashes % 2 == 1) {
        out.push_back('"');
      } else {
        in_quotes = !in_quotes;
      }
      backslashes = 0;
      continue;
    }
    out.append(backslashes, '\\');
    backslashes = 0;
    if ((c == ' ' || c == '\t') && !in_quotes)
      *one_arg = false;
    out.push_back(c);
  }
  out.append(backslashes, '\\');
  if (in_quotes)
    *one_arg = false;
  return out;
}

static void TestQuote() {
  EXPECT(QuoteWindowsArgument("") == "\"\"");
  EXPECT(QuoteWindowsArgument("a b") == "\"a b\"");
  EXPECT(QuoteWindowsArgument("a\"b") == "\"a\\\"b\"");
  EXPECT(QuoteWindowsArgument("a\\b") == "\"a\\b\"");
  EXPECT(QuoteWindowsArgument("a\\\"b") == "\"a\\\\\\\"b\"");
  EXPECT(QuoteWindowsArgument("a\\") == "\"a\\\\\"");
  for (const std::string& s :
       {std::string("--laufey-dock-item=x"), std::string("a \" b"),
        std::string("trailing\\\\"), std::string("\\\"\\\\\""),
        std::string("\" --runtime evil")}) {
    bool one = false;
    EXPECT(Unquote(QuoteWindowsArgument(s), &one) == s);
    EXPECT(one);
  }
}

struct Click {
  int count = 0;
  uint32_t window_id = 99;
  std::string id;
};

static void OnClick(void* data, uint32_t window_id, const char* id) {
  Click* c = static_cast<Click*>(data);
  ++c->count;
  c->window_id = window_id;
  c->id = id;
}

struct Forwarded {
  int count = 0;
  std::vector<std::string> args;
};

static void OnSecondInstance(void* data, const char* const* argv, size_t argc,
                             const char* /*cwd*/) {
  Forwarded* f = static_cast<Forwarded*>(data);
  ++f->count;
  f->args.assign(argv, argv + argc);
}

static SecondInstanceMessage Launch(std::vector<std::string> args) {
  SecondInstanceMessage m;
  m.args = std::move(args);
  m.cwd = "/";
  return m;
}

static void TestRouting() {
  Click click;
  Forwarded forwarded;
  SetSecondInstanceHandler(OnSecondInstance, &forwarded);

  // No dock menu: a task's launch is an ordinary second instance.
  EXPECT(!DeliverDockItemLaunch(Launch({"--laufey-dock-item=new"})));
  DeliverSecondInstance(Launch({"--laufey-dock-item=new"}));
  EXPECT(forwarded.count == 1);
  EXPECT(click.count == 0);

  auto tasks = JumpListTasksFromMenu(
      {Item("new", "New"), Separator(), Item("open", "Open")});
  SetDockItemHandler(tasks, OnClick, &click);

  // A task on the menu is its click, window id 0, and goes no further.
  DeliverSecondInstance(Launch({"--laufey-dock-item=open"}));
  EXPECT(click.count == 1);
  EXPECT(click.window_id == 0);
  EXPECT(click.id == "open");
  EXPECT(forwarded.count == 1);

  // An id no longer on the menu is dropped.
  DeliverSecondInstance(Launch({"--laufey-dock-item=gone"}));
  EXPECT(click.count == 1);
  EXPECT(forwarded.count == 1);

  // Any other launch reaches the second-instance handler.
  DeliverSecondInstance(Launch({"app://x"}));
  DeliverSecondInstance(Launch({"--laufey-dock-item=open", "extra"}));
  EXPECT(forwarded.count == 3);
  EXPECT(forwarded.args ==
         (std::vector<std::string>{"--laufey-dock-item=open", "extra"}));
  EXPECT(click.count == 1);

  // A new menu replaces the ids.
  SetDockItemHandler(JumpListTasksFromMenu({Item("new", "New")}), OnClick,
                     &click);
  DeliverSecondInstance(Launch({"--laufey-dock-item=open"}));
  EXPECT(click.count == 1);
  DeliverSecondInstance(Launch({"--laufey-dock-item=new"}));
  EXPECT(click.count == 2);
  EXPECT(click.id == "new");

  // Clearing the menu hands the launches back to the handler.
  SetDockItemHandler({}, nullptr, nullptr);
  DeliverSecondInstance(Launch({"--laufey-dock-item=new"}));
  EXPECT(click.count == 2);
  EXPECT(forwarded.count == 4);

  SetSecondInstanceHandler(nullptr, nullptr);
}

int main() {
  TestFlatten();
  TestDockItemFromArgs();
  TestQuote();
  TestRouting();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("jump_list_test: all passed\n");
  return 0;
}
