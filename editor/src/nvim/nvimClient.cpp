#include "nvim/nvimClient.h"

#include "build/buildEnvironment.h"

#include "engine/core/logger/logger.h"

#include <chrono>
#include <cstdlib>
#include <thread>

using msgpack::Array;
using msgpack::Map;
using msgpack::Value;

namespace
{
// MessagePack-RPC message types.
constexpr int64_t kRequest = 0;
constexpr int64_t kResponse = 1;
constexpr int64_t kNotification = 2;

// How long a quitting Neovim gets before it's killed.
constexpr auto kQuitTimeout = std::chrono::seconds(2);

// A string as a Vim script single-quoted literal.
std::string vimString(const std::string& text)
{
  std::string out = "'";
  for (const char c : text)
    out += c == '\'' ? std::string("''") : std::string(1, c);
  return out + "'";
}

Value str(std::string text) { return Value(std::move(text)); }
} // namespace

NvimClient::NvimClient(const BuildEnvironment& environment) : m_environment(environment) {}

NvimClient::~NvimClient()
{
  // Before the members its reader threads use.
  stop();
}

void NvimClient::stop()
{
  if (!m_process)
    return;

  // Quit properly, saving modified files first: killed, Neovim keeps its swap
  // files for crash recovery and asks about them on every later open.
  if (m_process->running())
  {
    request("nvim_command", {str("silent! wall | qall!")});
    const auto deadline = std::chrono::steady_clock::now() + kQuitTimeout;
    while (m_process->running() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  m_process.reset(); // kills it if it's still running, and joins the readers
}

bool NvimClient::start(const LaunchOptions& options, int columns, int rows)
{
  stop();
  m_screen = NvimScreen();
  {
    const std::lock_guard lock(m_incomingMutex);
    m_incomingBytes.clear();
    m_incoming.clear();
  }

  const char* mode = options.mode == ConfigMode::Engine ? "engine"
                     : options.mode == ConfigMode::User ? "user"
                                                        : "auto";
  // Settings for the engine's init.lua, set before any config runs.
  const std::string globals =
      "let g:sfs_editor = 1 | let g:sfs_nvim_mode = " + vimString(mode) +
      " | let g:sfs_project_root = " +
      vimString(m_environment.toBuildPath(options.workingDirectory));

  std::vector<std::string> args;
  // The engine config gets its own config/data folders, so the user's plugins
  // (~/.config/nvim/plugin, lazy.nvim's data) never leak into it.
  if (options.mode == ConfigMode::Engine)
    args = {"env", "NVIM_APPNAME=sfs-editor"};
  args.insert(args.end(), {"nvim", "--embed", "--cmd", globals});
  if (options.mode != ConfigMode::User)
    args.insert(args.end(), {"-u", m_environment.toBuildPath(options.engineConfig / "init.lua")});

  m_process = std::make_unique<ChildProcess>();
  ChildProcess::Options processOptions;
  processOptions.pipeStdin = true;
  processOptions.onStderr = [](std::string_view chunk)
  {
    // Neovim itself is quiet on stderr; this is the launcher (wsl.exe, a
    // missing nvim) explaining a failure.
    std::string text(chunk);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
      text.pop_back();
    if (!text.empty())
      LOG_ERROR("nvim: " + text);
  };
  if (!m_process->start(m_environment.command(args),
                        [this](std::string_view chunk) { receive(chunk); },
                        std::move(processOptions)))
  {
    m_process.reset();
    return false;
  }

  m_columns = std::max(columns, 1);
  m_rows = std::max(rows, 1);
  Map uiOptions;
  uiOptions.emplace_back(str("rgb"), Value(true));
  uiOptions.emplace_back(str("ext_linegrid"), Value(true));
  request("nvim_ui_attach", {Value(m_columns), Value(m_rows), Value(uiOptions)});
  return true;
}

bool NvimClient::running()
{
  return m_process && m_process->running();
}

void NvimClient::receive(std::string_view chunk)
{
  const std::lock_guard lock(m_incomingMutex);
  m_incomingBytes += chunk;

  std::string_view pending = m_incomingBytes;
  for (;;)
  {
    Value message;
    std::size_t consumed = 0;
    const msgpack::DecodeStatus status = msgpack::decode(pending, consumed, message);
    if (status == msgpack::DecodeStatus::NeedMore)
      break;
    if (status == msgpack::DecodeStatus::Error)
    {
      // Something other than RPC on stdout (e.g. a shell profile echoing):
      // nothing after it can be trusted.
      LOG_ERROR("nvim: unexpected output; is something in the shell profile printing?");
      pending = {};
      break;
    }
    m_incoming.push_back(std::move(message));
    pending.remove_prefix(consumed);
  }
  m_incomingBytes.erase(0, m_incomingBytes.size() - pending.size());
}

void NvimClient::poll()
{
  std::deque<Value> messages;
  {
    const std::lock_guard lock(m_incomingMutex);
    messages.swap(m_incoming);
  }
  for (const Value& message : messages)
    handle(message);
}

void NvimClient::handle(const Value& message)
{
  const Array& parts = message.asArray();
  if (parts.size() < 3)
    return;

  switch (parts[0].asInt(-1))
  {
  case kNotification:
    if (parts[1].asString() == "redraw")
      m_screen.applyRedraw(parts[2].asArray());
    break;
  case kResponse: // [1, id, error, result]
    if (parts.size() >= 3 && !parts[2].isNil())
    {
      const Array& error = parts[2].asArray();
      LOG_ERROR("nvim: " + (error.size() >= 2 ? error[1].asString() : std::string("request failed")));
    }
    break;
  case kRequest: // [0, id, method, params]: we serve no methods
  {
    msgpack::Writer writer;
    writer.arrayHeader(4)
        .integer(kResponse)
        .integer(parts[1].asInt())
        .string("SfsEditor serves no RPC methods")
        .nil();
    m_process->write(writer.data());
    break;
  }
  default:
    break;
  }
}

void NvimClient::request(const std::string& method, const Array& params)
{
  if (!m_process)
    return;
  msgpack::Writer writer;
  writer.arrayHeader(4).integer(kRequest).integer(m_nextRequestId++).string(method);
  writer.value(Value(params));
  m_process->write(writer.data());
}

void NvimClient::resize(int columns, int rows)
{
  columns = std::max(columns, 1);
  rows = std::max(rows, 1);
  if (columns == m_columns && rows == m_rows)
    return;
  m_columns = columns;
  m_rows = rows;
  request("nvim_ui_try_resize", {Value(columns), Value(rows)});
}

void NvimClient::input(const std::string& keys)
{
  if (!keys.empty())
    request("nvim_input", {str(keys)});
}

void NvimClient::mouse(const char* button,
                       const char* action,
                       const std::string& modifiers,
                       int row,
                       int column)
{
  request("nvim_input_mouse",
          {str(button), str(action), str(modifiers), Value(0), Value(row), Value(column)});
}

void NvimClient::open(const std::filesystem::path& file)
{
  // As typed keys rather than a direct call, so Neovim can still ask what it
  // needs to (e.g. the swap file "ATTENTION" prompt); <C-\><C-n> leaves
  // whatever mode it is in first.
  request("nvim_exec_lua",
          {str("local keys = '<C-\\\\><C-n>:edit ' .. vim.fn.fnameescape(...):gsub('<', '<lt>') .. '<CR>'\n"
               "vim.api.nvim_input(keys)"),
           Value(Array{str(m_environment.toBuildPath(file))})});
}

void NvimClient::setProject(const std::filesystem::path& root)
{
  if (root.empty())
    return;
  request("nvim_set_var", {str("sfs_project_root"), str(m_environment.toBuildPath(root))});
  request("nvim_set_current_dir", {str(m_environment.toBuildPath(root))});
}
