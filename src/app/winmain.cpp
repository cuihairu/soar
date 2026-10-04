// soarw.exe — the GUI-subsystem twin of the console soar.exe (Windows
// double-click story, BUGS.md "black box flash"). WIN32_EXECUTABLE keeps
// the console away; early exits (usage, open failures) surface through
// startup_report's dialog and log instead of a console that dies with
// the process.
#ifdef _WIN32

#  include "app_main.h"

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>

#  include <memory>
#  include <vector>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  int argc = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!wide) {
    return 2;
  }
  // Wide argv -> UTF-8: everything downstream (fmt, std::filesystem, the
  // FFmpeg/SDL layers) speaks UTF-8, so non-ASCII media paths must
  // survive the boundary. The buffers outlive argv (soarAppMain only
  // reads them), and the vector is null-terminated like a classic argv.
  std::vector<std::unique_ptr<char[]>> owned;
  std::vector<char*> argv;
  owned.reserve(argc);
  argv.reserve(static_cast<std::size_t>(argc) + 1);
  for (int i = 0; i < argc; ++i) {
    int bytes = WideCharToMultiByte(
        CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) bytes = 1;
    auto utf8 = std::make_unique<char[]>(static_cast<std::size_t>(bytes));
    WideCharToMultiByte(
        CP_UTF8, 0, wide[i], -1, utf8.get(), bytes, nullptr, nullptr);
    argv.push_back(utf8.get());
    owned.push_back(std::move(utf8));
  }
  argv.push_back(nullptr);
  LocalFree(wide);
  // gui_entry=true: this front end is the GUI subsystem one — launching
  // it with no media source must land in the empty player window, never
  // in the CLI usage exit (BUGS.md #3).
  return soarAppMain(argc, argv.data(), true);
}

#endif  // _WIN32
