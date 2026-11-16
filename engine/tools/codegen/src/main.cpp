#include "sfs/codegen/generator.h"
#include "sfs/codegen/parser.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// sfs-codegen: generate reflection registration for script headers.
//
//   sfs-codegen --module <name> --function <qualified-name> --output <file.cpp>
//               [--function-header <header>] [--include-root <dir>]
//               [--event-traits <file.h> [--event-source <file.cpp>]...]
//               [--ui-previews] <header>...
//   sfs-codegen --events-only --event-traits <file.h> <file>...
//
// With --ui-previews, the headers are UI headers (*.ui.h) and the output is
// the editor's UI preview module: their views, filled with placeholders.
// --event-traits also writes the traits header of the files' event types
// (generateEventTraits()) and [[sfs::scene_data]] types
// (generateSceneDataTraits()), which the target includes first, with those
// of each --event-source file (scanned for traits alone); --events-only
// writes only that, from any source files (a test's .cpp).
//
// Diagnostics print as `file:line: severity: message [rule]`; a header's
// sfs-ignore comments hide the warnings they name. Exits non-zero (and writes
// nothing) if any header has an error, which can't be hidden. The output
// file is only rewritten when its contents change, so an unchanged scan
// doesn't trigger a rebuild.
namespace
{

void usage()
{
  std::cerr << "usage: sfs-codegen --module <name> --function <qualified-name> "
               "--output <file.cpp> [--function-header <header>] "
               "[--include-root <dir>] [--event-traits <file.h> "
               "[--event-source <file>]...] [--ui-previews] <header>...\n"
               "       sfs-codegen --events-only --event-traits <file.h> "
               "<file>...\n";
}

bool isError(const sfs::codegen::Diagnostic& diagnostic)
{
  return sfs::codegen::severityOf(diagnostic) == sfs::codegen::Severity::Error;
}

// file:line: severity: message [rule], the format editors and IDEs parse; the
// rule is what an sfs-ignore comment names.
void print(const sfs::codegen::Diagnostic& diagnostic)
{
  std::cerr << sfs::codegen::formatDiagnostic(diagnostic, false) << '\n';
}

// Writes `text` to `path` unless it already holds it, so an unchanged scan
// doesn't trigger a rebuild.
void writeIfChanged(const std::filesystem::path& path, const std::string& text)
{
  if (std::filesystem::exists(path) && sfs::codegen::readFile(path) == text)
    return;
  if (path.has_parent_path())
    std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

// Whether a diagnostic is about what the traits header says (event and
// scene data declarations): all that a file scanned for traits alone shows.
bool aboutTraits(const sfs::codegen::Diagnostic& diagnostic)
{
  using sfs::codegen::RuleId;
  return diagnostic.rule == RuleId::EventDeclaration ||
         diagnostic.rule == RuleId::EventAndComponent ||
         diagnostic.rule == RuleId::SceneDataDeclaration;
}

// The traits header for the event types and scene data types `results`
// declare; false if a declaration is wrong.
bool generateTraits(const std::vector<sfs::codegen::ParseResult>& results,
                    std::string& generated)
{
  std::vector<sfs::codegen::EventModel> events;
  std::vector<std::string> plainTypes;
  std::vector<sfs::codegen::TypeModel> sceneData;
  for (const sfs::codegen::ParseResult& result : results)
  {
    events.insert(events.end(), result.events.begin(), result.events.end());
    plainTypes.insert(
        plainTypes.end(), result.plainTypes.begin(), result.plainTypes.end());
    if (result.type && result.type->kind == sfs::codegen::TypeKind::SceneData)
      sceneData.push_back(*result.type);
    for (const sfs::codegen::TypeModel& type : result.components)
      if (type.kind == sfs::codegen::TypeKind::SceneData)
        sceneData.push_back(type);
  }
  std::vector<sfs::codegen::Diagnostic> diagnostics;
  generated =
      sfs::codegen::generateEventTraits(events, plainTypes, diagnostics) +
      sfs::codegen::generateSceneDataTraits(sceneData);
  for (const sfs::codegen::Diagnostic& diagnostic : diagnostics)
    print(diagnostic);
  return diagnostics.empty();
}

// Parses each of `files` for its traits alone (event types, scene data
// types) into `results`, printing those diagnostics; false if one is an
// error.
bool scanEvents(const std::vector<std::filesystem::path>& files,
                std::vector<sfs::codegen::ParseResult>& results)
{
  bool failed = false;
  for (const std::filesystem::path& file : files)
  {
    sfs::codegen::ParseResult& result = results.emplace_back(
        sfs::codegen::parseHeader(sfs::codegen::readFile(file), file));
    for (const sfs::codegen::Diagnostic& diagnostic : result.diagnostics)
      if (aboutTraits(diagnostic))
      {
        print(diagnostic);
        failed |= isError(diagnostic);
      }
  }
  return !failed;
}

// The script registration for `headers`, and their and `eventSources`'
// event traits; false if one has an error.
bool generateScripts(const std::vector<std::filesystem::path>& headers,
                     const std::vector<std::filesystem::path>& eventSources,
                     const sfs::codegen::GeneratorOptions& options,
                     std::string& generated,
                     std::string* traits)
{
  std::vector<sfs::codegen::ParseResult> results;
  results.reserve(headers.size() + eventSources.size());
  bool failed = false;
  for (const std::filesystem::path& header : headers)
  {
    sfs::codegen::ParseResult& result = results.emplace_back(
        sfs::codegen::parseHeader(sfs::codegen::readFile(header), header));
    for (const sfs::codegen::Diagnostic& diagnostic : result.diagnostics)
    {
      print(diagnostic);
      failed |= isError(diagnostic);
    }
  }
  if (traits && (!scanEvents(eventSources, results) ||
                 !generateTraits(results, *traits)))
    failed = true;
  // The headers' types, taken once the traits have read them.
  std::vector<sfs::codegen::TypeModel> types;
  for (std::size_t i = 0; i < headers.size(); ++i)
  {
    sfs::codegen::ParseResult& result = results[i];
    if (result.type)
      types.push_back(std::move(*result.type));
    for (sfs::codegen::TypeModel& type : result.components)
      types.push_back(std::move(type));
  }
  for (const sfs::codegen::Diagnostic& diagnostic :
       sfs::codegen::checkNames(types))
  {
    print(diagnostic);
    failed = true;
  }
  if (failed)
    return false;
  generated = sfs::codegen::generateRegistration(types, options);
  return true;
}

} // namespace

int main(int argc, char* argv[])
{
  sfs::codegen::GeneratorOptions options;
  std::filesystem::path output;
  std::filesystem::path traitsOutput;
  std::vector<std::filesystem::path> headers;
  std::vector<std::filesystem::path> eventSources;
  bool uiPreviews = false;
  bool eventsOnly = false;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    const auto value = [&]() -> std::string
    {
      if (i + 1 >= argc)
      {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg == "--module")
      options.module = value();
    else if (arg == "--function")
      options.function = value();
    else if (arg == "--function-header")
      options.functionHeader = value();
    else if (arg == "--include-root")
      options.includeRoot = value();
    else if (arg == "--output")
      output = value();
    else if (arg == "--ui-previews")
      uiPreviews = true;
    else if (arg == "--event-traits")
      traitsOutput = value();
    else if (arg == "--events-only")
      eventsOnly = true;
    else if (arg == "--event-source")
      eventSources.emplace_back(value());
    else if (arg.starts_with("--"))
    {
      usage();
      return 2;
    }
    else
      headers.emplace_back(arg);
  }

  if (eventsOnly)
  {
    if (traitsOutput.empty())
    {
      usage();
      return 2;
    }
    std::vector<sfs::codegen::ParseResult> results;
    const bool scanned = scanEvents(headers, results);
    std::string traits;
    if (!generateTraits(results, traits) || !scanned)
      return 1;
    writeIfChanged(traitsOutput, traits);
    return 0;
  }

  if (options.module.empty() || options.function.empty() || output.empty())
  {
    usage();
    return 2;
  }

  std::string generated;
  if (uiPreviews)
  {
    std::vector<sfs::codegen::UiTypeModel> types;
    for (const std::filesystem::path& header : headers)
    {
      sfs::codegen::UiParseResult result =
          sfs::codegen::parseUiHeader(sfs::codegen::readFile(header), header);
      for (const sfs::codegen::Diagnostic& diagnostic : result.diagnostics)
        print(diagnostic);
      for (sfs::codegen::UiTypeModel& type : result.types)
        types.push_back(std::move(type));
    }
    generated = sfs::codegen::generateUiPreviews(types, options);
  }
  else
  {
    std::string traits;
    if (!generateScripts(headers,
                         eventSources,
                         options,
                         generated,
                         traitsOutput.empty() ? nullptr : &traits))
      return 1;
    if (!traitsOutput.empty())
      writeIfChanged(traitsOutput, traits);
  }

  writeIfChanged(output, generated);
  return 0;
}
