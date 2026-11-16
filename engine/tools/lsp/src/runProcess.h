#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

// Runs a program to completion with text on its stdin, for sfs-lsp's
// formatter (clang-format).
namespace sfs::lsp
{

/** What a finished process gave back. */
struct ProcessResult
{
  /** It was started (false: not found, or the OS refused). */
  bool started = false;
  /** It ran past the time allowed and was killed. */
  bool timedOut = false;
  /** Its exit code; -1 when it didn't start or was killed. */
  int exitCode = -1;
  /** Everything it wrote to stdout. */
  std::string output;
  /** Everything it wrote to stderr. */
  std::string errors;
};

/**
 * Runs `args[0]` (a path, or a name looked up on PATH) with the other
 * arguments, writes `input` to its stdin, closes it, and waits for it to exit,
 * reading stdout and stderr as it goes (so neither pipe fills and blocks it).
 *
 * @param args the program and its arguments, unquoted
 * @param input the text for its stdin
 * @param limit how long it may run before it's killed
 * @return what it wrote and its exit code
 */
ProcessResult runProcess(const std::vector<std::string>& args,
                         std::string_view input,
                         std::chrono::milliseconds limit);

} // namespace sfs::lsp
