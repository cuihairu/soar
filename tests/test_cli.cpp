// Integration tests for the soar CLI (src/app/main.cpp). Each case runs
// the real executable as a subprocess and asserts the exit code and the
// stderr report. Only the headless and error paths are exercised: the SDL
// window loop cannot be automated safely on every CI runner.
//
// SOAR_CLI_EXECUTABLE is passed in by tests/CMakeLists.txt as the path to
// the freshly built soar binary.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <cstdlib>
#  define SOAR_POPEN _popen
#  define SOAR_PCLOSE _pclose
#else
#  include <sys/wait.h>
#  define SOAR_POPEN popen
#  define SOAR_PCLOSE pclose
#endif

#ifndef SOAR_CLI_EXECUTABLE
#  define SOAR_CLI_EXECUTABLE "soar"
#endif

namespace {

struct RunResult {
  int exit_code = -1;
  std::string output;
};

RunResult runCli(const std::vector<std::string>& args) {
  std::string cmd(1, '"');
  cmd += SOAR_CLI_EXECUTABLE;
  cmd += '"';
  for (const auto& arg : args) {
    cmd += ' ';
    cmd += '"';
    cmd += arg;
    cmd += '"';
  }
  cmd += " 2>&1"; // merge stderr into the captured stream

  RunResult result;
  FILE* pipe = SOAR_POPEN(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[512];
  std::size_t n = 0;
  while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    result.output.append(buffer, n);
  }

#ifdef _WIN32
  result.exit_code = SOAR_PCLOSE(pipe);
#else
  const int status = SOAR_PCLOSE(pipe);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  return result;
}

} // namespace

TEST_CASE("no arguments prints usage and exits with 2") {
  const auto run = runCli({});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Usage:") != std::string::npos);
}

TEST_CASE("unknown option prints usage and exits with 2") {
  const auto run = runCli({"--bogus", "asset://sample"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Unknown option: --bogus") != std::string::npos);
  CHECK(run.output.find("Usage:") != std::string::npos);
}

TEST_CASE("options without a source URI exit with 2") {
  const auto run = runCli({"--headless"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Usage:") != std::string::npos);
}

TEST_CASE("unknown backend name exits with 2") {
  const auto run = runCli({"--backend=walrus", "asset://sample"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Unknown backend type: walrus") != std::string::npos);
  CHECK(run.output.find("Available backends: null") != std::string::npos);
}

TEST_CASE("headless run over the null backend succeeds") {
  const auto run = runCli({"--headless", "--backend=null", "asset://sample"});
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("Using null backend") != std::string::npos);
  // The headless flow announces the media and drives seek/pause/stop.
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.output.find("event: media-info") != std::string::npos);
}

TEST_CASE("headless run over an unopenable source fails with 1") {
  // "fail-open" makes the null backend simulate a failed open.
  const auto run = runCli({"--headless", "--backend=null", "asset://fail-open.mp4"});
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("Failed to open source: asset://fail-open.mp4") != std::string::npos);
}

TEST_CASE("ffmpeg request degrades gracefully when unavailable") {
  const auto run = runCli({"--headless", "--backend=ffmpeg", "asset://sample"});
#ifdef SOAR_WITH_FFMPEG
  // With the real backend the magic asset:// URI is not openable media.
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("Failed to open source") != std::string::npos);
#else
  // Without FFmpeg support the CLI falls back to the null backend.
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("Falling back to null backend") != std::string::npos);
#endif
}
