#include "../testHarness.h"

#include <sfs/lsp/protocol.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace sfs::lsp;
using nlohmann::json;

namespace
{

// sfs-lsp run as an editor runs it: stdin and stdout piped.
class LspProcess
{
public:
  LspProcess()
  {
    int toChild[2];
    int fromChild[2];
    if (pipe(toChild) != 0 || pipe(fromChild) != 0)
      return;
    m_pid = fork();
    if (m_pid == 0)
    {
      dup2(toChild[0], 0);
      dup2(fromChild[1], 1);
      close(toChild[1]);
      close(fromChild[0]);
      execl(SFS_LSP_PATH, SFS_LSP_PATH, static_cast<char*>(nullptr));
      _exit(127);
    }
    close(toChild[0]);
    close(fromChild[1]);
    m_in = toChild[1];
    m_out = fromChild[0];
  }

  ~LspProcess()
  {
    closeInput();
    if (m_out >= 0)
      close(m_out);
    if (m_pid > 0 && !m_exited)
    {
      kill(m_pid, SIGKILL);
      waitpid(m_pid, nullptr, 0);
    }
  }

  void write(std::string_view bytes)
  {
    while (!bytes.empty())
    {
      const ssize_t written = ::write(m_in, bytes.data(), bytes.size());
      if (written <= 0)
        return;
      bytes.remove_prefix(static_cast<std::size_t>(written));
    }
  }

  void send(const json& message) { write(frame(dump(message))); }

  void closeInput()
  {
    if (m_in >= 0)
      close(m_in);
    m_in = -1;
  }

  // The first message `wanted` accepts, read within `seconds`; earlier ones are
  // dropped.
  std::optional<json> waitFor(const std::function<bool(const json&)>& wanted,
                              double seconds = 2.0)
  {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(seconds);
    while (true)
    {
      while (!m_pending.empty())
      {
        json message = json::parse(m_pending.front());
        m_pending.erase(m_pending.begin());
        if (wanted(message))
          return message;
      }
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0)
        return std::nullopt;
      pollfd fd{m_out, POLLIN, 0};
      if (poll(&fd, 1, static_cast<int>(left.count())) <= 0)
        continue;
      char buffer[65536];
      const ssize_t count = read(m_out, buffer, sizeof buffer);
      if (count <= 0)
        return std::nullopt;
      std::vector<std::string> problems;
      m_reader.feed(std::string_view(buffer, static_cast<std::size_t>(count)),
                    m_pending,
                    problems);
    }
  }

  std::optional<json> response(int id)
  {
    return waitFor(
        [id](const json& m)
        { return m.contains("id") && m["id"] == id && !m.contains("method"); });
  }

  std::optional<json> diagnostics(const std::string& uri)
  {
    return waitFor(
        [&](const json& m)
        {
          return m.value("method", "") == "textDocument/publishDiagnostics" &&
                 m["params"]["uri"] == uri;
        });
  }

  // The exit code, if it exits within `seconds`.
  std::optional<int> exitCode(double seconds)
  {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
      int status = 0;
      if (waitpid(m_pid, &status, WNOHANG) == m_pid)
      {
        m_exited = true;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      }
      usleep(10000);
    }
    return std::nullopt;
  }

  void initialize(
      json capabilities = {
          {"general", {{"positionEncodings", {"utf-8", "utf-16"}}}},
          {"textDocument",
           {{"completion", {{"completionItem", {{"snippetSupport", true}}}}}}},
          {"workspace", {{"workspaceEdit", {{"documentChanges", true}}}}}})
  {
    send(request(
        1,
        "initialize",
        {{"rootUri", nullptr}, {"capabilities", std::move(capabilities)}}));
    m_initializeResult = response(1);
    send(notification("initialized", json::object()));
  }

  std::optional<json> m_initializeResult;

private:
  pid_t m_pid = -1;
  bool m_exited = false;
  int m_in = -1;
  int m_out = -1;
  MessageReader m_reader;
  std::vector<std::string> m_pending;
};

json position(int line, int character)
{
  return {{"line", line}, {"character", character}};
}

} // namespace

int main()
{
  // A broken pipe to a server that exited must not end the test program.
  std::signal(SIGPIPE, SIG_IGN);

  TEST("initialize should answer the capabilities and pick the client's first "
       "encoding")
  {
    LspProcess server;
    // A header and a body split across writes, then two messages in one write.
    const std::string init = frame(
        dump(request(1,
                     "initialize",
                     {{"capabilities",
                       {{"general", {{"positionEncodings", {"utf-8"}}}}}}})));
    const std::size_t split = init.find("\r\n\r\n") + 2;
    server.write(init.substr(0, split));
    server.write(init.substr(split));
    const std::optional<json> result = server.response(1);
    CHECK(result.has_value());
    if (result)
    {
      const json& capabilities = (*result)["result"]["capabilities"];
      CHECK(capabilities["positionEncoding"] == "utf-8");
      CHECK(capabilities["textDocumentSync"]["change"] == 2);
      CHECK(capabilities["hoverProvider"] == true);
      CHECK(capabilities["codeActionProvider"]["codeActionKinds"][0] ==
            "quickfix");
      CHECK((*result)["result"]["serverInfo"]["name"] == "sfs-lsp");
    }
    server.write(frame(dump(request(2, "nope/one", json::object()))) +
                 frame(dump(request(3, "nope/two", json::object()))));
    const std::optional<json> unknown = server.response(3);
    CHECK(unknown && (*unknown)["error"]["code"] == errors::kMethodNotFound);

    LspProcess utf16;
    utf16.initialize({{"general", {{"positionEncodings", {"utf-16"}}}}});
    CHECK(utf16.m_initializeResult &&
          (*utf16.m_initializeResult)["result"]["capabilities"]
                                     ["positionEncoding"] == "utf-16");
  }

  TEST("a request before initialize should get ServerNotInitialized")
  {
    LspProcess server;
    server.send(request(5, "textDocument/hover", json::object()));
    const std::optional<json> answer = server.response(5);
    CHECK(answer &&
          (*answer)["error"]["code"] == errors::kServerNotInitialized);
  }

  TEST("opening a header should publish its diagnostics, and completion should "
       "offer attributes")
  {
    LspProcess server;
    server.initialize();
    const std::string uri = "file:///tmp/sfsLspTest/Lamp.h";
    server.send(notification("textDocument/didOpen",
                             {{"textDocument",
                               {{"uri", uri},
                                {"languageId", "cpp"},
                                {"version", 1},
                                {"text",
                                 "struct Lamp\n{\n  [[sfs::ra]] float level = "
                                 "0;\n  [[sfs::hdie]] int x;\n};\n"}}}}));
    const std::optional<json> published = server.diagnostics(uri);
    CHECK(published.has_value());
    if (published)
    {
      const json& list = (*published)["params"]["diagnostics"];
      CHECK(list.size() == 2u);
      CHECK(list.size() == 2u && list[1]["code"] == "unknown-sfs-attribute");
      CHECK(list.size() == 2u && list[1]["source"] == "sfs-codegen");
    }
    server.send(request(7,
                        "textDocument/completion",
                        {{"textDocument", {{"uri", uri}}},
                         {"position", position(2, 11)},
                         {"context", {{"triggerKind", 1}}}}));
    const std::optional<json> answer = server.response(7);
    bool found = false;
    if (answer)
      for (const json& item : (*answer)["result"]["items"])
        if (item["label"] == "range")
        {
          found = true;
          CHECK(item["textEdit"]["newText"] == "range(${1:min}, ${2:max})");
          CHECK(item["insertTextFormat"] == 2);
        }
    CHECK(found);
  }

  TEST("a lint's fix and ignore actions should work end to end")
  {
    LspProcess server;
    server.initialize();
    const std::string uri = "file:///tmp/sfsLspTest/Mover.cpp";
    server.send(notification(
        "textDocument/didOpen",
        {{"textDocument",
          {{"uri", uri},
           {"languageId", "cpp"},
           {"version", 1},
           {"text",
            "void f()\n{\n  scene().query<A, sfs::Any<B>>();\n}\n"}}}}));
    const std::optional<json> published = server.diagnostics(uri);
    CHECK(published && (*published)["params"]["diagnostics"].size() == 1u);
    if (!published || (*published)["params"]["diagnostics"].size() != 1u)
      continue;
    const json diagnostic = (*published)["params"]["diagnostics"][0];
    CHECK(diagnostic["code"] == "single-any");
    CHECK(diagnostic["source"] == "sfs");

    server.send(request(9,
                        "textDocument/codeAction",
                        {{"textDocument", {{"uri", uri}}},
                         {"range", diagnostic["range"]},
                         {"context", {{"diagnostics", {diagnostic}}}}}));
    const std::optional<json> actions = server.response(9);
    CHECK(actions && (*actions)["result"].size() == 3u);
    if (!actions || (*actions)["result"].size() != 3u)
      continue;
    const json& fix = (*actions)["result"][0];
    CHECK(fix["isPreferred"] == true);
    CHECK((*actions)["result"][1]["title"] == "Ignore single-any on this line");
    CHECK((*actions)["result"][2]["title"] == "Ignore single-any in this file");
    const json& edit = fix["edit"]["documentChanges"][0]["edits"][0];
    CHECK(edit["newText"] == "B");

    server.send(notification(
        "textDocument/didChange",
        {{"textDocument", {{"uri", uri}, {"version", 2}}},
         {"contentChanges",
          {{{"range", edit["range"]}, {"text", edit["newText"]}}}}}));
    const std::optional<json> after = server.diagnostics(uri);
    CHECK(after && (*after)["params"]["diagnostics"].empty());
    CHECK(after && (*after)["params"]["version"] == 2);
  }

  TEST("a fix that spans two files should be one workspace edit of both")
  {
    // The header isn't open: it's read from disk and has no version.
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / "sfsLspTwoFiles";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const std::string headerText =
        "#pragma once\nclass GateSystem : public sfs::System\n{\npublic:\n"
        "  GateSystem();\n\nprotected:\n"
        "  void update(double deltaTime) override;\n};\n";
    std::ofstream(folder / "GateSystem.h", std::ios::binary) << headerText;
    LspProcess server;
    server.initialize();
    const std::string uri = pathToUri(folder / "GateSystem.cpp");
    const std::string headerUri = pathToUri(folder / "GateSystem.h");
    server.send(notification(
        "textDocument/didOpen",
        {{"textDocument",
          {{"uri", uri},
           {"languageId", "cpp"},
           {"version", 4},
           {"text",
            "#include \"GateSystem.h\"\n\nGateSystem::GateSystem()\n{\n"
            "  registerComponent<GateLeaf>();\n}\n"}}}}));
    const std::optional<json> published = server.diagnostics(uri);
    CHECK(published && (*published)["params"]["diagnostics"].size() == 1u);
    if (!published || (*published)["params"]["diagnostics"].size() != 1u)
      continue;
    const json diagnostic = (*published)["params"]["diagnostics"][0];
    CHECK(diagnostic["code"] == "setup-in-constructor");
    const auto fixOf = [&](int id) -> json
    {
      server.send(request(id,
                          "textDocument/codeAction",
                          {{"textDocument", {{"uri", uri}}},
                           {"range", diagnostic["range"]},
                           {"context", {{"diagnostics", {diagnostic}}}}}));
      const std::optional<json> actions = server.response(id);
      return actions && !(*actions)["result"].empty() ? (*actions)["result"][0]
                                                      : json();
    };
    const json fix = fixOf(12);
    CHECK(fix.is_object() && fix["isPreferred"] == true);
    if (!fix.is_object())
      continue;
    const json& changes = fix["edit"]["documentChanges"];
    CHECK(changes.size() == 2u);
    CHECK(changes.size() == 2u && changes[0]["textDocument"]["uri"] == uri &&
          changes[0]["textDocument"]["version"] == 4);
    CHECK(changes.size() == 2u &&
          changes[1]["textDocument"]["uri"] == headerUri &&
          changes[1]["textDocument"]["version"].is_null() &&
          !changes[1]["edits"].empty());

    // Open, the header's own version is checked.
    server.send(notification("textDocument/didOpen",
                             {{"textDocument",
                               {{"uri", headerUri},
                                {"languageId", "cpp"},
                                {"version", 7},
                                {"text", headerText}}}}));
    server.diagnostics(headerUri);
    const json again = fixOf(13);
    CHECK(again.is_object() && again["edit"]["documentChanges"].size() == 2u &&
          again["edit"]["documentChanges"][1]["textDocument"]["version"] == 7);
    std::filesystem::remove_all(folder);
  }

  TEST("malformed JSON should get ParseError and the server should carry on")
  {
    LspProcess server;
    server.initialize();
    server.write(frame("{not json"));
    const std::optional<json> error = server.waitFor(
        [](const json& m) { return m.contains("error") && m["id"].is_null(); });
    CHECK(error && (*error)["error"]["code"] == errors::kParseError);
    server.send(request(11, "textDocument/unknownThing", json::object()));
    const std::optional<json> unknown = server.response(11);
    CHECK(unknown && (*unknown)["error"]["code"] == errors::kMethodNotFound);
  }

  TEST("shutdown then exit should exit with 0; the end of input without exit "
       "with 1")
  {
    LspProcess server;
    server.initialize();
    server.send(request(20, "shutdown", nullptr));
    const std::optional<json> answer = server.response(20);
    CHECK(answer && answer->contains("result") &&
          (*answer)["result"].is_null());
    server.send(notification("exit", nullptr));
    CHECK(server.exitCode(2.0) == std::optional<int>(0));

    LspProcess dropped;
    dropped.initialize();
    dropped.closeInput();
    CHECK(dropped.exitCode(1.0) == std::optional<int>(1));
  }

  return testing::report("lspProtocolTests");
}
