#include "format.h"

#include "document.h"
#include "formatBlankLines.h"
#include "formatOrder.h"
#include "lintSupport.h"
#include "runProcess.h"

#include <sfs/codegen/parser.h>

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>

namespace sfs::lsp
{

namespace
{

// How long clang-format may take on one file before it's stopped.
constexpr std::chrono::milliseconds CLANG_FORMAT_LIMIT{20000};

using TreePtr = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

TreePtr parse(TSParser* parser, std::string_view text)
{
  return TreePtr(
      ts_parser_parse_string(
          parser, nullptr, text.data(), static_cast<uint32_t>(text.size())),
      &ts_tree_delete);
}

bool isSource(const std::filesystem::path& path)
{
  const std::string extension = path.extension().string();
  return extension == ".cpp" || extension == ".cc" || extension == ".cxx";
}

// A program on PATH, or empty.
std::string onPath(const std::string& name)
{
  const char* path = std::getenv("PATH");

  if (!path)
    return {};
#if defined(_WIN32)
  constexpr char SEPARATOR = ';';
  const std::string file = name.ends_with(".exe") ? name : name + ".exe";
#else
  constexpr char SEPARATOR = ':';
  const std::string& file = name;
#endif
  std::error_code error;

  for (std::string_view rest = path; !rest.empty();)
  {
    const std::size_t at = rest.find(SEPARATOR);
    const std::string_view folder = rest.substr(0, at);
    rest =
        at == std::string_view::npos ? std::string_view() : rest.substr(at + 1);

    if (folder.empty())
      continue;

    const std::filesystem::path candidate =
        std::filesystem::path(folder) / file;

    if (std::filesystem::is_regular_file(candidate, error))
      return candidate.string();
  }

  return {};
}

// A configured program: a path as given, or a name found on PATH.
std::string resolve(const std::string& program)
{
  if (program.find('/') != std::string::npos ||
      program.find('\\') != std::string::npos)
  {
    std::error_code error;
    return std::filesystem::is_regular_file(program, error) ? program
                                                            : std::string();
  }

  return onPath(program);
}

// The first line of a program's error output, for a note.
std::string firstLine(std::string_view errors)
{
  errors = lint::trimmed(errors);
  return std::string(errors.substr(0, errors.find('\n')));
}

std::vector<std::string_view> splitLines(std::string_view text)
{
  std::vector<std::string_view> lines;

  for (std::size_t start = 0; start < text.size();)
  {
    std::size_t end = text.find('\n', start);
    end = end == std::string_view::npos ? text.size() : end + 1;
    lines.push_back(text.substr(start, end - start));
    start = end;
  }

  return lines;
}

// The lines that count for sameLines(), sorted.
std::vector<std::string_view> countedLines(std::string_view text)
{
  std::vector<std::string_view> counted;

  for (std::string_view line : splitLines(text))
  {
    while (!line.empty() && lint::isSpace(line.back()))
      line.remove_suffix(1);

    const std::string_view bare = lint::trimmed(line);

    if (bare.empty() || bare == "public:" || bare == "protected:" ||
        bare == "private:")
      continue;

    counted.push_back(line);
  }

  std::sort(counted.begin(), counted.end());
  return counted;
}

} // namespace

HouseStyle parseHouseStyle(std::string_view text)
{
  HouseStyle style;
  style.on = true;
  std::istringstream lines{std::string(text)};

  for (std::string line; std::getline(lines, line);)
  {
    const std::string_view bare = lint::trimmed(line);

    if (bare.empty() || bare.front() == '#')
      continue;

    const std::size_t colon = bare.find(':');

    if (colon == std::string_view::npos)
    {
      style.problems.push_back("a line isn't 'key: value': " +
                               std::string(bare));
      continue;
    }

    const std::string_view key = lint::trimmed(bare.substr(0, colon));
    const std::string_view value = lint::trimmed(bare.substr(colon + 1));

    if (key == "sort-fields")
    {
      if (value == "true" || value == "false")
        style.sortFields = value == "true";
      else
        style.problems.push_back("sort-fields is true or false, not '" +
                                 std::string(value) + "'");
    }
    else if (key == "exclude")
    {
      for (const std::string_view item : lint::splitTerms(value))
      {
        std::string folder(item);
        std::replace(folder.begin(), folder.end(), '\\', '/');

        while (!folder.empty() && folder.back() == '/')
          folder.pop_back();

        if (!folder.empty())
          style.exclude.push_back(std::move(folder));
      }
    }
    else
      style.problems.push_back("unknown key '" + std::string(key) + "'");
  }

  return style;
}

HouseStyle houseStyleFor(const std::filesystem::path& file)
{
  std::error_code error;

  for (std::filesystem::path folder = file.parent_path(); !folder.empty();
       folder = folder.parent_path())
  {
    const std::filesystem::path config = folder / ".sfs-format";

    if (std::filesystem::is_regular_file(config, error))
    {
      std::ifstream in(config, std::ios::binary);
      std::stringstream text;
      text << in.rdbuf();
      HouseStyle style = parseHouseStyle(text.str());
      style.root = folder;
      return style;
    }

    if (folder == folder.parent_path())
      break;
  }

  return {};
}

bool isExcluded(const HouseStyle& style, const std::filesystem::path& file)
{
  if (!style.on || style.exclude.empty())
    return false;

  const std::string relative =
      file.lexically_normal()
          .lexically_relative(style.root.lexically_normal())
          .generic_string();

  if (relative.empty() || relative.starts_with(".."))
    return false;

  for (const std::string& folder : style.exclude)
    if (relative == folder || relative.starts_with(folder + "/"))
      return true;

  return false;
}

std::string findClangFormat(const std::string& configured)
{
  if (!configured.empty())
    return resolve(configured);

  if (const char* variable = std::getenv("SFS_CLANG_FORMAT");
      variable && *variable)
    return resolve(variable);

  for (const char* name : {"clang-format", "clang-format-17"})
    if (std::string found = onPath(name); !found.empty())
      return found;

  return {};
}

FormatResult formatSource(const FormatRequest& request, TSParser* parser)
{
  FormatResult result;
  result.text = std::string(request.text);
  const HouseStyle style = houseStyleFor(request.path);

  for (const std::string& problem : style.problems)
    result.notes.push_back(".sfs-format: " + problem);

  if (isExcluded(style, request.path))
  {
    result.excluded = true;
    return result;
  }

  const bool ranged = request.firstLine > 0;

  const std::string clangFormat = findClangFormat(request.clangFormat);
  result.noClangFormat = clangFormat.empty();
  const auto runClangFormat = [&](const std::string& input, std::string& output)
  {
    std::vector<std::string> args = {
        clangFormat,
        "--assume-filename=" + request.path.string(),
        "--fallback-style=none"};

    if (ranged)
      args.push_back(
          "--lines=" + std::to_string(request.firstLine) + ":" +
          std::to_string(std::max(request.firstLine, request.lastLine)));

    const ProcessResult run = runProcess(args, input, CLANG_FORMAT_LIMIT);

    if (run.started && !run.timedOut && run.exitCode == 0)
    {
      output = run.output;
      return true;
    }

    result.clangFormatFailed = true;

    if (!run.started)
      result.notes.push_back("clang-format (" + clangFormat +
                             ") couldn't be started");
    else if (run.timedOut)
      result.notes.push_back("clang-format took too long and was stopped");
    else
      result.notes.push_back("clang-format failed: " + firstLine(run.errors));

    return false;
  };

  if (!result.noClangFormat)
    runClangFormat(result.text, result.text);

  if (!style.on)
    return result;

  // The passes work on whole lines: a last line without a break gets one
  // while they run.
  std::string house = result.text;
  const bool addedBreak = !house.empty() && house.back() != '\n';

  if (addedBreak)
    house += '\n';

  TreePtr tree = parse(parser, house);

  if (!tree)
    return result;

  bool reordered = false;

  if (ranged)
  {
    // clang-format may have changed the range's line count.
    const auto lineCount = [](std::string_view text) {
      return static_cast<int64_t>(std::count(text.begin(), text.end(), '\n'));
    };
    const int64_t grown = lineCount(house) - lineCount(request.text);
    const int64_t last = static_cast<int64_t>(request.lastLine) - 1 + grown;
    // A blank line below the range's last line is the range's too.
    house = addControlBlankLines(
        house,
        tree.get(),
        request.firstLine - 1,
        static_cast<uint32_t>(std::max<int64_t>(last + 1, request.firstLine)));
  }
  else
  {
    std::string ordered;

    if (isHeader(request.path))
      ordered =
          orderClassBodies(house, tree.get(), style.sortFields, result.notes);
    else if (isSource(request.path))
      ordered = orderDefinitions(
          house,
          tree.get(),
          request.path,
          parser,
          request.read ? *request.read
                       : FileReader([](const std::filesystem::path& path)
                                    { return codegen::readFile(path); }),
          result.notes);

    if (!ordered.empty() && ordered != house)
    {
      house = std::move(ordered);
      tree = parse(parser, house);
      reordered = true;

      if (!tree)
        return result;
    }

    house = addControlBlankLines(house, tree.get());
  }

  if (addedBreak && !house.empty() && house.back() == '\n')
    house.pop_back();

  if (house == result.text)
    return result;

  if (!sameLines(result.text, house))
  {
    result.refused = true;
    result.notes.push_back(
        "the house passes would have lost or added a line, so only "
        "clang-format's changes were kept (a bug in sfs-lsp: please report "
        "it with the file)");
    return result;
  }

  result.text = std::move(house);

  // Moved lines may need their trailing comments aligned again.
  if (reordered && !result.noClangFormat && !result.clangFormatFailed)
    runClangFormat(result.text, result.text);

  return result;
}

bool sameLines(std::string_view before, std::string_view after)
{
  return countedLines(before) == countedLines(after);
}

std::vector<LineEdit> lineEdits(std::string_view before, std::string_view after)
{
  const std::vector<std::string_view> a = splitLines(before);
  const std::vector<std::string_view> b = splitLines(after);
  std::vector<uint32_t> offsets;
  offsets.reserve(a.size() + 1);
  {
    uint32_t at = 0;

    for (const std::string_view line : a)
    {
      offsets.push_back(at);
      at += static_cast<uint32_t>(line.size());
    }

    offsets.push_back(at);
  }

  std::size_t prefix = 0;

  while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix])
    ++prefix;

  std::size_t suffix = 0;

  while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
         a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix])
    ++suffix;

  const int n = static_cast<int>(a.size() - prefix - suffix);
  const int m = static_cast<int>(b.size() - prefix - suffix);

  if (n == 0 && m == 0)
    return {};

  const auto lineA = [&](int i)
  { return a[prefix + static_cast<std::size_t>(i)]; };
  const auto lineB = [&](int j)
  { return b[prefix + static_cast<std::size_t>(j)]; };
  const auto joined = [&](int from, int to)
  {
    std::string text;

    for (int j = from; j < to; ++j)
      text += lineB(j);

    return text;
  };

  // Myers' greedy diff, keeping each step's furthest reaches to walk back.
  constexpr int MOST_EDITS = 1000;
  const int most = std::min(n + m, MOST_EDITS);
  const int offset = most + 1;
  std::vector<int> reach(static_cast<std::size_t>(2 * most + 3), 0);
  std::vector<std::vector<int>> trace;
  int found = -1;

  for (int d = 0; d <= most && found < 0; ++d)
  {
    trace.push_back(reach);

    for (int k = -d; k <= d; k += 2)
    {
      int x =
          (k == -d || (k != d && reach[offset + k - 1] < reach[offset + k + 1]))
              ? reach[offset + k + 1]
              : reach[offset + k - 1] + 1;
      int y = x - k;

      while (x < n && y < m && lineA(x) == lineB(y))
      {
        ++x;
        ++y;
      }

      reach[offset + k] = x;

      if (x >= n && y >= m)
      {
        found = d;
        break;
      }
    }
  }

  if (found < 0)
    return {LineEdit{offsets[prefix], offsets[prefix + n], joined(0, m)}};

  // The lines kept, walking back from the end.
  std::vector<std::pair<int, int>> kept;
  int x = n;
  int y = m;

  for (int d = found; d > 0; --d)
  {
    const std::vector<int>& previous = trace[static_cast<std::size_t>(d)];
    const int k = x - y;
    const int fromK = (k == -d || (k != d && previous[offset + k - 1] <
                                                 previous[offset + k + 1]))
                          ? k + 1
                          : k - 1;
    const int fromX = previous[offset + fromK];
    const int fromY = fromX - fromK;

    while (x > fromX && y > fromY)
      kept.emplace_back(--x, --y);

    x = fromX;
    y = fromY;
  }

  while (x > 0 && y > 0)
    kept.emplace_back(--x, --y);

  std::reverse(kept.begin(), kept.end());

  std::vector<LineEdit> edits;
  int i = 0;
  int j = 0;
  const auto hunk = [&](int toI, int toJ)
  {
    if (toI > i || toJ > j)
      edits.push_back({offsets[prefix + static_cast<std::size_t>(i)],
                       offsets[prefix + static_cast<std::size_t>(toI)],
                       joined(j, toJ)});
  };

  for (const auto& [keptI, keptJ] : kept)
  {
    hunk(keptI, keptJ);
    i = keptI + 1;
    j = keptJ + 1;
  }

  hunk(n, m);
  return edits;
}

} // namespace sfs::lsp
