#include "analysis.h"
#include "server.h"

#include <sfs/codegen/parser.h>

#include "engine/core/util/atomicFile.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <iostream>
#include <system_error>
#include <tuple>

namespace sfs::lsp
{

namespace
{

bool isChecked(const std::filesystem::path& path)
{
  const std::string extension = path.extension().string();
  return isHeader(path) || extension == ".cpp" || extension == ".cc" ||
         extension == ".cxx";
}

// The folder of the nearest project.sfsproj above `path`, else of the nearest
// .sfs-engine, as an editor would pick the root.
std::filesystem::path rootOf(std::filesystem::path path)
{
  std::error_code error;
  for (const char* marker : {"project.sfsproj", ".sfs-engine"})
    for (std::filesystem::path folder = path; !folder.empty();
         folder = folder.parent_path())
    {
      if (std::filesystem::exists(folder / marker, error))
        return folder;
      if (folder == folder.parent_path())
        break;
    }
  return {};
}

void usage()
{
  std::cerr
      << "usage: sfs-lsp --check [--root DIR] [--disable RULE]... PATH...\n"
         "       sfs-lsp --fix RULE[,RULE...] [--root DIR] PATH...\n";
}

// What --check and --fix are run on.
struct Run
{
  std::filesystem::path root;
  codegen::RuleSet disabled;
  std::vector<std::filesystem::path> files; // sorted, each once
};

// Reads the shared arguments ([--root DIR] [--disable RULE]... PATH...) and
// lists the files they name. False when they're wrong (with the usage
// printed) or name no C++ files (with a message).
bool readRun(const std::vector<std::string>& args, bool allowDisable, Run& run)
{
  std::vector<std::filesystem::path> paths;
  std::vector<std::string> given;
  for (std::size_t i = 0; i < args.size(); ++i)
  {
    if ((args[i] == "--root" || args[i] == "--disable") && i + 1 >= args.size())
    {
      usage();
      return false;
    }
    if (args[i] == "--root")
      run.root = args[++i];
    else if (args[i] == "--disable" && allowDisable)
    {
      if (const std::string problem = disableRule(args[++i], run.disabled);
          !problem.empty())
        std::cerr << "sfs-lsp: " << problem << '\n';
    }
    else if (args[i].starts_with("--"))
    {
      usage();
      return false;
    }
    else
      given.push_back(args[i]);
  }
  if (given.empty())
  {
    usage();
    return false;
  }
  // Relative paths are the root's when one is given, else the working
  // folder's.
  const bool rooted = !run.root.empty();
  if (rooted)
    run.root = std::filesystem::absolute(run.root).lexically_normal();
  for (const std::string& path : given)
  {
    const std::filesystem::path spelled(path);
    paths.push_back((rooted && spelled.is_relative()
                         ? run.root / spelled
                         : std::filesystem::absolute(spelled))
                        .lexically_normal());
  }
  if (!rooted)
    run.root = rootOf(paths.front());

  std::error_code error;
  for (const std::filesystem::path& path : paths)
  {
    if (std::filesystem::is_directory(path, error))
    {
      for (std::filesystem::recursive_directory_iterator it(path, error), end;
           !error && it != end;
           it.increment(error))
        if (it->is_regular_file(error) && isChecked(it->path()))
          run.files.push_back(it->path().lexically_normal());
    }
    else if (std::filesystem::is_regular_file(path, error))
      run.files.push_back(path);
    else
      std::cerr << "sfs-lsp: " << path.string()
                << " isn't a file or a folder\n";
  }
  std::sort(run.files.begin(), run.files.end());
  run.files.erase(
      std::unique(run.files.begin(), run.files.end()), run.files.end());
  // Nothing to check is a wrong path, not a clean project.
  if (run.files.empty())
  {
    std::cerr << "sfs-lsp: no C++ files under the paths given\n";
    return false;
  }
  return true;
}

// One file --fix reads and may change: its text on disk and as fixed so far.
struct FixedFile
{
  std::filesystem::path path;
  std::string original;
  std::string text;
};

// One edit of a fix --fix takes, in byte offsets of its file's text.
struct Accepted
{
  FixedFile* file;
  uint32_t from;
  uint32_t to;
  std::string text;
};

// The byte offset of a source point in `text`.
uint32_t offsetOf(const std::vector<uint32_t>& lines,
                  const std::string& text,
                  unsigned row,
                  unsigned column)
{
  if (row >= lines.size())
    return static_cast<uint32_t>(text.size());
  return std::min(lines[row] + column, static_cast<uint32_t>(text.size()));
}

} // namespace

int runCheck(const std::vector<std::string>& args)
{
  Run run;
  if (!readRun(args, true, run))
    return 2;

  const ParserPtr parser = makeCppParser();
  ProjectIndex index;
  index.start(run.root);
  index.indexAll(parser.get());
  Linter linter;

  bool failed = false;
  for (const std::filesystem::path& file : run.files)
  {
    Document document;
    document.path = file;
    setText(document, codegen::readFile(file));
    analyse(document, parser.get(), linter, index, run.disabled);
    for (const codegen::Diagnostic& diagnostic : document.diagnostics)
    {
      failed |= codegen::severityOf(diagnostic) == codegen::Severity::Error;
      std::cout << codegen::formatDiagnostic(diagnostic, true) << '\n';
    }
  }
  return failed ? 1 : 0;
}

int runFix(const std::vector<std::string>& args)
{
  if (args.empty() || args.front().starts_with("--"))
  {
    usage();
    return 2;
  }
  // The rules, in the order given: each one's fixes go in before the next's.
  std::vector<codegen::RuleId> rules;
  codegen::RuleSet named;
  const std::string& list = args.front();
  for (std::size_t start = 0; start <= list.size();)
  {
    std::size_t comma = list.find(',', start);
    if (comma == std::string::npos)
      comma = list.size();
    const std::string name = list.substr(start, comma - start);
    start = comma + 1;
    if (name.empty())
      continue;
    const codegen::RuleInfo* rule = codegen::findRule(name);
    if (!rule)
    {
      std::cerr << "sfs-lsp: unknown rule '" << name << "'\n";
      return 2;
    }
    rules.push_back(rule->id);
    named.set(static_cast<std::size_t>(rule->id));
  }
  if (rules.empty())
  {
    usage();
    return 2;
  }
  Run run;
  if (!readRun(
          std::vector<std::string>(args.begin() + 1, args.end()), false, run))
    return 2;

  const ParserPtr parser = makeCppParser();
  ProjectIndex index;
  index.start(run.root);
  index.indexAll(parser.get());
  Linter linter;

  // Every file read so far, as fixed so far: what the lints of other files
  // read (a class's header), and what's written at the end.
  std::vector<std::unique_ptr<FixedFile>> texts;
  const auto fileOf = [&](const std::filesystem::path& path) -> FixedFile&
  {
    const std::filesystem::path normal = path.lexically_normal();
    for (const std::unique_ptr<FixedFile>& known : texts)
      if (known->path == normal)
        return *known;
    texts.push_back(std::make_unique<FixedFile>());
    FixedFile& made = *texts.back();
    made.path = normal;
    made.original = codegen::readFile(normal);
    made.text = made.original;
    return made;
  };
  const FileReader read = [&](const std::filesystem::path& path)
  { return fileOf(path).text; };

  constexpr int kPasses = 8;
  std::vector<Accepted> accepted;
  std::vector<Accepted> edits;
  std::vector<std::pair<FixedFile*, std::vector<uint32_t>>> lineTables;
  for (const codegen::RuleId rule : rules)
    for (const std::filesystem::path& file : run.files)
      for (int pass = 0; pass < kPasses; ++pass)
      {
        Document document;
        document.path = file;
        setText(document, fileOf(file).text);
        analyse(document, parser.get(), linter, index, {}, nullptr, &read);

        // This pass's fixes: the rule's, in order, none overlapping another.
        accepted.clear();
        lineTables.clear();
        const auto linesOf = [&](FixedFile& target) -> std::vector<uint32_t>&
        {
          for (auto& [known, lines] : lineTables)
            if (known == &target)
              return lines;
          lineTables.push_back({&target, countLines(target.text)});
          return lineTables.back().second;
        };
        for (const codegen::Diagnostic& diagnostic : document.diagnostics)
        {
          if (diagnostic.rule != rule || diagnostic.fix.empty())
            continue;
          edits.clear();
          for (const codegen::TextEdit& edit : diagnostic.fix)
          {
            FixedFile& target = fileOf(edit.file.empty() ? file : edit.file);
            const std::vector<uint32_t>& lines = linesOf(target);
            edits.push_back({&target,
                             offsetOf(lines,
                                      target.text,
                                      edit.range.startRow,
                                      edit.range.startColumn),
                             offsetOf(lines,
                                      target.text,
                                      edit.range.endRow,
                                      edit.range.endColumn),
                             edit.text});
          }
          // Overlapping, or starting where another starts, waits a pass.
          const bool clash = std::any_of(
              edits.begin(),
              edits.end(),
              [&](const Accepted& edit)
              {
                return std::any_of(
                    accepted.begin(),
                    accepted.end(),
                    [&](const Accepted& other)
                    {
                      return other.file == edit.file &&
                             (other.from == edit.from ||
                              (edit.from < other.to && other.from < edit.to));
                    });
              });
          if (clash)
            continue;
          accepted.insert(accepted.end(), edits.begin(), edits.end());
          std::cout << file.string() << ':' << diagnostic.range.startRow + 1
                    << ':' << diagnostic.range.startColumn + 1 << ": fixed ["
                    << codegen::ruleInfo(rule).name << "] "
                    << diagnostic.fixTitle << '\n';
        }
        if (accepted.empty())
          break;
        std::sort(accepted.begin(),
                  accepted.end(),
                  [](const Accepted& a, const Accepted& b) {
                    return std::tie(a.file, a.from) > std::tie(b.file, b.from);
                  });
        for (const Accepted& edit : accepted)
          edit.file->text.replace(edit.from, edit.to - edit.from, edit.text);
      }

  bool failed = false;
  for (const std::unique_ptr<FixedFile>& changed : texts)
    if (changed->text != changed->original &&
        !writeFileAtomically(changed->path, changed->text))
      failed = true;

  // What's left of the rules, and any error.
  for (const std::filesystem::path& file : run.files)
  {
    Document document;
    document.path = file;
    setText(document, fileOf(file).text);
    analyse(document, parser.get(), linter, index, {}, nullptr, &read);
    for (const codegen::Diagnostic& diagnostic : document.diagnostics)
    {
      const bool error =
          codegen::severityOf(diagnostic) == codegen::Severity::Error;
      failed |= error;
      if (error || named.test(static_cast<std::size_t>(diagnostic.rule)))
        std::cout << codegen::formatDiagnostic(diagnostic, true) << '\n';
    }
  }
  return failed ? 1 : 0;
}

} // namespace sfs::lsp
