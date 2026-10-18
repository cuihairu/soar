// Keyboard shortcut bindings (docs/ui-design.md §3.2) — implementation.
// See shortcuts.h for the config format and the matching rules.

#include "ui/shortcuts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace soar::app {
namespace {

// The name table: config token -> display name -> keycode. Letters and
// digits are generated; the named keys are the SDL scancode-masked
// values (SDLK_*), which player_window.cpp static_asserts against SDL.
struct KeyName {
  const char* name;
  const char* display;
  std::uint32_t sym;
};

const KeyName kKeyNames[] = {
    {"tab", "Tab", kKeyTab},
    {"backspace", "Backspace", kKeyBackspace},
    {"return", "Return", kKeyReturn},
    {"enter", "Enter", kKeyReturn},
    {"escape", "Esc", kKeyEscape},
    {"esc", "Esc", kKeyEscape},
    {"space", "Space", kKeySpace},
    {"apostrophe", "'", kKeyApostrophe},
    {"comma", ",", kKeyComma},
    {"minus", "-", kKeyMinus},
    {"period", ".", kKeyPeriod},
    {"slash", "/", kKeySlash},
    {"semicolon", ";", kKeySemicolon},
    {"equals", "=", kKeyEquals},
    {"leftbracket", "[", kKeyLeftBracket},
    {"backslash", "\\", kKeyBackslash},
    {"rightbracket", "]", kKeyRightBracket},
    {"grave", "`", kKeyGrave},
    {"delete", "Delete", kKeyDelete},
    {"capslock", "CapsLock", kKeyCapsLock},
    {"printscreen", "PrtSc", kKeyPrintScreen},
    {"scrolllock", "ScrollLock", kKeyScrollLock},
    {"pause", "Pause", kKeyPause},
    {"insert", "Insert", kKeyInsert},
    {"home", "Home", kKeyHome},
    {"end", "End", kKeyEnd},
    {"pageup", "PgUp", kKeyPageUp},
    {"pagedown", "PgDn", kKeyPageDown},
    {"up", "Up", kKeyUp},
    {"down", "Down", kKeyDown},
    {"left", "Left", kKeyLeft},
    {"right", "Right", kKeyRight},
    {"menu", "Menu", kKeyMenu},
    {"f1", "F1", kKeyF1},     {"f2", "F2", kKeyF1 + 1},
    {"f3", "F3", kKeyF1 + 2}, {"f4", "F4", kKeyF1 + 3},
    {"f5", "F5", kKeyF1 + 4}, {"f6", "F6", kKeyF1 + 5},
    {"f7", "F7", kKeyF1 + 6}, {"f8", "F8", kKeyF1 + 7},
    {"f9", "F9", kKeyF1 + 8}, {"f10", "F10", kKeyF1 + 9},
    {"f11", "F11", kKeyF1 + 10}, {"f12", "F12", kKeyF1 + 11},
    {"kp_divide", "Num/", kKeyNumDivide},
    {"kp_multiply", "Num*", kKeyNumMultiply},
    {"kp_minus", "Num-", kKeyNumMinus},
    {"kp_plus", "Num+", kKeyNumPlus},
    {"kp_enter", "NumEnter", kKeyNumEnter},
    {"kp_equals", "Num=", kKeyNumEquals},
    {"kp_period", "Num.", kKeyNumPeriod},
    {"kp_0", "Num0", kKeyNum0},   {"kp_1", "Num1", kKeyNum1},
    {"kp_2", "Num2", kKeyNum1 + 1}, {"kp_3", "Num3", kKeyNum1 + 2},
    {"kp_4", "Num4", kKeyNum1 + 3}, {"kp_5", "Num5", kKeyNum1 + 4},
    {"kp_6", "Num6", kKeyNum1 + 5}, {"kp_7", "Num7", kKeyNum1 + 6},
    {"kp_8", "Num8", kKeyNum1 + 7}, {"kp_9", "Num9", kKeyNum1 + 8},
};

std::string lowerCopy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trim(const std::string& s) {
  std::size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  std::size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Splits "a,b,c" on commas, trimming each piece. Empty pieces are dropped
// (a trailing comma is a typo, not a binding).
std::vector<std::string> splitList(const std::string& value) {
  std::vector<std::string> out;
  std::size_t pos = 0;
  while (pos <= value.size()) {
    const std::size_t comma = value.find(',', pos);
    const std::string piece =
        trim(value.substr(pos, comma == std::string::npos ? std::string::npos
                                                          : comma - pos));
    if (!piece.empty()) out.push_back(piece);
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

// The action name table, in enum order (None is not nameable).
const char* const kActionNames[] = {
    "escape",           "play_pause",
    "seek_back_5",      "seek_fwd_5",
    "seek_back_1",      "seek_fwd_1",
    "seek_back_60",     "seek_fwd_60",
    "seek_start",       "percent_seek",
    "volume_up",        "volume_down",
    "mute",             "rate_down",
    "rate_up",          "fullscreen",
    "loop_ab",          "cycle_audio",
    "cycle_subtitle",   "subtitle_offset_back",
    "subtitle_offset_fwd", "subtitle_visibility",
    "screenshot",       "open_file",
    "open_folder",      "playlist",
    "library",          "queue_next",
    "queue_prev",       "info",
    "recent",           "help",
    "cast",             "quit",
    "search_library",   "toggle_theme",
    "open_settings",
};

ShortcutAction actionFromName(const std::string& name) {
  for (std::size_t i = 0; i < sizeof(kActionNames) / sizeof(kActionNames[0]); ++i) {
    if (name == kActionNames[i]) return static_cast<ShortcutAction>(i + 1);
  }
  return ShortcutAction::None;
}

// The shipped defaults (docs/ui-design.md §32). Plain bindings match
// regardless of held modifiers; flagged ones outrank the plain for the
// same key (shift+left is the 1s seek, ctrl+o the folder picker).
void addDefault(std::vector<ShortcutKey>* out, std::uint32_t sym) {
  out->push_back(ShortcutKey{sym});
}

void addDefaultShift(std::vector<ShortcutKey>* out, std::uint32_t sym) {
  out->push_back(ShortcutKey{sym, /*shift=*/true});
}

void addDefaultCtrl(std::vector<ShortcutKey>* out, std::uint32_t sym) {
  out->push_back(ShortcutKey{sym, /*shift=*/false, /*ctrl=*/true});
}

}  // namespace

ShortcutMap::ShortcutMap() {
  for (int a = 0; a < kActionCount; ++a) bindings_[a].clear();
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Escape)], kKeyEscape);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::PlayPause)], kKeySpace);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::PlayPause)], kKeyK);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::SeekBack5)], kKeyLeft);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::SeekFwd5)], kKeyRight);
  addDefaultShift(&bindings_[static_cast<int>(ShortcutAction::SeekBack1)], kKeyLeft);
  addDefaultShift(&bindings_[static_cast<int>(ShortcutAction::SeekFwd1)], kKeyRight);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::SeekBack60)], kKeyPageUp);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::SeekFwd60)], kKeyPageDown);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::SeekStart)], kKeyHome);
  for (std::uint32_t d = kKey0; d <= kKey9; ++d) {
    addDefault(&bindings_[static_cast<int>(ShortcutAction::PercentSeek)], d);
  }
  addDefault(&bindings_[static_cast<int>(ShortcutAction::VolumeUp)], kKeyUp);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::VolumeDown)], kKeyDown);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Mute)], kKeyM);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::RateDown)], kKeyComma);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::RateUp)], kKeyPeriod);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Fullscreen)], kKeyF);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::LoopAB)], kKeyL);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::CycleAudio)], kKeyA);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::CycleSubtitle)], kKeyC);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::SubtitleOffsetBack)],
             kKeyLeftBracket);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::SubtitleOffsetFwd)],
             kKeyRightBracket);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::SubtitleVisibility)], kKeyV);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Screenshot)], kKeyS);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::OpenFile)], kKeyO);
  addDefaultCtrl(&bindings_[static_cast<int>(ShortcutAction::OpenFolder)], kKeyO);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Playlist)], kKeyP);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Library)], kKeyB);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::QueueNext)], kKeyN);
  addDefaultShift(&bindings_[static_cast<int>(ShortcutAction::QueuePrev)], kKeyN);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Info)], kKeyI);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Recent)], kKeyR);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Help)], kKeyH);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Cast)], kKeyT);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::Quit)], kKeyQ);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::SearchLibrary)],
             kKeySlash);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::ToggleTheme)],
             kKeyY);
  addDefault(&bindings_[static_cast<int>(ShortcutAction::OpenSettings)],
             kKeyG);
}

void ShortcutMap::load(const std::string& path) {
  // A directory (or a broken symlink) opens fine on POSIX and yields an
  // empty body, and an empty body would rebuild the defaults and wipe an
  // existing customization. Probe the kind before treating "nothing read"
  // as a config.
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return;
  std::ifstream in(path, std::ios::binary);
  if (!in) return;  // missing/unreadable: the stock player
  std::ostringstream buf;
  buf << in.rdbuf();
  applyText(buf.str());
}

void ShortcutMap::applyText(const std::string& text) {
  ShortcutMap fresh;  // the file fully defines the customization
  bool saw_switch = false;
  bool switch_on = true;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed[0] == '#') continue;
    const std::size_t eq = trimmed.find('=');
    if (eq == std::string::npos) continue;  // malformed: skip the line
    const std::string name = lowerCopy(trim(trimmed.substr(0, eq)));
    std::string value = trim(trimmed.substr(eq + 1));
    // An inline comment runs from '#' (no key name contains one).
    const std::size_t hash = value.find('#');
    if (hash != std::string::npos) value = trim(value.substr(0, hash));
    if (name == "shortcuts") {
      if (value == "off") {
        saw_switch = true;
        switch_on = false;
      } else if (value == "on") {
        saw_switch = true;
        switch_on = true;
      }
      continue;
    }
    const ShortcutAction action = actionFromName(name);
    if (action == ShortcutAction::None) continue;  // unknown action: skip
    std::vector<ShortcutKey> parsed;
    for (const std::string& token : splitList(value)) {
      ShortcutKey key;
      std::uint32_t range_end = 0;
      if (!parseShortcutKey(token, &key, &range_end)) continue;  // unknown key
      parsed.push_back(key);
      for (std::uint32_t s = key.sym + 1; s <= range_end; ++s) {
        ShortcutKey step = key;
        step.sym = s;
        parsed.push_back(step);
      }
    }
    // A non-empty value whose every token failed is a typo, not an
    // unbinding: skip the line so the action keeps its current bindings.
    // Unbinding is the empty value.
    if (!value.empty() && parsed.empty()) continue;
    // The file is read top to bottom and a key claimed by an earlier line
    // is taken over, so `mute = k` works even though play_pause still
    // defaults to k. Stealing compares the full key, so `mute = k` and
    // `rate_up = shift+k` coexist.
    const int self = static_cast<int>(action);
    for (const ShortcutKey& taken : parsed) {
      for (int other = 0; other < kActionCount; ++other) {
        if (other == self) continue;
        std::vector<ShortcutKey>& rest = fresh.bindings_[other];
        rest.erase(std::remove_if(rest.begin(), rest.end(),
                                  [&](const ShortcutKey& k) {
                                    return k.sym == taken.sym &&
                                           k.shift == taken.shift &&
                                           k.ctrl == taken.ctrl &&
                                           k.alt == taken.alt;
                                  }),
                   rest.end());
      }
    }
    fresh.bindings_[static_cast<int>(action)] = std::move(parsed);
  }
  for (int a = 0; a < kActionCount; ++a) {
    bindings_[a] = fresh.bindings_[a];
  }
  if (saw_switch) setEnabled(switch_on);
}

int ShortcutMap::bindingFor(const ShortcutKey& in,
                            ShortcutAction* out_action) const {
  if (out_action != nullptr) *out_action = ShortcutAction::None;
  if (!enabled_ || in.sym == kKeyNone) return -1;
  ShortcutKey key = in;
  if (key.sym >= 'A' && key.sym <= 'Z') key.sym += 'a' - 'A';
  int best = -1;
  int best_index = -1;
  ShortcutAction out = ShortcutAction::None;
  for (int a = 0; a < kActionCount; ++a) {
    const std::vector<ShortcutKey>& list = bindings_[a];
    for (std::size_t i = 0; i < list.size(); ++i) {
      const ShortcutKey& b = list[i];
      if (b.sym != key.sym) continue;
      if (b.shift && b.shift != key.shift) continue;
      if (b.ctrl && b.ctrl != key.ctrl) continue;
      if (b.alt && b.alt != key.alt) continue;
      // Plain bindings match regardless of held modifiers; a flagged
      // binding outranks the plain ones for the same key.
      const int score = (b.shift ? 1 : 0) + (b.ctrl ? 2 : 0) + (b.alt ? 4 : 0);
      if (score > best) {
        best = score;
        best_index = static_cast<int>(i);
        out = static_cast<ShortcutAction>(a);
      }
    }
  }
  if (out_action != nullptr) *out_action = out;
  return out == ShortcutAction::None ? -1 : best_index;
}

ShortcutAction ShortcutMap::actionFor(const ShortcutKey& key) const {
  ShortcutAction out;
  bindingFor(key, &out);
  return out;
}

const std::vector<ShortcutKey>& ShortcutMap::bindings(
    ShortcutAction action) const {
  return bindings_[static_cast<int>(action)];
}

std::string ShortcutMap::bindingNames(ShortcutAction action) const {
  const std::vector<ShortcutKey>& list = bindings(action);
  if (list.empty()) return "(off)";
  // A run of plain single digits collapses to "0 - 9" (the help column
  // is narrow); anything else lists every binding.
  bool all_plain_digits = !list.empty();
  for (const ShortcutKey& k : list) {
    if (k.shift || k.ctrl || k.alt || k.sym < kKey0 || k.sym > kKey9) {
      all_plain_digits = false;
      break;
    }
  }
  if (all_plain_digits && list.size() >= 2) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%c - %c",
                  static_cast<char>(list.front().sym),
                  static_cast<char>(list.back().sym));
    return buf;
  }
  std::string out;
  for (std::size_t i = 0; i < list.size(); ++i) {
    if (i != 0) out += " / ";
    out += shortcutKeyName(list[i]);
  }
  return out;
}

bool parseShortcutKey(const std::string& token, ShortcutKey* out,
                      std::uint32_t* range_end) {
  if (range_end != nullptr) *range_end = 0;
  if (token.empty()) return false;
  // A single-char range ("0-9", "a-z") expands to every code in between.
  if (token.size() == 3 && token[1] == '-') {
    const unsigned char lo = static_cast<unsigned char>(token[0]);
    const unsigned char hi = static_cast<unsigned char>(token[2]);
    if (lo > hi) return false;
    *out = ShortcutKey{static_cast<std::uint32_t>(lo)};
    if (range_end != nullptr) *range_end = static_cast<std::uint32_t>(hi);
    return true;
  }
  // Modifier prefixes: "ctrl+alt+shift+s" — order-free, last piece is the
  // key. A bare "+" never appears in the name table.
  ShortcutKey key;
  std::string rest = lowerCopy(token);
  bool have_key = false;
  while (true) {
    const std::size_t plus = rest.find('+');
    if (plus == std::string::npos) {
      if (have_key) return false;  // two key pieces: malformed
      have_key = true;
      break;
    }
    const std::string mod = rest.substr(0, plus);
    if (mod == "shift") {
      key.shift = true;
    } else if (mod == "ctrl" || mod == "control") {
      key.ctrl = true;
    } else if (mod == "alt") {
      key.alt = true;
    } else {
      return false;  // unknown modifier
    }
    rest = rest.substr(plus + 1);
  }
  if (!have_key || rest.empty()) return false;
  if (rest.size() == 1) {
    const char c = rest[0];
    if (c >= 'a' && c <= 'z') {
      key.sym = static_cast<std::uint32_t>(c);
      *out = key;
      return true;
    }
    if (c >= '0' && c <= '9') {
      key.sym = static_cast<std::uint32_t>(c);
      *out = key;
      return true;
    }
  }
  for (const KeyName& kn : kKeyNames) {
    if (rest == kn.name) {
      key.sym = kn.sym;
      *out = key;
      return true;
    }
  }
  return false;
}

std::string shortcutKeyName(const ShortcutKey& key) {
  if (key.sym == kKeyNone) return "";
  // Conventional modifier order: Ctrl+Alt+Shift (rightmost prepended
  // first).
  const auto decorate = [](const std::string& name, const ShortcutKey& k) {
    std::string out = name;
    if (k.shift) out = "Shift+" + out;
    if (k.alt) out = "Alt+" + out;
    if (k.ctrl) out = "Ctrl+" + out;
    return out;
  };
  if (key.sym >= 'a' && key.sym <= 'z') {
    return decorate(std::string(1, static_cast<char>(key.sym - ('a' - 'A'))),
                    key);
  }
  for (const KeyName& kn : kKeyNames) {
    if (key.sym == kn.sym) return decorate(kn.display, key);
  }
  return "?";
}

std::string shortcutActionName(ShortcutAction action) {
  const int a = static_cast<int>(action);
  if (a <= 0 || a >= ShortcutMap::kActionCount) return "";
  return kActionNames[a - 1];
}

std::string defaultShortcutsPath() {
  const char* dir = nullptr;
#ifdef _WIN32
  dir = std::getenv("APPDATA");
  if (dir && *dir) return std::string(dir) + "\\soar\\shortcuts.txt";
  return "soar-shortcuts.txt";
#else
  dir = std::getenv("XDG_STATE_HOME");
  if (dir && *dir) return std::string(dir) + "/soar/shortcuts.txt";
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.local/state/soar/shortcuts.txt";
  }
  return "soar-shortcuts.txt";
#endif
}

}  // namespace soar::app
