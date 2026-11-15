#include "server.h"

#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <fcntl.h>
  #include <io.h>
#endif

// sfs-lsp: the SFS language server, run beside clangd by an editor.
//
//   sfs-lsp [--log-timing]       serve LSP over stdin and stdout
//   sfs-lsp --check [--root DIR] [--disable RULE]... PATH...
//   sfs-lsp --fix RULE[,RULE...] [--root DIR] PATH...
//   sfs-lsp --version
//
// Stdout carries only the protocol; logs go to stderr.
namespace
{

void usage()
{
  std::cerr
      << "usage: sfs-lsp [--log-timing]\n"
         "       sfs-lsp --check [--root DIR] [--disable RULE]... PATH...\n"
         "       sfs-lsp --fix RULE[,RULE...] [--root DIR] PATH...\n"
         "       sfs-lsp --version\n";
}

} // namespace

int main(int argc, char* argv[])
{
  bool logTiming = false;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--version")
    {
      std::cout << "sfs-lsp " << SFS_ENGINE_VERSION << '\n';
      return 0;
    }
    if (arg == "--check" || arg == "--fix")
    {
      try
      {
        const std::vector<std::string> rest(argv + i + 1, argv + argc);
        return arg == "--check" ? sfs::lsp::runCheck(rest)
                                : sfs::lsp::runFix(rest);
      }
      catch (const std::exception& error)
      {
        std::cerr << "sfs-lsp: " << error.what() << '\n';
        return 2;
      }
    }
    if (arg == "--log-timing")
      logTiming = true;
    else
    {
      usage();
      return 2;
    }
  }

#if defined(_WIN32)
  // CRLF translation would break the Content-Length framing.
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  try
  {
    return sfs::lsp::Server(logTiming).run();
  }
  catch (const std::exception& error)
  {
    std::cerr << "sfs-lsp: " << error.what() << '\n';
    return 1;
  }
}
