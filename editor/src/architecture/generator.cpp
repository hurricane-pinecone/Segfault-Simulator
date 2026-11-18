#include "architecture/generator.h"

#include "architecture/graph.h"
#include "build/childProcess.h"

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace architecture
{

namespace
{

// sfs-lsp's stderr kept for an error message: its last this many bytes.
constexpr std::size_t MESSAGES_KEPT = 2000;

} // namespace

Generator::Generator() = default;

Generator::~Generator() { stop(); }

std::optional<Generated> Generator::poll()
{
  if (m_failed)
    return std::exchange(m_failed, std::nullopt);

  if (m_process && !m_process->running())
  {
    const int exitCode = m_process->exitCode();
    m_process.reset();
    m_output->errorLines.flush();
    std::string json;
    std::string messages;
    {
      const std::lock_guard lock(m_output->mutex);
      json = std::move(m_output->json);
      messages = std::move(m_output->messages);
    }

    // The shell's code for a program it couldn't run.
    if (exitCode == 127 && messages.empty())
      return Generated{
          std::nullopt, "Couldn't run " + m_program + ": is it built?"};

    if (exitCode != 0)
      return Generated{std::nullopt,
                       m_program + " failed (exit code " +
                           std::to_string(exitCode) + ")" +
                           (messages.empty() ? "" : ":\n" + messages)};

    // Reading a large export takes a while: off the UI thread.
    m_parse = std::async(std::launch::async,
                         [json = std::move(json)]
                         {
                           Generated result;
                           result.graph = parseGraph(json, result.error);
                           return result;
                         });
  }

  if (!m_parse.valid() ||
      m_parse.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return std::nullopt;

  return m_parse.get();
}

Progress Generator::progress() const
{
  if (!m_output)
    return {};

  const std::lock_guard lock(m_output->mutex);
  return m_output->progress;
}

bool Generator::start(const std::vector<std::string>& command)
{
  stop();
  m_program = command.empty() ? std::string("sfs-lsp") : command.front();
  m_output = std::make_shared<Output>();
  m_process = std::make_unique<ChildProcess>();
  const bool started =
      m_process->start(command,
                       [output = m_output](std::string_view chunk)
                       {
                         const std::lock_guard lock(output->mutex);
                         output->json += chunk;
                       },
                       {.onStderr = [output = m_output](std::string_view chunk)
                        { output->errorLines.feed(chunk); }});

  if (started)
    return true;

  m_process.reset();
  m_failed = Generated{std::nullopt, "Couldn't start " + m_program};
  return false;
}

void Generator::stop()
{
  if (m_process)
    m_process->terminate();

  m_process.reset();
  m_failed.reset();

  // A read under way finishes on its own; its result is dropped.
  if (m_parse.valid())
    m_parse.wait();

  m_parse = {};
}

Generator::Output::Output()
    : errorLines(
          [this](const std::string& line)
          {
            const std::optional<Progress> progress = parseProgress(line);
            const std::lock_guard lock(mutex);

            if (progress)
            {
              this->progress = *progress;
              return;
            }

            messages += line + "\n";

            if (messages.size() > MESSAGES_KEPT)
              messages.erase(0, messages.size() - MESSAGES_KEPT);
          })
{
}

} // namespace architecture
