#include "server.h"

#include "analysis.h"

#include <sfs/codegen/parser.h>
#include <sfs/lsp/protocol.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
  #include <io.h>
#else
  #include <unistd.h>
#endif

namespace sfs::lsp
{

namespace
{

using Clock = std::chrono::steady_clock;

double millisecondsSince(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start)
      .count();
}

void log(const std::string& line)
{
  std::cerr << "sfs-lsp: " << line << std::endl;
}

// `json` at a path of object keys, or null where one is missing.
const nlohmann::json& at(const nlohmann::json& json,
                         std::initializer_list<const char*> path)
{
  static const nlohmann::json kNull;
  const nlohmann::json* node = &json;
  for (const char* key : path)
  {
    if (!node->is_object())
      return kNull;
    const auto it = node->find(key);
    if (it == node->end())
      return kNull;
    node = &*it;
  }
  return *node;
}

} // namespace

std::string disableRule(std::string_view name, codegen::RuleSet& disabled)
{
  const codegen::RuleInfo* rule = codegen::findRule(name);
  if (!rule)
    return "unknown rule '" + std::string(name) + "' can't be disabled";
  if (!codegen::canIgnore(rule->id))
    return std::string(name) + " can't be disabled";
  disabled.set(static_cast<std::size_t>(rule->id));
  return {};
}

struct Server::Input
{
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::string> messages;
  bool closed = false;
};

namespace
{

// The reader thread: blocks on stdin and queues each whole message.
void readInput(std::shared_ptr<Server::Input> input)
{
  MessageReader reader;
  std::vector<std::string> messages;
  std::vector<std::string> problems;
  std::vector<char> buffer(64 * 1024);
  while (true)
  {
#if defined(_WIN32)
    const int count =
        _read(0, buffer.data(), static_cast<unsigned>(buffer.size()));
#else
    const ssize_t count = ::read(0, buffer.data(), buffer.size());
#endif
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    reader.feed(
        std::string_view(buffer.data(), static_cast<std::size_t>(count)),
        messages,
        problems);
    for (const std::string& problem : problems)
      log(problem);
    problems.clear();
    if (messages.empty())
      continue;
    {
      const std::lock_guard lock(input->mutex);
      for (std::string& message : messages)
        input->messages.push_back(std::move(message));
    }
    messages.clear();
    input->ready.notify_one();
  }
  {
    const std::lock_guard lock(input->mutex);
    input->closed = true;
  }
  input->ready.notify_one();
}

} // namespace

Server::Server(bool logTiming)
    : m_input(std::make_shared<Input>()), m_logTiming(logTiming),
      m_parser(makeCppParser()),
      m_reader(
          [this](const std::filesystem::path& path)
          {
            const Document* open = openDocument(path);
            return open ? open->text : codegen::readFile(path);
          })
{
}

Server::~Server() { m_documents.clear(); }

int Server::run()
{
  // The reader may still be blocked on stdin when the server exits; it only
  // touches the shared input, which it keeps alive itself.
  std::thread(readInput, m_input).detach();
  while (!m_exitCode)
  {
    std::deque<std::string> batch;
    bool closed = false;
    {
      std::unique_lock lock(m_input->mutex);
      while (m_input->messages.empty() && !m_input->closed &&
             !m_index.pending())
      {
        const std::optional<Clock::time_point> deadline = nextSettle();
        if (!deadline)
          m_input->ready.wait(lock);
        else if (m_input->ready.wait_until(lock, *deadline) ==
                 std::cv_status::timeout)
          break;
      }
      batch.swap(m_input->messages);
      closed = m_input->closed;
    }
    for (const std::string& body : batch)
    {
      handle(body);
      if (m_exitCode)
        return *m_exitCode;
    }
    settle();
    if (batch.empty())
    {
      if (closed)
        return m_shutdown ? 0 : 1;
      indexIdle();
    }
  }
  return *m_exitCode;
}

void Server::send(const nlohmann::json& message)
{
  m_output.clear();
  appendFrame(m_output, dump(message));
  std::fwrite(m_output.data(), 1, m_output.size(), stdout);
  std::fflush(stdout);
}

void Server::handle(const std::string& body)
{
  nlohmann::json message;
  try
  {
    message = nlohmann::json::parse(body);
  }
  catch (const std::exception& error)
  {
    log(std::string("a message isn't JSON: ") + error.what());
    send(errorResponse(nullptr, errors::kParseError, "the message isn't JSON"));
    return;
  }
  const bool isRequest = message.is_object() && message.contains("id") &&
                         message.contains("method");
  const nlohmann::json id = isRequest ? message["id"] : nlohmann::json();
  try
  {
    if (!message.is_object() || !message.contains("method") ||
        !message["method"].is_string())
    {
      // A response to a request: sfs-lsp sends none, so there's nothing to do.
      if (!message.is_object() || !message.contains("id"))
        send(errorResponse(
            nullptr, errors::kInvalidRequest, "not a JSON-RPC message"));
      return;
    }
    static const nlohmann::json kNoParams = nlohmann::json::object();
    const auto params = message.find("params");
    const nlohmann::json& args = params != message.end() ? *params : kNoParams;
    const std::string method = message["method"];
    if (isRequest)
      handleRequest(id, method, args);
    else
      handleNotification(method, args);
  }
  catch (const std::exception& error)
  {
    log(std::string("a message failed: ") + error.what());
    if (isRequest)
      send(errorResponse(id, errors::kInternalError, error.what()));
  }
}

void Server::handleRequest(const nlohmann::json& id,
                           const std::string& method,
                           const nlohmann::json& params)
{
  if (method == "initialize")
  {
    if (m_initialized)
      send(errorResponse(
          id, errors::kInvalidRequest, "initialize was already sent"));
    else
      send(response(id, initialize(params)));
    return;
  }
  if (!m_initialized)
  {
    send(errorResponse(
        id, errors::kServerNotInitialized, "the server isn't initialized"));
    return;
  }
  if (m_shutdown)
  {
    send(errorResponse(id, errors::kInvalidRequest, "the server is shut down"));
    return;
  }
  if (method == "shutdown")
  {
    m_shutdown = true;
    send(response(id, nullptr));
    return;
  }
  if (method == "textDocument/completion" || method == "textDocument/hover" ||
      method == "textDocument/codeAction")
  {
    Document* document = find(at(params, {"textDocument"}));
    if (!document)
    {
      send(response(id, nullptr));
      return;
    }
    if (method == "textDocument/codeAction")
    {
      if (document->dirty)
        analyseAndPublish(*document);
      send(response(id,
                    codeActions(*document,
                                positionFrom(at(params, {"range", "start"})),
                                positionFrom(at(params, {"range", "end"})),
                                m_settings,
                                [this](const std::filesystem::path& path)
                                { return openDocument(path); })));
      return;
    }
    if (document->text.size() > kMaxAnalysedSize)
    {
      send(response(id, nullptr));
      return;
    }
    reparse(*document, m_parser.get());
    const Position position = positionFrom(at(params, {"position"}));
    if (method == "textDocument/hover")
      send(response(id, hover(*document, position, m_settings)));
    else
    {
      const nlohmann::json& trigger = at(params, {"context", "triggerKind"});
      const bool invoked = !trigger.is_number() || trigger.get<int>() == 1;
      send(response(id, completion(*document, position, invoked, m_settings)));
    }
    return;
  }
  send(errorResponse(
      id, errors::kMethodNotFound, "sfs-lsp doesn't handle " + method));
}

void Server::handleNotification(const std::string& method,
                                const nlohmann::json& params)
{
  if (method == "exit")
  {
    m_exitCode = m_shutdown ? 0 : 1;
    return;
  }
  if (!m_initialized)
    return;
  if (method == "initialized")
    m_index.start(m_root);
  else if (method == "textDocument/didOpen")
    open(params);
  else if (method == "textDocument/didChange")
    change(params);
  else if (method == "textDocument/didSave")
    save(params);
  else if (method == "textDocument/didClose")
    close(params);
}

nlohmann::json Server::initialize(const nlohmann::json& params)
{
  m_settings.encoding = PositionEncoding::Utf16;
  const nlohmann::json& encodings =
      at(params, {"capabilities", "general", "positionEncodings"});
  if (encodings.is_array())
    for (const nlohmann::json& encoding : encodings)
    {
      if (!encoding.is_string())
        continue;
      const std::string name = encoding;
      if (name == "utf-8" || name == "utf-16" || name == "utf-32")
      {
        m_settings.encoding = name == "utf-8"    ? PositionEncoding::Utf8
                              : name == "utf-16" ? PositionEncoding::Utf16
                                                 : PositionEncoding::Utf32;
        break;
      }
    }
  m_settings.snippets = at(params,
                           {"capabilities",
                            "textDocument",
                            "completion",
                            "completionItem",
                            "snippetSupport"}) == true;
  m_settings.documentChanges =
      at(params,
         {"capabilities", "workspace", "workspaceEdit", "documentChanges"}) ==
      true;
  m_settings.diagnosticTags =
      at(params,
         {"capabilities", "textDocument", "publishDiagnostics", "tagSupport"})
          .is_object();

  const nlohmann::json& rootUri = at(params, {"rootUri"});
  const nlohmann::json& folders = at(params, {"workspaceFolders"});
  const nlohmann::json& rootPath = at(params, {"rootPath"});
  if (rootUri.is_string())
    m_root = uriToPath(rootUri.get<std::string>());
  else if (folders.is_array() && !folders.empty() &&
           at(folders[0], {"uri"}).is_string())
    m_root = uriToPath(at(folders[0], {"uri"}).get<std::string>());
  else if (rootPath.is_string())
    m_root = rootPath.get<std::string>();

  const nlohmann::json& disabled =
      at(params, {"initializationOptions", "disabledRules"});
  if (disabled.is_array())
    for (const nlohmann::json& rule : disabled)
      if (rule.is_string())
        if (const std::string problem =
                disableRule(rule.get<std::string>(), m_settings.disabledRules);
            !problem.empty())
          log(problem);

  m_initialized = true;
  return {
      {"capabilities",
       {{"positionEncoding", encodingName(m_settings.encoding)},
        {"textDocumentSync",
         {{"openClose", true},
          {"change", 2},
          {"save", {{"includeText", false}}}}},
        {"completionProvider",
         {{"triggerCharacters", {":", "[", "(", "!", ","}},
          {"resolveProvider", false}}},
        {"hoverProvider", true},
        {"codeActionProvider", {{"codeActionKinds", {"quickfix"}}}}}},
      {"serverInfo", {{"name", "sfs-lsp"}, {"version", SFS_ENGINE_VERSION}}}};
}

const Document* Server::openDocument(const std::filesystem::path& path) const
{
  const std::filesystem::path normal = path.lexically_normal();
  for (const std::unique_ptr<Document>& document : m_documents)
    if (document->path == normal)
      return document.get();
  return nullptr;
}

Document* Server::find(const nlohmann::json& textDocument)
{
  const nlohmann::json& uri = at(textDocument, {"uri"});
  if (!uri.is_string())
    return nullptr;
  const std::string& name = uri.get_ref<const std::string&>();
  for (const std::unique_ptr<Document>& document : m_documents)
    if (document->uri == name)
      return document.get();
  return nullptr;
}

void Server::open(const nlohmann::json& params)
{
  const nlohmann::json& item = at(params, {"textDocument"});
  const nlohmann::json& uri = at(item, {"uri"});
  const nlohmann::json& text = at(item, {"text"});
  if (!uri.is_string() || !text.is_string())
    return;
  Document* document = find(item);
  if (!document)
  {
    m_documents.push_back(std::make_unique<Document>());
    document = m_documents.back().get();
  }
  document->uri = uri.get<std::string>();
  document->path = uriToPath(document->uri).lexically_normal();
  document->version = at(item, {"version"}).is_number_integer()
                          ? at(item, {"version"}).get<int>()
                          : 0;
  setText(*document, text.get<std::string>());
  if (document->text.size() > kMaxAnalysedSize)
    log(document->path.string() + " is over 2 MB: it isn't checked");
  codegen::ParseResult parsed;
  analyseAndPublish(*document, &parsed);
  if (document->role == FileRole::CodegenHeader)
    m_index.update(
        document->path, parsed, document->tree.get(), document->text);
}

void Server::change(const nlohmann::json& params)
{
  Document* document = find(at(params, {"textDocument"}));
  const nlohmann::json& changes = at(params, {"contentChanges"});
  if (!document || !changes.is_array())
    return;
  const Clock::time_point start = Clock::now();
  const nlohmann::json& version = at(params, {"textDocument", "version"});
  if (version.is_number_integer())
    document->version = version.get<int>();
  for (const nlohmann::json& edit : changes)
  {
    const nlohmann::json& text = at(edit, {"text"});
    if (!text.is_string())
      continue;
    const nlohmann::json& range = at(edit, {"range"});
    if (range.is_object())
      applyEdit(*document,
                positionFrom(at(range, {"start"})),
                positionFrom(at(range, {"end"})),
                text.get_ref<const std::string&>(),
                m_settings.encoding);
    else
      setText(*document, text.get<std::string>());
  }
  if (m_logTiming)
    log("change " + std::to_string(millisecondsSince(start)) + " ms (" +
        document->path.filename().string() + ")");
}

void Server::save(const nlohmann::json& params)
{
  Document* document = find(at(params, {"textDocument"}));
  // A saved script header's types go into the index from its analysis.
  if (document && document->text.size() <= kMaxAnalysedSize &&
      m_index.isScriptHeader(document->path))
  {
    codegen::ParseResult parsed;
    analyseAndPublish(*document, &parsed);
    m_index.update(
        document->path, parsed, document->tree.get(), document->text);
  }
  m_index.rescan();
}

void Server::close(const nlohmann::json& params)
{
  Document* document = find(at(params, {"textDocument"}));
  if (!document)
    return;
  send(notification(
      "textDocument/publishDiagnostics",
      {{"uri", document->uri}, {"diagnostics", nlohmann::json::array()}}));
  std::erase_if(m_documents,
                [&](const std::unique_ptr<Document>& open)
                { return open.get() == document; });
}

void Server::analyseAndPublish(Document& document, codegen::ParseResult* parsed)
{
  const Clock::time_point start = Clock::now();
  if (document.text.size() <= kMaxAnalysedSize)
    reparse(document, m_parser.get());
  const double reparsed = millisecondsSince(start);
  analyse(document,
          m_parser.get(),
          m_linter,
          m_index,
          m_settings.disabledRules,
          parsed,
          &m_reader);
  if (m_logTiming)
    log("analysis " + std::to_string(millisecondsSince(start)) +
        " ms: reparse " + std::to_string(reparsed) + " ms (" +
        document.path.filename().string() + ")");
  publish(document);
}

void Server::publish(const Document& document)
{
  nlohmann::json diagnostics = nlohmann::json::array();
  for (const codegen::Diagnostic& diagnostic : document.diagnostics)
    diagnostics.push_back(diagnosticJson(document, diagnostic, m_settings));
  send(notification("textDocument/publishDiagnostics",
                    {{"uri", document.uri},
                     {"version", document.version},
                     {"diagnostics", std::move(diagnostics)}}));
}

std::optional<Clock::time_point> Server::nextSettle() const
{
  std::optional<Clock::time_point> next;
  for (const std::unique_ptr<Document>& document : m_documents)
    if (document->dirty && (!next || document->lastChange + kSettle < *next))
      next = document->lastChange + kSettle;
  return next;
}

void Server::settle()
{
  const Clock::time_point now = Clock::now();
  for (const std::unique_ptr<Document>& document : m_documents)
    if (document->dirty && document->lastChange + kSettle <= now)
      analyseAndPublish(*document);
}

void Server::indexIdle()
{
  if (!m_index.pending())
    return;
  m_index.indexSlice(m_parser.get(), 8);
  if (m_index.pending())
    return;
  // The index is complete: what the open files' lints said without it may
  // change.
  const Clock::time_point due = Clock::now() - kSettle;
  for (const std::unique_ptr<Document>& document : m_documents)
  {
    document->dirty = true;
    document->lastChange = due;
  }
}

} // namespace sfs::lsp
