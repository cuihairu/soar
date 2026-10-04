#pragma once

#include <string>

namespace soar::app {

// Startup failure visibility (BUGS.md, 2026-10 "black box flash"): a
// double-clicked console-subsystem binary gets a fresh console from
// Windows, prints usage or an early error into it, and the console dies
// with the process — the user sees a black box flash and nothing else.
// These helpers make every early exit leave a trace the user can still
// find: a message box (Windows only, and only when no interactive
// stdout exists) and, for real failures, one timestamped line in a log
// file.
//
// SOAR_NO_DIALOG=1 suppresses the dialog (CI/automation); SOAR_STARTUP_LOG
// overrides the log path (tests — an empty value disables logging).

// Usage-class early exits (no arguments, unknown option/backend, no
// source): expected behavior, not a failure — dialog only, no log line.
void showStartupNotice(const std::string& title, const std::string& message);

// Failure-class early exits (source open failed, torrent start failed,
// SDL init failed, ...): appends one timestamped line to the startup
// log first, then shows the dialog the same way.
void reportFatalStartupError(const std::string& title, const std::string& message);

// Appends `message` behind an ISO-8601 local timestamp to the startup
// failure log. Best-effort by design: an unwritable log location must
// never take the app down on its way out.
void logStartupFailure(const std::string& message);

}  // namespace soar::app
