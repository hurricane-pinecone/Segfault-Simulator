#include "build/moduleBuilder.h"

#include "project/project.h"

#include "engine/core/logger/logger.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

namespace
{
constexpr int kJobs = 3; // compiles at once


// Compiler and CMake errors, worth highlighting among the build output.
bool looksLikeError(const std::string& line)
{
  return line.find("error:") != std::string::npos ||
         line.find("Error ") != std::string::npos ||
         line.find("CMake Error") != std::string::npos ||
         line.find("undefined reference") != std::string::npos;
}
} // namespace

std::filesystem::path ModuleBuilder::buildDirectory(const Project& project) const
{
  // Per platform, so a Linux and a Windows editor can share a project.
  return project.buildFolder() / m_environment.platformName();
}

std::filesystem::path ModuleBuilder::modulePath(const Project& project) const
{
  return buildDirectory(project) / "module" /
         (project.moduleName() + m_environment.moduleSuffix());
}

bool ModuleBuilder::start(const Project& project)
{
  if (busy())
  {
    LOG_ERROR("A build is already running");
    return false;
  }
  if (!project.ensureBuildFiles())
    return false;

  const std::string source = m_environment.toBuildPath(project.root());
  const std::filesystem::path buildDir = buildDirectory(project);
  const std::string build = m_environment.toBuildPath(buildDir);

  m_steps.clear();
  // Configure once; afterwards `cmake --build` re-runs configuration itself
  // when it needs to (new script files, a changed CMakeLists.txt).
  if (!std::filesystem::exists(buildDir / "CMakeCache.txt"))
    m_steps.push_back({"cmake",
                       "-S", source,
                       "-B", build,
                       "-G", m_environment.generator(),
                       "-DCMAKE_BUILD_TYPE=" + m_environment.buildType(),
                       "-DCMAKE_TOOLCHAIN_FILE=" + m_environment.toolchainFile(),
                       "-DCMAKE_PREFIX_PATH=" + m_environment.enginePrefix(),
                       "-DSFS_CODEGEN_EXECUTABLE=" + m_environment.codegen()});
  // A few compiles at a time: more can swamp the machine (WSL, especially)
  // for a module this size, and keep the editor from being usable meanwhile.
  m_steps.push_back({"cmake", "--build", build, "--parallel", std::to_string(kJobs)});

  m_percent = -1;
  LOG_INFO("Building " + project.moduleName() + "...");
  return startNextStep();
}

bool ModuleBuilder::startNextStep()
{
  // Build output is text: log it line by line, errors highlighted.
  auto lines = std::make_shared<LineSplitter>(
      [this](const std::string& line)
      {
        // Where the build runs, so cancel() can stop it.
        if (line.starts_with(BuildEnvironment::kGroupMarker))
        {
          m_group = std::atoi(line.c_str() + BuildEnvironment::kGroupMarker.size());
          return;
        }
        // CMake's generators start lines with "[ 42%]" as they go.
        if (const std::size_t open = line.find('['), close = line.find("%]");
            open != std::string::npos && close != std::string::npos && open < close)
        {
          int value = 0;
          bool digits = false;
          for (std::size_t i = open + 1; i < close; ++i)
            if (line[i] >= '0' && line[i] <= '9')
            {
              value = value * 10 + (line[i] - '0');
              digits = true;
            }
            else if (line[i] != ' ')
            {
              digits = false;
              break;
            }
          if (digits && value <= 100)
            m_percent = value;
        }
        if (looksLikeError(line))
          LOG_ERROR(line);
        else if (!line.empty())
          LOG_INFO(line);
      });

  m_group = -1;
  auto process = std::make_unique<ChildProcess>();
  const bool started = process->start(m_environment.stoppableCommand(m_steps.front()),
                                      [lines](std::string_view chunk) { lines->feed(chunk); });
  m_steps.erase(m_steps.begin());

  if (!started)
  {
    m_steps.clear();
    m_process.reset();
    return false;
  }
  m_process = std::move(process);
  return true;
}

void ModuleBuilder::cancel()
{
  if (!m_process)
    return;
  m_steps.clear();
  // On the build machine first (it runs on without its local process), then
  // the local process.
  if (const std::vector<std::string> stop = m_environment.stopCommand(m_group); !stop.empty())
  {
    ChildProcess stopper;
    if (stopper.start(stop, [](std::string_view) {}))
      for (int i = 0; i < 300 && stopper.running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  m_process->terminate();
  m_process.reset();
  m_cancelled = true;
  LOG_INFO("Build stopped");
}

std::optional<bool> ModuleBuilder::poll()
{
  if (m_cancelled)
  {
    m_cancelled = false;
    return false;
  }
  if (!m_process || m_process->running())
    return std::nullopt;

  const int exitCode = m_process->exitCode();
  m_process.reset();

  if (exitCode != 0)
  {
    m_steps.clear();
    LOG_ERROR("Build failed (exit code " + std::to_string(exitCode) + ")");
    return false;
  }
  if (!m_steps.empty())
    return startNextStep() ? std::nullopt : std::optional<bool>(false);

  LOG_INFO("Build succeeded");
  return true;
}
