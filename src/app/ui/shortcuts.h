// Keyboard shortcut bindings (docs/ui-design.md §3.2). One action per
// behavior the window handles; the map decides which key fires it.
//
// Pure logic over keycode integers — no SDL dependency, same contract as
// ui_state/dir_scan/library. SDL's SDL_Keycode IS std::uint32_t, so the
// window hands event keysyms straight in; kKey* below mirrors the SDL
// values and player_window.cpp static_asserts the equality, so any drift
// between the two tables fails the window build instead of silently
// rebinding keys.
//
// Defaults are the shipped table, byte for byte: a missing or unreadable
// config file is the stock player. Customization comes from a hand-edited
// line file (defaultShortcutsPath(), next to recent.txt/library.txt in the
// state dir):
//
//   # comment
//   play_pause = space, k     # list of keys; a listed action's defaults
//                             # are dropped, not extended
//   percent_seek = 0-9        # single-char range expands to ten keys
//   quit =                    # empty value = action disabled
//   shortcuts = off           # global kill switch (on restores)
//
// `name = shift+key / ctrl+key / alt+key` prefixes flag modifiers, e.g.
// `screenshot = ctrl+alt+s`; a `#` starts a comment (whole line or after
// the value). Unknown actions, unknown key names and malformed lines are
// skipped — and a line whose every key token fails keeps the action's
// binding, so a typo never silently disables anything: unbinding is the
// empty value. A key claimed by two lines goes to the later one: the file
// is read top to bottom and the newest binding takes the key over, so
// `mute = k` works even though play_pause still defaults to k.

#ifndef SOAR_APP_UI_SHORTCUTS_H_
#define SOAR_APP_UI_SHORTCUTS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace soar::app {

// Keycode values (SDL_Keycode is Uint32; ASCII keys are their code point,
// the rest carry SDL's SDLK_SCANCODE_MASK). player_window.cpp pins every
// value against SDL's own constants at compile time.
enum Key : std::uint32_t {
  kKeyNone = 0,
  kKeyTab = 9,
  kKeyBackspace = 8,
  kKeyReturn = 13,
  kKeyEscape = 27,
  kKeySpace = 32,
  kKeyApostrophe = 39,
  kKeyComma = 44,
  kKeyMinus = 45,
  kKeyPeriod = 46,
  kKeySlash = 47,
  kKey0 = 48, kKey9 = 57,
  kKeySemicolon = 59,
  kKeyEquals = 61,
  kKeyLeftBracket = 91,
  kKeyBackslash = 92,
  kKeyRightBracket = 93,
  kKeyGrave = 96,
  kKeyA = 97,  kKeyB = 98,  kKeyC = 99,  kKeyD = 100, kKeyE = 101,
  kKeyF = 102,  kKeyG = 103, kKeyH = 104, kKeyI = 105, kKeyJ = 106,
  kKeyK = 107,  kKeyL = 108, kKeyM = 109, kKeyN = 110, kKeyO = 111,
  kKeyP = 112,  kKeyQ = 113, kKeyR = 114, kKeyS = 115, kKeyT = 116,
  kKeyU = 117,  kKeyV = 118, kKeyW = 119, kKeyX = 120, kKeyY = 121,
  kKeyZ = 122,
  kKeyDelete = 127,  // SDL keeps DELETE in the ASCII range (DEL), not the
                     // scancode-mask block; the mask covers kKeyCapsLock+
  kKeyCapsLock = 0x40000039,
  kKeyF1 = 0x4000003A, kKeyF12 = 0x40000045,
  kKeyPrintScreen = 0x40000046,
  kKeyScrollLock = 0x40000047,
  kKeyPause = 0x40000048,
  kKeyInsert = 0x40000049,
  kKeyHome = 0x4000004A,
  kKeyPageUp = 0x4000004B,
  kKeyEnd = 0x4000004D,
  kKeyPageDown = 0x4000004E,
  kKeyRight = 0x4000004F,
  kKeyLeft = 0x40000050,
  kKeyDown = 0x40000051,
  kKeyUp = 0x40000052,
  kKeyNumDivide = 0x40000054,
  kKeyNumMultiply = 0x40000055,
  kKeyNumMinus = 0x40000056,
  kKeyNumPlus = 0x40000057,
  kKeyNumEnter = 0x40000058,
  kKeyNum1 = 0x40000059, kKeyNum9 = 0x40000061,
  kKeyNum0 = 0x40000062,
  kKeyNumPeriod = 0x40000063,
  kKeyMenu = 0x40000076,
  kKeyNumEquals = 0x40000067,
};

// One physical binding: a keycode plus the modifier flags the event must
// carry. Plain bindings (no flags) match regardless of held modifiers —
// shift+q still quits — while a flagged binding outranks the plain one for
// the same key (shift+left is the 1s seek, not the 5s one).
struct ShortcutKey {
  std::uint32_t sym = kKeyNone;
  bool shift = false;
  bool ctrl = false;
  bool alt = false;
};

inline bool operator==(const ShortcutKey& a, const ShortcutKey& b) {
  return a.sym == b.sym && a.shift == b.shift && a.ctrl == b.ctrl &&
         a.alt == b.alt;
}

// The behaviors, in the order docs/ui-design.md §3.2 lists them. A key
// bound to two lines goes to the later line (see the header comment), so
// this order only decides which line wins when the file is ambiguous.
enum class ShortcutAction {
  None = 0,
  Escape,             // fullscreen: leave; otherwise quit
  PlayPause,          // space / k
  SeekBack5, SeekFwd5,
  SeekBack1, SeekFwd1,
  SeekBack60, SeekFwd60,
  SeekStart,          // home
  PercentSeek,        // 0-9 -> 0%-90%
  VolumeUp, VolumeDown,
  Mute,
  RateDown, RateUp,
  Fullscreen,
  LoopAB,
  CycleAudio, CycleSubtitle,
  SubtitleOffsetBack, SubtitleOffsetFwd,
  SubtitleVisibility,
  Screenshot,
  OpenFile,           // plain o (Windows dialog / drop hint)
  OpenFolder,         // ctrl+o (the in-app folder picker)
  Playlist, Library,
  QueueNext, QueuePrev,
  Info, Recent, Help,
  Cast,               // t: the DLNA cast overlay (docs/mvp.md §3)
  Quit,
  SearchLibrary,      // /: focus the library overlay's search box
  ToggleTheme,        // y: light/dark palette (proto.css [data-theme])
  OpenSettings,       // g: the settings panel (proto redesign/settings.html)
};

class ShortcutMap {
 public:
  // The shipped table from docs/ui-design.md §3.2.
  ShortcutMap();

  // Global kill switch (--no-shortcuts / `shortcuts = off`). Enabled by
  // default; the defaults are still loaded while disabled, so `on`
  // restores them untouched.
  bool enabled() const { return enabled_; }
  void setEnabled(bool on) { enabled_ = on; }

  // Best-effort load: a missing or unreadable file keeps the current
  // state (the stock player on first run). Same call family as
  // RecentStore/MediaLibrary.
  void load(const std::string& path);

  // Applies one config body (lines as in the header comment). Bindings
  // are rebuilt from the defaults plus this text — the file fully
  // defines the customization, so re-reading gives the same map. The
  // kill switch is only touched when the text carries a `shortcuts =`
  // line, so setEnabled() survives re-reads that say nothing about it.
  void applyText(const std::string& text);

  // Which action a keydown fires. `sym` may arrive uppercase (SDL
  // reports shifted letters) — normalized here like the window does.
  ShortcutAction actionFor(const ShortcutKey& key) const;

  // actionFor plus the matched binding's index in bindings(action) —
  // parameterized actions (percent_seek: which digit fired) read the key
  // back from there. -1 and None when nothing fires.
  int bindingFor(const ShortcutKey& key,
                 ShortcutAction* out_action = nullptr) const;

  // The bindings an action currently has; empty for a disabled action.
  const std::vector<ShortcutKey>& bindings(ShortcutAction action) const;

  // Human-facing key column for the help overlay: "Space / K", the
  // digit ranges collapsed ("0 - 9"), "(off)" when unbound.
  std::string bindingNames(ShortcutAction action) const;

  static constexpr int kActionCount =
      static_cast<int>(ShortcutAction::OpenSettings) + 1;

 private:
  std::vector<ShortcutKey> bindings_[kActionCount];
  bool enabled_ = true;
};

// Parses one config token ("k", "shift+left", "ctrl+alt+f5", "0-9" range
// -> fills `out` with the first key and returns the range end via `end`,
// nullptr/0 when not a range). False for an unknown name.
bool parseShortcutKey(const std::string& token, ShortcutKey* out,
                      std::uint32_t* range_end = nullptr);

// The reverse of the name table: "Shift+Left" etc. Empty for kKeyNone.
std::string shortcutKeyName(const ShortcutKey& key);

// Action names as they appear on the left of config lines. Empty for None.
std::string shortcutActionName(ShortcutAction action);

// <state>/soar/shortcuts.txt — the XDG ladder recent.txt/library.txt use
// (APPDATA on Windows, cwd fallback last). Hand-edited config living in
// the state dir is the repo's one-directory convention: everything soar
// writes sits next to each other.
std::string defaultShortcutsPath();

}  // namespace soar::app

#endif  // SOAR_APP_UI_SHORTCUTS_H_
