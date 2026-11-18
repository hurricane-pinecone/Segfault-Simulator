#pragma once

#include "document.h"
#include "lints.h"
#include "lspFeatures.h"
#include "projectIndex.h"
#include "sfs/codegen/model.h"
#include "sfs/codegen/rules.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct TSParser;

namespace sfs::lsp
{

/**
 * sfs-lsp's language server over stdio. A reader thread cuts messages from
 * stdin; the main thread handles them in order, writes every reply, and
 * analyses a changed document once it has been still for kSettle (or when a
 * request needs it). While idle it builds the project index in slices.
 */
class Server
{
public:
  /**
   * The messages the reader thread has cut from stdin, waiting to be handled.
   */
  struct Input;

  /** @param logTiming log each change's and analysis's cost to stderr */
  explicit Server(bool logTiming);
  ~Server();

  /**
   * Serves stdin and stdout until `exit` or the end of stdin; returns the exit
   * code.
   */
  int run();

  /** How long a document must be still before it's analysed again. */
  static constexpr std::chrono::milliseconds kSettle{100};

private:
  void analyseAndPublish(Document& document,
                         codegen::ParseResult* parsed = nullptr);
  void change(const nlohmann::json& params);
  void close(const nlohmann::json& params);
  Document* find(const nlohmann::json& textDocument);
  nlohmann::json
  format(const Document& document, const nlohmann::json& params, bool ranged);
  void handle(const std::string& body);
  void handleNotification(const std::string& method,
                          const nlohmann::json& params);
  void handleRequest(const nlohmann::json& id,
                     const std::string& method,
                     const nlohmann::json& params);
  void indexIdle();
  nlohmann::json initialize(const nlohmann::json& params);
  std::optional<std::chrono::steady_clock::time_point> nextSettle() const;
  void open(const nlohmann::json& params);
  const Document* openDocument(const std::filesystem::path& path) const;
  void publish(const Document& document);
  void save(const nlohmann::json& params);
  void send(const nlohmann::json& message);
  void settle();
  void showMessage(int type, const std::string& message);

  std::shared_ptr<Input> m_input; // shared with the reader thread
  bool m_logTiming = false;
  bool m_initialized = false;
  bool m_shutdown = false;
  std::optional<int> m_exitCode;
  ClientSettings m_settings;
  std::string m_clangFormat; // initializationOptions.clangFormat
  bool m_toldNoClangFormat = false;
  std::filesystem::path m_root;
  std::vector<std::unique_ptr<Document>> m_documents;
  ParserPtr m_parser;
  FileReader m_reader; // open documents' text, else the file on disk
  Linter m_linter;
  ProjectIndex m_index;
  std::string m_output; // each message framed, reused
};

/** What a command-line run (--check, --fix, --format, --graph) is run on. */
struct CommandRun
{
  /** The root: --root's, else the nearest project's or engine's folder. */
  std::filesystem::path root;
  /** The rules --disable turned off. */
  codegen::RuleSet disabled;
  /** The C++ files the paths name, sorted, each once. */
  std::vector<std::filesystem::path> files;
};

/**
 * Reads the arguments the command-line runs share, `[--root DIR] [--disable
 * RULE]... PATH...`, and lists the C++ files the paths name (a folder's,
 * recursively). Relative paths are the root's when one is given, else the
 * working folder's.
 *
 * @param args the arguments
 * @param allowDisable whether --disable is taken
 * @param run filled in
 * @return false when they're wrong (with the usage printed) or name no C++
 *   files (with a message)
 */
bool readCommandRun(const std::vector<std::string>& args,
                    bool allowDisable,
                    CommandRun& run);

/**
 * Turns a rule off for the session (disabledRules, --disable). Errors and
 * bad-ignore can't be turned off.
 *
 * @param name the rule's id
 * @param disabled the set to add it to
 * @return empty, or why it wasn't turned off
 */
std::string disableRule(std::string_view name, codegen::RuleSet& disabled);

/**
 * Checks files from the command line and prints what it finds as
 * `file:line:col: severity: message [rule]` (sfs-lsp --check).
 *
 * @param args the arguments after --check: [--root DIR] [--disable RULE]...
 * PATH...
 * @return 0, or 1 when an error was found; 2 for bad arguments
 */
int runCheck(const std::vector<std::string>& args);

/**
 * Applies the named rules' fixes to files from the command line (sfs-lsp
 * --fix): each rule in turn, file by file, its fixes that don't overlap
 * applied and the file linted again, until none is left or 8 passes. A fix
 * may edit another file too (a class's header). Prints each fix applied as
 * `file:line:col: fixed [rule] title`, then what's left of the rules (and any
 * error) as --check does. Writes each changed file atomically; never formats.
 *
 * @param args the arguments after --fix: RULE[,RULE...] [--root DIR] PATH...
 * @return 0, or 1 when an error was found or a file couldn't be written; 2
 *   for bad arguments
 */
int runFix(const std::vector<std::string>& args);

/**
 * Formats files from the command line (sfs-lsp --format): clang-format, then
 * the house passes under a .sfs-format, each file written in place. With
 * --check nothing is written: it lists the files that would change. Notes (a
 * class that kept its order, and why) go to stderr.
 *
 * @param args the arguments after --format: [--check] [--clang-format PATH]
 *   [--root DIR] PATH...
 * @return 0; 1 when --check found a file to change, or a file couldn't be
 *   formatted or written; 2 for bad arguments or no clang-format
 */
int runFormat(const std::vector<std::string>& args);

} // namespace sfs::lsp
