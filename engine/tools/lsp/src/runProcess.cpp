#include "runProcess.h"

#include <algorithm>
#include <initializer_list>
#include <thread>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <cerrno>
  #include <csignal>
  #include <fcntl.h>
  #include <poll.h>
  #include <pthread.h>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>

extern char** environ;
#endif

namespace sfs::lsp
{

#if defined(_WIN32)

namespace
{

// One argument quoted for CreateProcess's command line (the MSVC runtime's
// rules).
std::string quoted(const std::string& arg)
{
  if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos)
    return arg;

  std::string out = "\"";
  std::size_t backslashes = 0;

  for (const char c : arg)
  {
    if (c == '\\')
    {
      ++backslashes;
      continue;
    }

    out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
    backslashes = 0;
    out += c;
  }

  out.append(backslashes * 2, '\\');
  return out + '"';
}

// A pipe whose child end is inherited and whose own end isn't.
bool makePipe(HANDLE& read, HANDLE& write, bool childWrites)
{
  SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};

  if (!CreatePipe(&read, &write, &security, 0))
    return false;

  SetHandleInformation(childWrites ? read : write, HANDLE_FLAG_INHERIT, 0);
  return true;
}

void readAll(HANDLE pipe, std::string& into)
{
  char buffer[8192];
  DWORD count = 0;

  while (ReadFile(pipe, buffer, sizeof(buffer), &count, nullptr) && count > 0)
    into.append(buffer, count);
}

void closeAll(std::initializer_list<HANDLE> handles)
{
  for (HANDLE handle : handles)
    if (handle)
      CloseHandle(handle);
}

} // namespace

ProcessResult runProcess(const std::vector<std::string>& args,
                         std::string_view input,
                         std::chrono::milliseconds limit)
{
  ProcessResult result;

  if (args.empty())
    return result;

  HANDLE outRead = nullptr, outWrite = nullptr;
  HANDLE errRead = nullptr, errWrite = nullptr;
  HANDLE inRead = nullptr, inWrite = nullptr;

  if (!makePipe(outRead, outWrite, true) ||
      !makePipe(errRead, errWrite, true) || !makePipe(inRead, inWrite, false))
  {
    closeAll({outRead, outWrite, errRead, errWrite, inRead, inWrite});
    return result;
  }

  std::string commandLine;

  for (const std::string& arg : args)
    commandLine += (commandLine.empty() ? "" : " ") + quoted(arg);

  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = outWrite;
  startup.hStdError = errWrite;
  startup.hStdInput = inRead;
  PROCESS_INFORMATION info = {};
  const BOOL created = CreateProcessA(nullptr,
                                      commandLine.data(),
                                      nullptr,
                                      nullptr,
                                      TRUE,
                                      CREATE_NO_WINDOW,
                                      nullptr,
                                      nullptr,
                                      &startup,
                                      &info);
  // The child holds its own copies of its ends.
  closeAll({outWrite, errWrite, inRead});

  if (!created)
  {
    closeAll({outRead, errRead, inWrite});
    return result;
  }

  CloseHandle(info.hThread);
  result.started = true;

  std::thread writer(
      [inWrite, input]
      {
        std::string_view rest = input;

        while (!rest.empty())
        {
          DWORD written = 0;
          const DWORD chunk =
              static_cast<DWORD>(std::min<std::size_t>(rest.size(), 1 << 20));

          if (!WriteFile(inWrite, rest.data(), chunk, &written, nullptr))
            break;

          rest.remove_prefix(written);
        }

        CloseHandle(inWrite);
      });
  std::thread errorReader([errRead, &result]
                          { readAll(errRead, result.errors); });
  std::thread outputReader([outRead, &result]
                           { readAll(outRead, result.output); });

  if (WaitForSingleObject(info.hProcess, static_cast<DWORD>(limit.count())) !=
      WAIT_OBJECT_0)
  {
    TerminateProcess(info.hProcess, 1);
    WaitForSingleObject(info.hProcess, INFINITE);
    result.timedOut = true;
  }

  writer.join();
  errorReader.join();
  outputReader.join();
  DWORD code = 0;

  if (!result.timedOut && GetExitCodeProcess(info.hProcess, &code))
    result.exitCode = static_cast<int>(code);

  closeAll({info.hProcess, outRead, errRead});
  return result;
}

#else

namespace
{

// A pipe that isn't inherited past exec (posix_spawn's dup2 clears that on the
// child's copy).
bool makePipe(int ends[2])
{
  if (::pipe(ends) != 0)
    return false;

  for (int i = 0; i < 2; ++i)
    ::fcntl(ends[i], F_SETFD, FD_CLOEXEC);

  return true;
}

void closeAll(std::initializer_list<int> fds)
{
  for (int fd : fds)
    if (fd >= 0)
      ::close(fd);
}

} // namespace

ProcessResult runProcess(const std::vector<std::string>& args,
                         std::string_view input,
                         std::chrono::milliseconds limit)
{
  ProcessResult result;

  if (args.empty())
    return result;

  int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};

  if (!makePipe(in) || !makePipe(out) || !makePipe(err))
  {
    closeAll({in[0], in[1], out[0], out[1], err[0], err[1]});
    return result;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, in[0], 0);
  posix_spawn_file_actions_adddup2(&actions, out[1], 1);
  posix_spawn_file_actions_adddup2(&actions, err[1], 2);
  // The child starts with no signal blocked and SIGPIPE at its default,
  // whatever this thread has.
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  sigset_t none;
  sigemptyset(&none);
  sigset_t pipeSignal;
  sigemptyset(&pipeSignal);
  sigaddset(&pipeSignal, SIGPIPE);
  posix_spawnattr_setsigmask(&attributes, &none);
  posix_spawnattr_setsigdefault(&attributes, &pipeSignal);
  posix_spawnattr_setflags(
      &attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);

  for (const std::string& arg : args)
    argv.push_back(const_cast<char*>(arg.c_str()));

  argv.push_back(nullptr);
  pid_t pid = -1;
  const int spawned =
      posix_spawnp(&pid, argv[0], &actions, &attributes, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attributes);
  closeAll({in[0], out[1], err[1]});

  if (spawned != 0)
  {
    closeAll({in[1], out[0], err[0]});
    return result;
  }

  result.started = true;

  // A write to a child that has gone gives EPIPE here, not a SIGPIPE that
  // would end the server: the signal is blocked for this thread meanwhile, and
  // a pending one is taken before it's unblocked.
  sigset_t previous;
  pthread_sigmask(SIG_BLOCK, &pipeSignal, &previous);

  int writeEnd = in[1];
  ::fcntl(writeEnd, F_SETFL, ::fcntl(writeEnd, F_GETFL) | O_NONBLOCK);
  std::string_view rest = input;

  if (rest.empty())
  {
    ::close(writeEnd);
    writeEnd = -1;
  }

  int outputEnd = out[0];
  int errorEnd = err[0];
  const auto deadline = std::chrono::steady_clock::now() + limit;
  char buffer[65536];

  while (writeEnd >= 0 || outputEnd >= 0 || errorEnd >= 0)
  {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());

    if (left.count() <= 0)
    {
      ::kill(pid, SIGKILL);
      result.timedOut = true;
      break;
    }

    pollfd fds[3];
    nfds_t count = 0;

    if (writeEnd >= 0)
      fds[count++] = {writeEnd, POLLOUT, 0};

    if (outputEnd >= 0)
      fds[count++] = {outputEnd, POLLIN, 0};

    if (errorEnd >= 0)
      fds[count++] = {errorEnd, POLLIN, 0};

    const int ready = ::poll(fds, count, static_cast<int>(left.count()));

    if (ready < 0 && errno != EINTR)
      break;

    for (nfds_t i = 0; ready > 0 && i < count; ++i)
    {
      if (fds[i].revents == 0)
        continue;

      if (fds[i].fd == writeEnd)
      {
        const ssize_t written = ::write(writeEnd, rest.data(), rest.size());

        if (written > 0)
          rest.remove_prefix(static_cast<std::size_t>(written));

        if ((written < 0 && errno != EAGAIN && errno != EINTR) || rest.empty())
        {
          ::close(writeEnd);
          writeEnd = -1;
        }

        continue;
      }

      int& end = fds[i].fd == outputEnd ? outputEnd : errorEnd;
      std::string& into =
          fds[i].fd == outputEnd ? result.output : result.errors;
      const ssize_t got = ::read(end, buffer, sizeof(buffer));

      if (got > 0)
        into.append(buffer, static_cast<std::size_t>(got));
      else if (got == 0 || (errno != EAGAIN && errno != EINTR))
      {
        ::close(end);
        end = -1;
      }
    }
  }

  closeAll({writeEnd, outputEnd, errorEnd});

  int status = 0;

  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR)
  {
  }

  if (!result.timedOut && WIFEXITED(status))
    result.exitCode = WEXITSTATUS(status);

  sigset_t pending;
  sigpending(&pending);
  int taken = 0;

  if (sigismember(&pending, SIGPIPE))
    sigwait(&pipeSignal, &taken);

  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  return result;
}

#endif

} // namespace sfs::lsp
