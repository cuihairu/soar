#pragma once

// Shared application entry point. The console front end (soar.exe /
// `soar`) calls it straight from main; the Windows GUI front end
// (soarw.exe, winmain.cpp) converts the wide command line to UTF-8
// argv and calls it from WinMain. Both binaries run identical argument
// handling and the same startup_report visibility for early exits.
//
// gui_entry marks the GUI front end (or an explicit --gui): with no
// media source it opens the empty player window instead of taking the
// CLI usage exit — the double-clicked shortcut is a launch, not a
// command line (BUGS.md #3).
int soarAppMain(int argc, char** argv, bool gui_entry = false);
