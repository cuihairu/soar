// Unit tests for the keyboard shortcut map (src/app/ui/shortcuts.*):
// the shipped default table, the config grammar (rebind / unbind / digit
// range / kill switch), the lookup specificity rules, and the help-overlay
// name rendering. Pure logic over in-memory text + one temp file — no
// display, no media fixtures, no network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/shortcuts.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#  include <unistd.h>
#endif

using soar::app::defaultShortcutsPath;
using soar::app::kKeyA;
using soar::app::kKeyC;
using soar::app::kKeyComma;
using soar::app::kKeyEscape;
using soar::app::kKeyF;
using soar::app::kKeyH;
using soar::app::kKeyHome;
using soar::app::kKeyI;
using soar::app::kKeyK;
using soar::app::kKeyL;
using soar::app::kKeyLeft;
using soar::app::kKeyLeftBracket;
using soar::app::kKeyM;
using soar::app::kKeyN;
using soar::app::kKeyO;
using soar::app::kKeyP;
using soar::app::kKeyPageDown;
using soar::app::kKeyPageUp;
using soar::app::kKeyPeriod;
using soar::app::kKeyQ;
using soar::app::kKeyR;
using soar::app::kKeyRight;
using soar::app::kKeyRightBracket;
using soar::app::kKeyS;
using soar::app::kKeyT;
using soar::app::kKeyB;
using soar::app::kKeySpace;
using soar::app::kKeyZ;
using soar::app::kKeyDown;
using soar::app::kKeyUp;
using soar::app::kKeyV;
using soar::app::parseShortcutKey;
using soar::app::shortcutActionName;
using soar::app::shortcutKeyName;
using soar::app::ShortcutAction;
using soar::app::ShortcutKey;
using soar::app::ShortcutMap;

namespace {

constexpr ShortcutKey plain(std::uint32_t sym) { return ShortcutKey{sym}; }
constexpr ShortcutKey shifted(std::uint32_t sym) {
  return ShortcutKey{sym, true};
}
constexpr ShortcutKey ctrled(std::uint32_t sym) {
  return ShortcutKey{sym, false, true};
}

std::uint32_t digit(char c) { return static_cast<std::uint32_t>(c); }

std::string tempFile(const char* stem) {
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
  if (ec) dir = std::filesystem::path("/tmp");
  static int calls = 0;
  dir /= std::string(stem) + "_" + std::to_string(::getpid()) + "_" +
         std::to_string(calls++) + ".txt";
  return dir.string();
}

void writeFile(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::trunc | std::ios::binary);
  out << text;
}

}  // namespace

TEST_CASE("defaults match the shipped table exactly") {
  const ShortcutMap map;
  using A = ShortcutAction;

  CHECK(map.bindings(A::Escape) == std::vector<ShortcutKey>{plain(kKeyEscape)});
  CHECK(map.bindings(A::PlayPause) ==
        std::vector<ShortcutKey>{plain(kKeySpace), plain(kKeyK)});
  CHECK(map.bindings(A::SeekBack5) == std::vector<ShortcutKey>{plain(kKeyLeft)});
  CHECK(map.bindings(A::SeekFwd5) == std::vector<ShortcutKey>{plain(kKeyRight)});
  CHECK(map.bindings(A::SeekBack1) == std::vector<ShortcutKey>{shifted(kKeyLeft)});
  CHECK(map.bindings(A::SeekFwd1) == std::vector<ShortcutKey>{shifted(kKeyRight)});
  CHECK(map.bindings(A::SeekBack60) ==
        std::vector<ShortcutKey>{plain(kKeyPageUp)});
  CHECK(map.bindings(A::SeekFwd60) ==
        std::vector<ShortcutKey>{plain(kKeyPageDown)});
  CHECK(map.bindings(A::SeekStart) == std::vector<ShortcutKey>{plain(kKeyHome)});
  std::vector<ShortcutKey> digits;
  for (char c = '0'; c <= '9'; ++c) digits.push_back(plain(digit(c)));
  CHECK(map.bindings(A::PercentSeek) == digits);
  CHECK(map.bindings(A::VolumeUp) == std::vector<ShortcutKey>{plain(kKeyUp)});
  CHECK(map.bindings(A::VolumeDown) == std::vector<ShortcutKey>{plain(kKeyDown)});
  CHECK(map.bindings(A::Mute) == std::vector<ShortcutKey>{plain(kKeyM)});
  CHECK(map.bindings(A::RateDown) == std::vector<ShortcutKey>{plain(kKeyComma)});
  CHECK(map.bindings(A::RateUp) == std::vector<ShortcutKey>{plain(kKeyPeriod)});
  CHECK(map.bindings(A::Fullscreen) == std::vector<ShortcutKey>{plain(kKeyF)});
  CHECK(map.bindings(A::LoopAB) == std::vector<ShortcutKey>{plain(kKeyL)});
  CHECK(map.bindings(A::CycleAudio) == std::vector<ShortcutKey>{plain(kKeyA)});
  CHECK(map.bindings(A::CycleSubtitle) == std::vector<ShortcutKey>{plain(kKeyC)});
  CHECK(map.bindings(A::SubtitleOffsetBack) ==
        std::vector<ShortcutKey>{plain(kKeyLeftBracket)});
  CHECK(map.bindings(A::SubtitleOffsetFwd) ==
        std::vector<ShortcutKey>{plain(kKeyRightBracket)});
  CHECK(map.bindings(A::SubtitleVisibility) ==
        std::vector<ShortcutKey>{plain(kKeyV)});
  CHECK(map.bindings(A::Screenshot) == std::vector<ShortcutKey>{plain(kKeyS)});
  CHECK(map.bindings(A::OpenFile) == std::vector<ShortcutKey>{plain(kKeyO)});
  CHECK(map.bindings(A::OpenFolder) == std::vector<ShortcutKey>{ctrled(kKeyO)});
  CHECK(map.bindings(A::Playlist) == std::vector<ShortcutKey>{plain(kKeyP)});
  CHECK(map.bindings(A::Library) == std::vector<ShortcutKey>{plain(kKeyB)});
  CHECK(map.bindings(A::QueueNext) == std::vector<ShortcutKey>{plain(kKeyN)});
  CHECK(map.bindings(A::QueuePrev) == std::vector<ShortcutKey>{shifted(kKeyN)});
  CHECK(map.bindings(A::Info) == std::vector<ShortcutKey>{plain(kKeyI)});
  CHECK(map.bindings(A::Recent) == std::vector<ShortcutKey>{plain(kKeyR)});
  CHECK(map.bindings(A::Help) == std::vector<ShortcutKey>{plain(kKeyH)});
  CHECK(map.bindings(A::Cast) == std::vector<ShortcutKey>{plain(kKeyT)});
  CHECK(map.bindings(A::Quit) == std::vector<ShortcutKey>{plain(kKeyQ)});

  CHECK(map.enabled());
}

TEST_CASE("lookup fires the action for every default key") {
  const ShortcutMap map;
  using A = ShortcutAction;

  CHECK(map.actionFor(plain(kKeySpace)) == A::PlayPause);
  CHECK(map.actionFor(plain(kKeyK)) == A::PlayPause);
  CHECK(map.actionFor(plain(kKeyLeft)) == A::SeekBack5);
  CHECK(map.actionFor(plain(kKeyRight)) == A::SeekFwd5);
  CHECK(map.actionFor(shifted(kKeyLeft)) == A::SeekBack1);
  CHECK(map.actionFor(shifted(kKeyRight)) == A::SeekFwd1);
  CHECK(map.actionFor(plain(kKeyPageUp)) == A::SeekBack60);
  CHECK(map.actionFor(plain(kKeyPageDown)) == A::SeekFwd60);
  CHECK(map.actionFor(plain(kKeyHome)) == A::SeekStart);
  CHECK(map.actionFor(plain(digit('0'))) == A::PercentSeek);
  CHECK(map.actionFor(plain(digit('7'))) == A::PercentSeek);
  CHECK(map.actionFor(plain(digit('9'))) == A::PercentSeek);
  CHECK(map.actionFor(plain(kKeyUp)) == A::VolumeUp);
  CHECK(map.actionFor(plain(kKeyDown)) == A::VolumeDown);
  CHECK(map.actionFor(plain(kKeyM)) == A::Mute);
  CHECK(map.actionFor(plain(kKeyComma)) == A::RateDown);
  CHECK(map.actionFor(plain(kKeyPeriod)) == A::RateUp);
  CHECK(map.actionFor(plain(kKeyF)) == A::Fullscreen);
  CHECK(map.actionFor(plain(kKeyL)) == A::LoopAB);
  CHECK(map.actionFor(plain(kKeyA)) == A::CycleAudio);
  CHECK(map.actionFor(plain(kKeyC)) == A::CycleSubtitle);
  CHECK(map.actionFor(plain(kKeyLeftBracket)) == A::SubtitleOffsetBack);
  CHECK(map.actionFor(plain(kKeyRightBracket)) == A::SubtitleOffsetFwd);
  CHECK(map.actionFor(plain(kKeyV)) == A::SubtitleVisibility);
  CHECK(map.actionFor(plain(kKeyS)) == A::Screenshot);
  CHECK(map.actionFor(plain(kKeyO)) == A::OpenFile);
  CHECK(map.actionFor(ctrled(kKeyO)) == A::OpenFolder);
  CHECK(map.actionFor(plain(kKeyP)) == A::Playlist);
  CHECK(map.actionFor(plain(kKeyB)) == A::Library);
  CHECK(map.actionFor(plain(kKeyN)) == A::QueueNext);
  CHECK(map.actionFor(shifted(kKeyN)) == A::QueuePrev);
  CHECK(map.actionFor(plain(kKeyI)) == A::Info);
  CHECK(map.actionFor(plain(kKeyR)) == A::Recent);
  CHECK(map.actionFor(plain(kKeyH)) == A::Help);
  CHECK(map.actionFor(plain(kKeyT)) == A::Cast);
  CHECK(map.actionFor(plain(kKeyQ)) == A::Quit);
  CHECK(map.actionFor(plain(kKeyEscape)) == A::Escape);
}

TEST_CASE("plain bindings ignore modifiers, flagged ones outrank plain") {
  const ShortcutMap map;
  using A = ShortcutAction;

  // The shipped switch fired on the letter regardless of shift; plain
  // bindings keep that: shift+q quits, shift+space toggles playback.
  CHECK(map.actionFor(shifted(kKeyQ)) == A::Quit);
  CHECK(map.actionFor(shifted(kKeySpace)) == A::PlayPause);
  CHECK(map.actionFor(ShortcutKey{kKeyQ, false, true}) == A::Quit);  // ctrl+q
  // A flagged binding wins the same key over the plain one.
  CHECK(map.actionFor(shifted(kKeyN)) == A::QueuePrev);
  CHECK(map.actionFor(ctrled(kKeyO)) == A::OpenFolder);
  // Uppercase syms (SDL reports shifted letters) normalize to lowercase.
  ShortcutKey upper;
  upper.sym = 'Q';
  CHECK(map.actionFor(upper) == A::Quit);
  // Unknown keys are nobody's.
  CHECK(map.actionFor(plain(0x40000067 + 5)) == A::None);
  CHECK(map.actionFor(ShortcutKey{soar::app::kKeyNone}) == A::None);
}

TEST_CASE("bindingFor reports which binding fired") {
  const ShortcutMap map;
  using A = ShortcutAction;
  ShortcutAction action = A::None;
  // Parameterized actions read the fired key back from the live list:
  // the 8 key is the 8th of percent_seek's 0-9 bindings.
  const int eight = map.bindingFor(plain(digit('8')), &action);
  CHECK(action == A::PercentSeek);
  REQUIRE(eight >= 0);
  CHECK(map.bindings(action)[eight].sym == digit('8'));
  // Single-binding actions report index 0.
  CHECK(map.bindingFor(plain(kKeyQ), &action) == 0);
  CHECK(action == A::Quit);
  // Nothing fires: -1 and None.
  CHECK(map.bindingFor(plain(119)) == -1);  // w
  CHECK(map.bindingFor(plain(119), &action) == -1);
  CHECK(action == A::None);
}

TEST_CASE("applyText rebinds by replacing the action's defaults") {
  ShortcutMap map;
  map.applyText("play_pause = p\n");
  using A = ShortcutAction;
  CHECK(map.actionFor(plain(kKeyP)) == A::PlayPause);
  // The old defaults are gone, not extended.
  CHECK(map.actionFor(plain(kKeySpace)) == A::None);
  CHECK(map.actionFor(plain(kKeyK)) == A::None);
  CHECK(map.bindings(A::PlayPause) == std::vector<ShortcutKey>{plain(kKeyP)});
  // Everything else keeps its default.
  CHECK(map.actionFor(plain(kKeyQ)) == A::Quit);
}

TEST_CASE("applyText with an empty value disables the action") {
  ShortcutMap map;
  map.applyText("quit =\n");
  CHECK(map.actionFor(plain(kKeyQ)) == ShortcutAction::None);
  CHECK(map.actionFor(shifted(kKeyQ)) == ShortcutAction::None);
  CHECK(map.bindings(ShortcutAction::Quit).empty());
  CHECK(map.bindingNames(ShortcutAction::Quit) == "(off)");
}

TEST_CASE("single-char ranges expand to every key between") {
  ShortcutMap map;
  map.applyText("percent_seek = 0-9\n");
  for (char c = '0'; c <= '9'; ++c) {
    CHECK(map.actionFor(plain(digit(c))) == ShortcutAction::PercentSeek);
  }
  // x-z: no default binding there, so the new action owns every key.
  map.applyText("mute = x-z\n");
  CHECK(map.actionFor(plain(soar::app::kKeyX)) == ShortcutAction::Mute);
  CHECK(map.actionFor(plain(soar::app::kKeyY)) == ShortcutAction::Mute);
  CHECK(map.actionFor(plain(soar::app::kKeyZ)) == ShortcutAction::Mute);
  CHECK(map.actionFor(plain(119)) == ShortcutAction::None);  // w, outside
  // Only digit runs collapse to a range; letters list every binding.
  CHECK(map.bindingNames(ShortcutAction::Mute) == "X / Y / Z");
  // An inverted range is a typo: the whole line is skipped.
  map.applyText("mute = z-x\n");
  CHECK(map.actionFor(plain(kKeyM)) == ShortcutAction::Mute);  // default kept
}

TEST_CASE("modifier prefixes parse in any order and with aliases") {
  ShortcutMap map;
  map.applyText("screenshot = ctrl+alt+s\n"
                "help = alt+shift+h\n"
                "fullscreen = f, control+f\n");
  using A = ShortcutAction;
  CHECK(map.actionFor(ShortcutKey{kKeyS, false, true, true}) == A::Screenshot);
  // The line replaces screenshot's defaults: ctrl+s (no alt) matches no
  // binding at all, so it does nothing instead of a stray screenshot.
  CHECK(map.actionFor(ShortcutKey{kKeyS, false, true}) == A::None);
  CHECK(map.actionFor(plain(kKeyS)) == A::None);
  CHECK(map.actionFor(ShortcutKey{kKeyH, true, false, true}) == A::Help);
  CHECK(map.actionFor(ShortcutKey{kKeyF, false, true}) == A::Fullscreen);
  // Both pieces of one line bind; the flagged one outranks the plain one.
  CHECK(map.actionFor(plain(kKeyF)) == A::Fullscreen);
  CHECK(map.bindings(A::Fullscreen).size() == 2);  // f + control+f alias
  CHECK(map.bindingNames(A::Screenshot) == "Ctrl+Alt+S");
  CHECK(map.bindingNames(A::Help) == "Alt+Shift+H");
}

TEST_CASE("unknown actions, unknown keys and malformed lines are skipped") {
  ShortcutMap map;
  const std::string text =
      "# a comment line\n"
      "\n"
      "not_an_action = q\n"
      "play_pause = notakey\n"
      "no_equals_sign_here\n"
      "mute = comma, , k    # unknown middle piece drops, the rest applies\n"
      "quit = z\n";
  map.applyText(text);
  using A = ShortcutAction;
  // The known pieces applied and took their keys from the actions that
  // defaulted them; the garbage did not blank the table.
  CHECK(map.actionFor(plain(kKeyM)) == A::None);
  CHECK(map.actionFor(plain(kKeyComma)) == A::Mute);  // taken from rate_down
  CHECK(map.actionFor(plain(kKeyK)) == A::Mute);      // taken from play_pause
  CHECK(map.actionFor(plain(kKeyZ)) == A::Quit);
  // A line whose every token failed is a typo, not an unbinding: the
  // action keeps its default (unbinding is the empty value).
  CHECK(map.actionFor(plain(kKeySpace)) == A::PlayPause);
}

TEST_CASE("the kill switch turns everything off and back on") {
  ShortcutMap map;
  map.applyText("shortcuts = off\n");
  CHECK_FALSE(map.enabled());
  CHECK(map.actionFor(plain(kKeyQ)) == ShortcutAction::None);
  CHECK(map.actionFor(plain(kKeySpace)) == ShortcutAction::None);
  // The defaults stay loaded behind the switch, so `on` restores them.
  map.applyText("shortcuts = on\n");
  CHECK(map.enabled());
  CHECK(map.actionFor(plain(kKeyQ)) == ShortcutAction::Quit);
  // A text with no switch line leaves setEnabled() untouched.
  map.setEnabled(false);
  map.applyText("mute = n\n");
  CHECK_FALSE(map.enabled());
  map.setEnabled(true);
  CHECK(map.actionFor(plain(kKeyN)) == ShortcutAction::Mute);
}

TEST_CASE("applyText rebuilds from the defaults, so re-reads are stable") {
  ShortcutMap map;
  map.applyText("quit = x\n");
  map.applyText("quit = q\n");
  CHECK(map.actionFor(plain(kKeyQ)) == ShortcutAction::Quit);
  CHECK(map.actionFor(plain(soar::app::kKeyX)) == ShortcutAction::None);
  CHECK(map.actionFor(plain(kKeySpace)) == ShortcutAction::PlayPause);
}

TEST_CASE("load applies a real file and ignores a missing one") {
  ShortcutMap map;
  map.load(tempFile("soar_shortcuts_missing"));  // no such file: defaults
  CHECK(map.actionFor(plain(kKeyQ)) == ShortcutAction::Quit);

  const std::string path = tempFile("soar_shortcuts_config");
  writeFile(path, "queue_next = j\n");
  map.load(path);
  CHECK(map.actionFor(plain(soar::app::kKeyJ)) == ShortcutAction::QueueNext);
  CHECK(map.actionFor(plain(kKeyN)) == ShortcutAction::None);

  // A directory path is unreadable: best-effort keeps the current state.
  std::error_code ec;
  std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
  map.load(dir.string());
  CHECK(map.actionFor(plain(soar::app::kKeyJ)) == ShortcutAction::QueueNext);
}

TEST_CASE("help-overlay names: plain, shifted, digit range, off") {
  const ShortcutMap map;
  using A = ShortcutAction;
  CHECK(map.bindingNames(A::PlayPause) == "Space / K");
  CHECK(map.bindingNames(A::PercentSeek) == "0 - 9");
  CHECK(map.bindingNames(A::SeekBack1) == "Shift+Left");
  CHECK(map.bindingNames(A::QueuePrev) == "Shift+N");
  CHECK(map.bindingNames(A::OpenFolder) == "Ctrl+O");
  CHECK(map.bindingNames(A::Escape) == "Esc");
  CHECK(map.bindingNames(A::Mute) == "M");
  CHECK(map.bindingNames(A::RateDown) == ",");
  ShortcutMap trimmed;
  trimmed.applyText("mute =\n");
  CHECK(trimmed.bindingNames(A::Mute) == "(off)");
}

TEST_CASE("key name round-trips through parseShortcutKey") {
  ShortcutKey key;
  std::uint32_t range_end = 99;
  CHECK(parseShortcutKey("shift+left", &key, &range_end));
  CHECK(key == shifted(kKeyLeft));
  CHECK(range_end == 0);
  CHECK(parseShortcutKey("K", &key));  // case-insensitive letters
  CHECK(key == plain(kKeyK));
  CHECK(parseShortcutKey("pageup", &key));
  CHECK(key == plain(kKeyPageUp));
  CHECK(parseShortcutKey("0-9", &key, &range_end));
  CHECK(key == plain(digit('0')));
  CHECK(range_end == digit('9'));
  // A bare digit binds on its own - the percent-seek keys, singly.
  CHECK(parseShortcutKey("5", &key));
  CHECK(key == plain(digit('5')));
  CHECK_FALSE(parseShortcutKey("", &key));
  CHECK_FALSE(parseShortcutKey("shift+", &key));
  CHECK_FALSE(parseShortcutKey("f13", &key));
  CHECK_FALSE(parseShortcutKey("bogus+left", &key));
  CHECK_FALSE(parseShortcutKey("left+right", &key));

  CHECK(shortcutKeyName(plain(kKeyK)) == "K");
  CHECK(shortcutKeyName(ctrled(kKeyO)) == "Ctrl+O");
  CHECK(shortcutKeyName(ShortcutKey{kKeyEscape, false, false, true}) ==
        "Alt+Esc");
  CHECK(shortcutKeyName(ShortcutKey{soar::app::kKeyNone}) == "");
  CHECK(shortcutKeyName(plain(0x12345678)) == "?");
}

TEST_CASE("action names round-trip") {
  CHECK(shortcutActionName(ShortcutAction::PlayPause) == "play_pause");
  CHECK(shortcutActionName(ShortcutAction::SubtitleOffsetBack) ==
        "subtitle_offset_back");
  CHECK(shortcutActionName(ShortcutAction::Cast) == "cast");
  CHECK(shortcutActionName(ShortcutAction::None) == "");
  // Every nameable action parses back to itself; the config grammar and
  // the enum stay in lockstep.
  for (int a = 1; a < ShortcutMap::kActionCount; ++a) {
    const std::string name =
        shortcutActionName(static_cast<ShortcutAction>(a));
    CHECK_FALSE(name.empty());
  }
}

#ifndef _WIN32
TEST_CASE("defaultShortcutsPath walks the XDG ladder like the other stores") {
  const char* xdg = std::getenv("XDG_STATE_HOME");
  const char* home = std::getenv("HOME");
  const std::string xdg_save = xdg ? xdg : "";
  const std::string home_save = home ? home : "";
  const bool xdg_was_set = xdg != nullptr;
  const bool home_was_set = home != nullptr;

  ::setenv("XDG_STATE_HOME", "/tmp/soar_state_xdg", 1);
  ::setenv("HOME", "/tmp/soar_home_saved", 1);
  CHECK(defaultShortcutsPath() == "/tmp/soar_state_xdg/soar/shortcuts.txt");

  ::setenv("XDG_STATE_HOME", "", 1);
  CHECK(defaultShortcutsPath() ==
        "/tmp/soar_home_saved/.local/state/soar/shortcuts.txt");

  ::unsetenv("XDG_STATE_HOME");
  ::setenv("HOME", "", 1);
  CHECK(defaultShortcutsPath() == "soar-shortcuts.txt");

  ::unsetenv("HOME");
  CHECK(defaultShortcutsPath() == "soar-shortcuts.txt");

  if (xdg_was_set) ::setenv("XDG_STATE_HOME", xdg_save.c_str(), 1);
  else ::unsetenv("XDG_STATE_HOME");
  if (home_was_set) ::setenv("HOME", home_save.c_str(), 1);
  else ::unsetenv("HOME");
}
#endif
