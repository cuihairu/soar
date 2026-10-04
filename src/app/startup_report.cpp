#include "startup_report.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace soar::app {
namespace {

// SOAR_STARTUP_LOG overrides the location (tests, automation); an empty
// override means "log nowhere". Default: the temp directory, next to
// where the torrent store and CI scratch live.
std::string startupLogPath() {
  if (const char* override_path = std::getenv("SOAR_STARTUP_LOG")) {
    return std::string(override_path);
  }
  return (std::filesystem::temp_directory_path() /
          "soar-startup-failures.log")
      .string();
}

std::string timestampNow() {
  const std::time_t now = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  std::tm tm_buf{};
#ifdef _WIN32
  localtime_s(&tm_buf, &now);
#else
  localtime_r(&now, &tm_buf);
#endif
  char buf[24];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);
  return std::string(buf);
}

#ifdef _WIN32
// A dialog only helps when the user has no other channel for the
// message. The flash cases are:
//   - a GUI-subsystem exe double-clicked from Explorer (no console,
//     stdout handles are NULL), and
//   - a console exe double-clicked from Explorer (owns the freshly
//     spawned console alone, and that console dies with the process).
// Everything else already prints somewhere readable — a terminal child
// shares the shell's console, CI captures through pipes, a redirected
// run writes a file — and a modal dialog there would only hang
// automation.
bool dialogWouldHelp() {
  if (const char* no_dialog = std::getenv("SOAR_NO_DIALOG")) {
    if (std::string(no_dialog) == "1") return false;
  }
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != nullptr && out != INVALID_HANDLE_VALUE) {
    const DWORD file_type = GetFileType(out);
    // Captured to a pipe (CI, `| Out-String`) or a file (>): the parent
    // reads us; printing is enough. FILE_TYPE_CHAR (a console screen
    // buffer) falls through to the console checks below.
    if (file_type == FILE_TYPE_PIPE || file_type == FILE_TYPE_DISK) {
      return false;
    }
  }
  if (GetConsoleWindow() == nullptr) {
    // No console at all: the GUI-subsystem exe double-clicked case.
    return true;
  }
  // Console attached. Sharing it with a shell (>1 process) means the
  // text stays on screen after we exit; alone means Windows spawned the
  // console just for us and it closes with the process.
  DWORD attached[2] = {};
  return GetConsoleProcessList(2, attached) <= 1;
}

std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int wide_len = MultiByteToWideChar(
      CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(wide_len > 0 ? wide_len : 0), L'\0');
  if (wide.empty()) return wide;
  MultiByteToWideChar(
      CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), &wide[0],
      static_cast<int>(wide.size()));
  return wide;
}

void maybeShowDialog(const std::string& title, const std::string& message,
                     bool fatal) {
  if (!dialogWouldHelp()) return;
  MessageBoxW(nullptr, widen(message).c_str(), widen(title).c_str(),
              MB_OK | (fatal ? MB_ICONERROR : MB_ICONINFORMATION));
}
#else
void maybeShowDialog(const std::string& title, const std::string& message,
                     bool fatal) {
  // No dialog channel off Windows; the print + log carry the message.
  (void)title;
  (void)message;
  (void)fatal;
}
#endif

}  // namespace

void logStartupFailure(const std::string& message) {
  const std::string path = startupLogPath();
  std::ofstream out(path, std::ios::app);
  if (!out) return;
  out << timestampNow() << " " << message << "\n";
}

void showStartupNotice(const std::string& title, const std::string& message) {
  maybeShowDialog(title, message, /*fatal=*/false);
}

void reportFatalStartupError(const std::string& title,
                             const std::string& message) {
  logStartupFailure(title + ": " + message);
  maybeShowDialog(title, message, /*fatal=*/true);
}

}  // namespace soar::app
