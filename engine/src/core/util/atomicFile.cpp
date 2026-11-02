#include "engine/core/util/atomicFile.h"

#include "engine/core/logger/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <system_error>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <cerrno>
  #include <cstring>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace sfs
{

namespace
{
enum class Written
{
  Ok,
  NameTaken, // the temporary file's name is in use: pick another
  Failed,
};

// A name for the temporary file, unique enough that two writers (two threads,
// two editors) don't pick the same; creating it exclusively catches the rest.
fs::path temporaryPathFor(const fs::path& target)
{
  static std::atomic<unsigned> counter{0};
  const auto now = static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fs::path name = target.filename();
  return target.parent_path() /
         ("." + name.string() + ".tmp-" + std::to_string(now % 1000000007ull) +
          "-" + std::to_string(counter.fetch_add(1)));
}

void fail(const fs::path& path, const std::string& why)
{
  LOG_ERROR("Can't write " + path.string() + ": " + why);
}

#if defined(_WIN32)

std::string lastError()
{
  const DWORD code = GetLastError();
  char* text = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr,
                 code,
                 0,
                 reinterpret_cast<LPSTR>(&text),
                 0,
                 nullptr);
  std::string message = text ? text : "error " + std::to_string(code);
  if (text)
    LocalFree(text);
  while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
    message.pop_back();
  return message;
}

Written writeTemporary(const fs::path& temporary,
                       const fs::path&,
                       std::string_view contents,
                       std::string& error)
{
  const HANDLE file = CreateFileW(temporary.wstring().c_str(),
                                  GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
  if (file == INVALID_HANDLE_VALUE)
  {
    const bool taken = GetLastError() == ERROR_FILE_EXISTS;
    error = lastError();
    return taken ? Written::NameTaken : Written::Failed;
  }
  bool ok = true;
  while (ok && !contents.empty())
  {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(contents.size(), 1u << 30));
    DWORD written = 0;
    ok = WriteFile(file, contents.data(), chunk, &written, nullptr) != 0;
    contents.remove_prefix(written);
  }
  ok = ok && FlushFileBuffers(file) != 0;
  if (!ok)
    error = lastError();
  CloseHandle(file);
  if (!ok)
    DeleteFileW(temporary.wstring().c_str());
  return ok ? Written::Ok : Written::Failed;
}

bool replace(const fs::path& temporary,
             const fs::path& target,
             std::string& error)
{
  // Another program may have the file open for a moment (a virus scanner,
  // the search indexer, an editor watching it): try again, briefly.
  for (int attempt = 0; attempt < 20; ++attempt)
  {
    if (MoveFileExW(temporary.wstring().c_str(),
                    target.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      return true;
    const DWORD code = GetLastError();
    if (code != ERROR_ACCESS_DENIED && code != ERROR_SHARING_VIOLATION &&
        code != ERROR_LOCK_VIOLATION)
      break;
    Sleep(25);
  }
  error = lastError();
  return false;
}

#else

Written writeTemporary(const fs::path& temporary,
                       const fs::path& target,
                       std::string_view contents,
                       std::string& error)
{
  const int file =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (file < 0)
  {
    const bool taken = errno == EEXIST;
    error = std::strerror(errno);
    return taken ? Written::NameTaken : Written::Failed;
  }
  // The file keeps its permissions (an executable script stays executable).
  struct stat existing
  {
  };
  if (::stat(target.c_str(), &existing) == 0)
    (void)::fchmod(file, existing.st_mode & 07777);

  bool ok = true;
  while (ok && !contents.empty())
  {
    const ssize_t written = ::write(file, contents.data(), contents.size());
    if (written < 0 && errno == EINTR)
      continue;
    ok = written > 0;
    if (ok)
      contents.remove_prefix(static_cast<std::size_t>(written));
  }
  if (!ok)
    error = std::strerror(errno ? errno : EIO);
  if (ok && ::fsync(file) != 0)
  {
    error = std::strerror(errno);
    ok = false;
  }
  if (::close(file) != 0 && ok)
  {
    error = std::strerror(errno);
    ok = false;
  }
  if (!ok)
    ::unlink(temporary.c_str());
  return ok ? Written::Ok : Written::Failed;
}

bool replace(const fs::path& temporary,
             const fs::path& target,
             std::string& error)
{
  if (::rename(temporary.c_str(), target.c_str()) != 0)
  {
    error = std::strerror(errno);
    return false;
  }
  // The rename itself reaches the disk once the folder is flushed. (Best
  // effort: not every filesystem lets a folder be opened for it.)
  const fs::path folder =
      target.parent_path().empty() ? fs::path(".") : target.parent_path();
  const int directory = ::open(folder.c_str(), O_RDONLY | O_CLOEXEC);
  if (directory >= 0)
  {
    (void)::fsync(directory);
    ::close(directory);
  }
  return true;
}

#endif
} // namespace

bool writeFileAtomically(const fs::path& path, std::string_view contents)
{
  // A link is followed, so it stays a link to the rewritten file.
  std::error_code ignored;
  fs::path target = path;
  if (fs::is_symlink(path, ignored))
  {
    const fs::path resolved = fs::canonical(path, ignored);
    if (!ignored)
      target = resolved;
  }

  std::string error;
  fs::path temporary;
  Written written = Written::NameTaken;
  for (int attempt = 0; attempt < 5 && written == Written::NameTaken; ++attempt)
  {
    temporary = temporaryPathFor(target);
    written = writeTemporary(temporary, target, contents, error);
  }
  if (written != Written::Ok)
  {
    fail(path, error);
    return false;
  }
  if (!replace(temporary, target, error))
  {
    fs::remove(temporary, ignored);
    fail(path, error);
    return false;
  }
  return true;
}

} // namespace sfs
