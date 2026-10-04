#pragma once

// Shared application entry point. The console front end (soar.exe /
// `soar`) calls it straight from main; the Windows GUI front end
// (soarw.exe, winmain.cpp) converts the wide command line to UTF-8
// argv and calls it from WinMain. Both binaries run identical argument
// handling and the same startup_report visibility for early exits.
int soarAppMain(int argc, char** argv);
