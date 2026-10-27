#include "build/buildEnvironment.h"

#include "editorBuildConfig.h"

#include <algorithm>
#include <cctype>

namespace
{
// UNC prefixes Windows uses for a WSL distro's files.
const std::vector<std::string> kWslRoots = {"\\\\wsl.localhost\\", "\\\\wsl$\\"};

bool startsWithInsensitive(std::string_view text, std::string_view prefix)
{
  return text.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b)
                    { return std::tolower(static_cast<unsigned char>(a)) ==
                             std::tolower(static_cast<unsigned char>(b)); });
}
} // namespace

BuildEnvironment BuildEnvironment::current()
{
  BuildEnvironment env;
  env.m_runner = std::string_view(SFS_EDITOR_BUILD_RUNNER) == "wsl" ? Runner::Wsl : Runner::Native;
  env.m_wslDistro = SFS_EDITOR_WSL_DISTRO;
  env.m_toolchainFile = SFS_EDITOR_TOOLCHAIN_FILE;
  env.m_enginePrefix = SFS_EDITOR_ENGINE_PREFIX;
  env.m_codegen = SFS_EDITOR_CODEGEN;
  env.m_generator = SFS_EDITOR_GENERATOR;
  env.m_buildType = SFS_EDITOR_BUILD_TYPE;
  env.m_moduleSuffix = SFS_EDITOR_MODULE_SUFFIX;
  return env;
}

std::string BuildEnvironment::toBuildPath(const std::filesystem::path& editorPath) const
{
  const std::string path = editorPath.string();
  if (m_runner != Runner::Wsl)
    return path;

  // \\wsl.localhost\<distro>\home\me -> /home/me
  for (const std::string& root : kWslRoots)
  {
    if (!startsWithInsensitive(path, root))
      continue;
    const std::size_t distroEnd = path.find('\\', root.size());
    std::string posixPath = distroEnd == std::string::npos ? "/" : path.substr(distroEnd);
    std::replace(posixPath.begin(), posixPath.end(), '\\', '/');
    return posixPath;
  }

  // C:\Users\me -> /mnt/c/Users/me
  if (path.size() >= 2 && path[1] == ':')
  {
    std::string posixPath = "/mnt/";
    posixPath += static_cast<char>(std::tolower(static_cast<unsigned char>(path[0])));
    posixPath += path.substr(2);
    std::replace(posixPath.begin(), posixPath.end(), '\\', '/');
    return posixPath;
  }
  return path;
}

std::filesystem::path BuildEnvironment::toEditorPath(const std::string& buildPath) const
{
  if (m_runner != Runner::Wsl)
    return buildPath;

  if (buildPath.starts_with("/mnt/") && buildPath.size() >= 6)
  {
    std::string windows(1, static_cast<char>(std::toupper(static_cast<unsigned char>(buildPath[5]))));
    windows += ":" + buildPath.substr(6);
    std::replace(windows.begin(), windows.end(), '/', '\\');
    return windows;
  }

  std::string windows = "\\\\wsl.localhost\\" + m_wslDistro + buildPath;
  std::replace(windows.begin(), windows.end(), '/', '\\');
  return windows;
}

std::vector<std::string> BuildEnvironment::command(std::vector<std::string> args) const
{
  if (m_runner != Runner::Wsl)
    return args;

  // A login shell, so the user's PATH (cmake, conan, compilers) applies.
  std::string script;
  for (const std::string& arg : args)
  {
    std::string quoted = "'";
    for (const char c : arg)
      quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    script += (script.empty() ? "" : " ") + quoted + "'";
  }
  return {"wsl.exe", "-d", m_wslDistro, "--", "bash", "-lc", script};
}
