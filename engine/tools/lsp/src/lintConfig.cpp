#include "lintConfig.h"

#include "lintSupport.h"

#include <sfs/codegen/parser.h>

#include <iostream>
#include <mutex>
#include <system_error>

namespace sfs::lsp
{

namespace
{

// A config file as last read, and when it was last changed then.
struct CachedConfig
{
  std::filesystem::path file;
  std::filesystem::file_time_type changed;
  std::uintmax_t size = 0;
  LintConfig config;
};

} // namespace

LintConfig parseLintConfig(std::string_view text,
                           const std::filesystem::path& file,
                           std::vector<std::string>& warnings)
{
  LintConfig config;
  std::size_t lineNumber = 0;
  const auto warn = [&](const std::string& problem)
  {
    warnings.push_back(file.string() + ':' + std::to_string(lineNumber) + ": " +
                       problem);
  };
  for (std::size_t start = 0; start < text.size();)
  {
    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos)
      end = text.size();
    std::string_view line = text.substr(start, end - start);
    start = end + 1;
    ++lineNumber;
    line = lint::trimmed(line.substr(0, line.find('#')));
    if (line.empty())
      continue;

    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos)
    {
      warn("expected 'key: value', got '" + std::string(line) + "'");
      continue;
    }
    const std::string_view key = lint::trimmed(line.substr(0, colon));
    const std::string_view values = line.substr(colon + 1);
    if (key != "include" && key != "disable")
    {
      warn("unknown key '" + std::string(key) + "'");
      continue;
    }

    for (std::size_t from = 0; from <= values.size();)
    {
      std::size_t comma = values.find(',', from);
      if (comma == std::string_view::npos)
        comma = values.size();
      const std::string_view value =
          lint::trimmed(values.substr(from, comma - from));
      from = comma + 1;
      if (value.empty())
        continue;

      if (key == "include")
      {
        if (value == "house")
          config.house = true;
        else
          warn("unknown rule set '" + std::string(value) + "'");
        continue;
      }
      const codegen::RuleInfo* rule = codegen::findRule(value);
      if (!rule)
        warn("unknown rule '" + std::string(value) + "'");
      else if (!codegen::canIgnore(rule->id))
        warn(std::string(value) + " can't be disabled");
      else
        config.disabled.set(static_cast<std::size_t>(rule->id));
    }
  }
  return config;
}

std::filesystem::path findLintConfig(const std::filesystem::path& file)
{
  std::error_code error;
  for (std::filesystem::path folder = file.parent_path(); !folder.empty();
       folder = folder.parent_path())
  {
    std::filesystem::path candidate = folder / ".sfs-lint";
    if (std::filesystem::is_regular_file(candidate, error))
      return candidate;
    if (folder == folder.parent_path())
      break;
  }
  return {};
}

LintConfig lintConfigFor(const std::filesystem::path& file)
{
  const std::filesystem::path found = findLintConfig(file);
  if (found.empty())
    return {};

  std::error_code error;
  const std::filesystem::file_time_type changed =
      std::filesystem::last_write_time(found, error);
  const std::uintmax_t size =
      error ? 0 : std::filesystem::file_size(found, error);

  // The server and the CLI may ask from more than one thread.
  static std::mutex mutex;
  static std::vector<CachedConfig> cache;
  const std::lock_guard lock(mutex);
  CachedConfig* entry = nullptr;
  for (CachedConfig& cached : cache)
    if (cached.file == found)
      entry = &cached;
  if (entry && !error && entry->changed == changed && entry->size == size)
    return entry->config;

  if (!entry)
  {
    cache.push_back({found, {}, 0, {}});
    entry = &cache.back();
  }
  std::vector<std::string> warnings;
  entry->config = parseLintConfig(codegen::readFile(found), found, warnings);
  entry->changed = changed;
  entry->size = size;
  for (const std::string& warning : warnings)
    std::cerr << "sfs-lsp: " << warning << '\n';
  return entry->config;
}

} // namespace sfs::lsp
