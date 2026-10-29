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
//               [--ui-previews] <header>...
//
// With --ui-previews, the headers are UI headers (*.ui.h) and the output is
// the editor's UI preview module: their views, filled with placeholders.
//
// Exits non-zero (and writes nothing) if any header has an error. The output
// file is only rewritten when its contents change, so an unchanged scan
// doesn't trigger a rebuild.
namespace
{

void usage()
{
  std::cerr << "usage: sfs-codegen --module <name> --function <qualified-name> "
               "--output <file.cpp> [--function-header <header>] "
               "[--include-root <dir>] [--ui-previews] <header>...\n";
}

std::string readFile(const std::filesystem::path& path)
{
  std::ifstream in(path, std::ios::binary);
  std::ostringstream contents;
  contents << in.rdbuf();
  return contents.str();
}

void print(const sfs::codegen::Diagnostic& diagnostic)
{
  const char* severity =
      diagnostic.severity == sfs::codegen::Diagnostic::Severity::Error
          ? "error"
          : "warning";
  // file:line: severity: message, the format editors and IDEs parse.
  std::cerr << diagnostic.file.string() << ':' << diagnostic.line << ": "
            << severity << ": " << diagnostic.message << '\n';
}

// The script registration for `headers`; false if one has an error.
bool generateScripts(const std::vector<std::filesystem::path>& headers,
                     const sfs::codegen::GeneratorOptions& options,
                     std::string& generated)
{
  std::vector<sfs::codegen::TypeModel> types;
  bool failed = false;
  for (const std::filesystem::path& header : headers)
  {
    sfs::codegen::ParseResult result =
        sfs::codegen::parseHeader(readFile(header), header);
    for (const sfs::codegen::Diagnostic& diagnostic : result.diagnostics)
    {
      print(diagnostic);
      failed |=
          diagnostic.severity == sfs::codegen::Diagnostic::Severity::Error;
    }
    if (result.type)
      types.push_back(std::move(*result.type));
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
  std::vector<std::filesystem::path> headers;
  bool uiPreviews = false;

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
    else if (arg.starts_with("--"))
    {
      usage();
      return 2;
    }
    else
      headers.emplace_back(arg);
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
          sfs::codegen::parseUiHeader(readFile(header), header);
      for (const sfs::codegen::Diagnostic& diagnostic : result.diagnostics)
        print(diagnostic);
      for (sfs::codegen::UiTypeModel& type : result.types)
        types.push_back(std::move(type));
    }
    generated = sfs::codegen::generateUiPreviews(types, options);
  }
  else if (!generateScripts(headers, options, generated))
    return 1;

  if (std::filesystem::exists(output) && readFile(output) == generated)
    return 0;

  if (output.has_parent_path())
    std::filesystem::create_directories(output.parent_path());
  std::ofstream(output, std::ios::binary) << generated;
  return 0;
}
