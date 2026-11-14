#pragma once

#include "document.h"
#include "lints.h"
#include "lspFeatures.h"
#include "projectIndex.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
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
  /** How long a document must be still before it's analysed again. */
  static constexpr std::chrono::milliseconds kSettle{100};

  /** @param logTiming log each change's and analysis's cost to stderr */
  explicit Server(bool logTiming);
  ~Server();

  /**
   * Serves stdin and stdout until `exit` or the end of stdin; returns the exit
   * code.
   */
  int run();

  /**
   * The messages the reader thread has cut from stdin, waiting to be handled.
   */
  struct Input;

private:
  void handle(const std::string& body);
  void handleRequest(const nlohmann::json& id,
                     const std::string& method,
                     const nlohmann::json& params);
  void handleNotification(const std::string& method,
                          const nlohmann::json& params);
  nlohmann::json initialize(const nlohmann::json& params);
  void send(const nlohmann::json& message);

  Document* find(const nlohmann::json& textDocument);
  const Document* openDocument(const std::filesystem::path& path) const;
  void open(const nlohmann::json& params);
  void change(const nlohmann::json& params);
  void save(const nlohmann::json& params);
  void close(const nlohmann::json& params);
  void analyseAndPublish(Document& document,
                         codegen::ParseResult* parsed = nullptr);
  void publish(const Document& document);
  void settle();
  std::optional<std::chrono::steady_clock::time_point> nextSettle() const;
  void indexIdle();

  std::shared_ptr<Input> m_input; // shared with the reader thread
  bool m_logTiming = false;
  bool m_initialized = false;
  bool m_shutdown = false;
  std::optional<int> m_exitCode;
  ClientSettings m_settings;
  std::filesystem::path m_root;
  std::vector<std::unique_ptr<Document>> m_documents;
  ParserPtr m_parser;
  FileReader m_reader; // open documents' text, else the file on disk
  Linter m_linter;
  ProjectIndex m_index;
  std::string m_output; // each message framed, reused
};

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

} // namespace sfs::lsp
