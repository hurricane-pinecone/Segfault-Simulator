#include "scripting/scriptFiles.h"

#include "platform/paths.h"

#include "engine/core/logger/logger.h"
#include "engine/core/util/atomicFile.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace scriptFiles
{

namespace
{
std::string lowercase(std::string_view text)
{
  std::string out(text);
  for (char& c : out)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
} // namespace

std::optional<std::filesystem::path> find(const std::filesystem::path& scriptsFolder, std::string_view name,
                                          Prefer prefer)
{
  const std::string wanted = lowercase(name);
  std::optional<std::filesystem::path> source, header;
  std::error_code error;
  for (auto it = std::filesystem::recursive_directory_iterator(scriptsFolder, error);
       !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
  {
    const std::filesystem::path& path = it->path();
    if (lowercase(path.stem().string()) != wanted)
      continue;
    if (path.extension() == ".cpp")
      source = path;
    else if (path.extension() == ".h")
      header = path;
  }
  return prefer == Prefer::Header ? (header ? header : source) : (source ? source : header);
}

namespace
{
namespace fs = std::filesystem;

bool isSource(const fs::path& file)
{
  const fs::path extension = file.extension();
  return extension == ".h" || extension == ".hpp" || extension == ".inl" || extension == ".cpp";
}

std::vector<fs::path> sources(const fs::path& scriptsFolder)
{
  std::vector<fs::path> out;
  std::error_code error;
  for (auto it = fs::recursive_directory_iterator(scriptsFolder, error);
       !error && it != fs::recursive_directory_iterator(); it.increment(error))
    if (it->is_regular_file(error) && isSource(it->path()))
      out.push_back(it->path());
  return out;
}

std::string readText(const fs::path& file)
{
  std::ifstream in(file, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool writeText(const fs::path& file, const std::string& text)
{
  return sfs::writeFileAtomically(file, text);
}


// An #include in a file: where its name is, the name, and how it's quoted.
struct Include
{
  std::size_t at = 0, size = 0;
  std::string name;
  bool quoted = true; // "name", not <name>
};

std::vector<Include> includesOf(const std::string& text)
{
  std::vector<Include> out;
  std::size_t line = 0;
  while (line < text.size())
  {
    const std::size_t end = std::min(text.find('\n', line), text.size());
    std::size_t i = text.find_first_not_of(" \t", line);
    if (i < end && text[i] == '#')
    {
      i = text.find_first_not_of(" \t", i + 1);
      if (i < end && text.compare(i, 7, "include") == 0)
      {
        i = text.find_first_not_of(" \t", i + 7);
        if (i < end && (text[i] == '"' || text[i] == '<'))
        {
          const char close = text[i] == '"' ? '"' : '>';
          const std::size_t stop = text.find(close, i + 1);
          if (stop < end)
            out.push_back({i + 1, stop - i - 1, text.substr(i + 1, stop - i - 1), close == '"'});
        }
      }
    }
    line = end + 1;
  }
  return out;
}

// Where an include finds its file (as paths::normal): beside the including
// file (quoted), else from the scripts folder. Nothing if it isn't a script's.
std::optional<std::string> resolve(const Include& include, const fs::path& file, const fs::path& scriptsFolder,
                                   const std::function<bool(const std::string&)>& exists)
{
  if (include.quoted)
    if (const std::string beside = paths::normal(file.parent_path() / include.name); exists(beside))
      return beside;
  if (const std::string fromRoot = paths::normal(scriptsFolder / include.name); exists(fromRoot))
    return fromRoot;
  return std::nullopt;
}
} // namespace

namespace
{
// What a script includes, and what each include finds, read where it is.
struct Found
{
  fs::path file;
  std::vector<std::pair<Include, std::string>> includes;
};

Found scan(const fs::path& file, const fs::path& scriptsFolder)
{
  std::error_code error;
  Found entry{file, {}};
  for (const Include& include : includesOf(readText(file)))
    if (const auto target =
            resolve(include, file, scriptsFolder, [&](const std::string& p) { return fs::exists(fs::path(p), error); }))
      entry.includes.push_back({include, *target});
  return entry;
}

// Rewrite the includes of scanned scripts, now at `relocated(file)`, that no
// longer find where their targets now are (`relocated(target)`).
int fixIncludes(const std::vector<Found>& found, const fs::path& scriptsFolder,
                const std::function<std::string(const fs::path&)>& relocated)
{
  int updated = 0;
  for (const Found& entry : found)
  {
    const fs::path file(relocated(entry.file));
    std::string text = readText(file);
    bool changed = false;
    for (auto it = entry.includes.rbegin(); it != entry.includes.rend(); ++it) // back to front: offsets hold
    {
      const auto& [include, target] = *it;
      const std::string now = relocated(fs::path(target));
      if (resolve(include, file, scriptsFolder, [&](const std::string& p) { return p == now; }))
        continue; // still finds it
      // Written as it was: beside the file if it was found beside it (and
      // still is in its folder), else from the scripts folder.
      const bool wasBeside = include.quoted && paths::normal(entry.file.parent_path() / include.name) == target;
      const std::optional<std::string> beside = paths::relative(now, file.parent_path());
      text.replace(include.at, include.size,
                   wasBeside && beside ? *beside : paths::relative(now, scriptsFolder).value_or(now));
      changed = true;
    }
    if (changed && writeText(file, text))
      ++updated;
  }
  return updated;
}

// Rename identifiers in some files' code (not comments or strings).
bool renameIn(const fs::path& file, const std::map<std::string, std::string>& names)
{
  const auto identifierChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  const std::string text = readText(file);
  std::string out;
  out.reserve(text.size());
  bool any = false;
  for (std::size_t i = 0; i < text.size();)
  {
    const char c = text[i];
    const char next = i + 1 < text.size() ? text[i + 1] : '\0';
    // Copied as they are: comments, string and character literals.
    std::size_t skip = 0;
    if (c == '/' && next == '/')
      skip = std::min(text.find('\n', i), text.size()) - i;
    else if (c == '/' && next == '*')
      skip = std::min(text.find("*/", i + 2) + 2, text.size()) - i;
    else if (c == 'R' && next == '"' && (i == 0 || !identifierChar(text[i - 1])))
    {
      const std::size_t open = text.find('(', i + 2);
      const std::string close = ")" + text.substr(i + 2, open - i - 2) + "\"";
      skip = open == std::string::npos ? text.size() - i
                                       : std::min(text.find(close, open) + close.size(), text.size()) - i;
    }
    else if (c == '"' || c == '\'')
    {
      std::size_t j = i + 1;
      while (j < text.size() && text[j] != c && text[j] != '\n')
        j += text[j] == '\\' ? 2 : 1;
      skip = std::min(j + 1, text.size()) - i;
    }
    if (skip > 0)
    {
      out.append(text, i, skip);
      i += skip;
      continue;
    }
    if (identifierChar(c) && !std::isdigit(static_cast<unsigned char>(c)))
    {
      std::size_t j = i;
      while (j < text.size() && identifierChar(text[j]))
        ++j;
      const std::string word = text.substr(i, j - i);
      if (const auto renamed = names.find(word); renamed != names.end())
      {
        out += renamed->second;
        any = true;
      }
      else
        out += word;
      i = j;
      continue;
    }
    out += c;
    ++i;
  }
  return any && writeText(file, out);
}
} // namespace

bool move(const fs::path& scriptsFolder, const std::vector<std::pair<fs::path, fs::path>>& moves)
{
  std::error_code error;
  for (const auto& [from, to] : moves)
    if (!fs::exists(from, error) || fs::exists(to, error))
    {
      LOG_ERROR("Can't move " + from.filename().string() + " to " + to.generic_string() +
                (fs::exists(to, error) ? ": there's one there already" : ": it's gone"));
      return false;
    }
  // Where something is once everything's moved (as paths::normal).
  const auto moved = [&](const fs::path& path)
  {
    for (const auto& [from, to] : moves)
      if (paths::within(path, from))
        return paths::normal(paths::moved(path, from, to));
    return paths::normal(path);
  };

  // What each script includes, found before anything moves.
  std::vector<Found> found;
  for (const fs::path& file : sources(scriptsFolder))
    found.push_back(scan(file, scriptsFolder));

  for (const auto& [from, to] : moves)
  {
    fs::create_directories(to.parent_path(), error);
    fs::rename(from, to, error);
    if (error)
    {
      LOG_ERROR("Can't move " + from.filename().string() + ": " + error.message());
      return false;
    }
  }

  // Includes that no longer find what they did.
  if (const int updated = fixIncludes(found, scriptsFolder, moved); updated > 0)
    LOG_INFO("Updated the #includes in " + std::to_string(updated) + " script" + (updated == 1 ? "" : "s"));
  return true;
}

std::vector<fs::path> copy(const fs::path& scriptsFolder, const fs::path& item, const fs::path& folder)
{
  std::error_code error;
  const bool isFolder = fs::is_directory(item, error);

  // What's copied, and to where: a folder (as "Name Copy" if its name's
  // taken there), or a script with its .h/.cpp pair.
  fs::path top = folder / item.filename();
  if (isFolder)
    for (int n = 1; fs::exists(top, error); ++n)
      top = folder / (item.filename().string() + " Copy" + (n > 1 ? " " + std::to_string(n) : ""));
  std::vector<fs::path> originals;
  if (isFolder)
  {
    for (auto it = fs::recursive_directory_iterator(item, error);
         !error && it != fs::recursive_directory_iterator(); it.increment(error))
      if (it->is_regular_file(error))
        originals.push_back(it->path());
  }
  else
  {
    originals.push_back(item);
    if (isSource(item))
      for (const char* extension : {".h", ".cpp"})
        if (const fs::path pair = item.parent_path() / (item.stem().string() + extension);
            pair != item && fs::exists(pair, error))
          originals.push_back(pair);
  }

  // Every class copied (a header's, named after it) needs a name of its
  // own: <Name>Copy, <Name>Copy2...
  std::map<std::string, std::string> names;
  std::set<std::string> taken;
  for (const fs::path& original : originals)
  {
    const std::string stem = original.stem().string();
    if (original.extension() != ".h" || names.contains(stem))
      continue;
    std::string name = stem + "Copy";
    for (int n = 2; taken.contains(name) || find(scriptsFolder, name, Prefer::Header); ++n)
      name = stem + "Copy" + std::to_string(n);
    names[stem] = name;
    taken.insert(name);
  }
  // Where each copied file goes: under the copy of the folder, or into
  // `folder`; renamed for its class.
  const auto destination = [&](const fs::path& original)
  {
    const fs::path at = isFolder ? fs::path(paths::moved(original, item, top)).parent_path() : folder;
    const auto renamed = names.find(original.stem().string());
    if (renamed != names.end() && isSource(original))
      return at / (renamed->second + original.extension().string());
    // Another file (a README): "<name> Copy" where its name's taken.
    fs::path to = at / original.filename();
    std::error_code taken;
    for (int n = 1; !isFolder && fs::exists(to, taken); ++n)
      to = at / (original.stem().string() + " Copy" + (n > 1 ? " " + std::to_string(n) : "") +
                 original.extension().string());
    return to;
  };

  std::map<std::string, std::string> copies; // original -> copy (paths::normal)
  std::vector<Found> found;
  for (const fs::path& original : originals)
  {
    const fs::path to = destination(original);
    fs::create_directories(to.parent_path(), error);
    fs::copy_file(original, to, error);
    if (error)
    {
      LOG_ERROR("Can't copy " + original.filename().string() + ": " + error.message());
      return {};
    }
    copies[paths::normal(original)] = paths::normal(to);
    if (isSource(original))
      found.push_back(scan(original, scriptsFolder));
  }

  // The copies' includes of each other point at the copies, and their
  // classes have their new names.
  fixIncludes(found, scriptsFolder,
              [&](const fs::path& path)
              {
                const auto copied = copies.find(paths::normal(path));
                return copied != copies.end() ? copied->second : paths::normal(path);
              });
  for (const auto& [original, copied] : copies)
    if (isSource(fs::path(copied)))
      renameIn(fs::path(copied), names);

  if (isFolder)
    return {top};
  std::vector<fs::path> created;
  for (const fs::path& original : originals)
    created.push_back(fs::path(copies[paths::normal(original)]));
  return created;
}

int renameIdentifier(const fs::path& scriptsFolder, const std::string& from, const std::string& to)
{
  int changed = 0;
  for (const fs::path& file : sources(scriptsFolder))
    changed += renameIn(file, {{from, to}}) ? 1 : 0;
  return changed;
}

} // namespace scriptFiles
