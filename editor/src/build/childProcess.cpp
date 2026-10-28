#include "build/childProcess.h"

#include "engine/core/logger/logger.h"

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <csignal>
  #include <fcntl.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

bool ChildProcess::start(const std::vector<std::string>& args, OutputHandler onStdout)
{
  return start(args, std::move(onStdout), Options{});
}

void LineSplitter::feed(std::string_view chunk)
{
  for (const char c : chunk)
  {
    if (c == '\n')
    {
      m_onLine(m_pending);
      m_pending.clear();
    }
    else if (c != '\r')
      m_pending += c;
  }
}

void LineSplitter::flush()
{
  if (!m_pending.empty())
    m_onLine(m_pending);
  m_pending.clear();
}

#if defined(_WIN32)

struct ChildProcess::Platform
{
  HANDLE process = nullptr;
  HANDLE job = nullptr; // it and its children
  HANDLE stdinWrite = nullptr;
  std::vector<HANDLE> outputs; // read ends: stdout, then stderr if separate
};

ChildProcess::ChildProcess() = default;

namespace
{
// Quote one argument for CreateProcess's command line (MSVC runtime rules).
std::string quoteArgument(const std::string& arg)
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
    if (c == '"')
      out.append(backslashes * 2 + 1, '\\');
    else
      out.append(backslashes, '\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, '\\');
  return out + '"';
}

// A pipe whose `childEnd` is inherited by the child and whose other end isn't.
bool makePipe(HANDLE& read, HANDLE& write, bool childWrites)
{
  SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
  if (!CreatePipe(&read, &write, &security, 0))
    return false;
  SetHandleInformation(childWrites ? read : write, HANDLE_FLAG_INHERIT, 0);
  return true;
}

void readUntilClosed(HANDLE pipe, const ChildProcess::OutputHandler& onOutput)
{
  char buffer[8192];
  DWORD read = 0;
  while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
    onOutput(std::string_view(buffer, read));
}
} // namespace

bool ChildProcess::start(const std::vector<std::string>& args,
                         OutputHandler onStdout,
                         Options options)
{
  HANDLE outRead = nullptr, outWrite = nullptr;
  HANDLE errRead = nullptr, errWrite = nullptr;
  HANDLE inRead = nullptr, inWrite = nullptr;
  const bool separateStderr = static_cast<bool>(options.onStderr);

  if (!makePipe(outRead, outWrite, true) ||
      (separateStderr && !makePipe(errRead, errWrite, true)) ||
      (options.pipeStdin && !makePipe(inRead, inWrite, false)))
  {
    LOG_ERROR("Can't create pipes for " + args.front());
    return false;
  }

  std::string commandLine;
  for (const std::string& arg : args)
    commandLine += (commandLine.empty() ? "" : " ") + quoteArgument(arg);

  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = outWrite;
  startup.hStdError = separateStderr ? errWrite : outWrite;
  startup.hStdInput = options.pipeStdin ? inRead : GetStdHandle(STD_INPUT_HANDLE);

  // In a job of its own (started suspended, so it's in before it can start
  // anything), so terminate() takes its children too, and closing the job
  // when the editor exits does.
  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  if (job)
  {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  }

  PROCESS_INFORMATION info = {};
  const BOOL created = CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &info);
  if (created)
  {
    if (job && !AssignProcessToJobObject(job, info.hProcess))
    {
      CloseHandle(job);
      job = nullptr;
    }
    ResumeThread(info.hThread);
  }

  // The child holds its own copies of its ends.
  CloseHandle(outWrite);
  if (errWrite)
    CloseHandle(errWrite);
  if (inRead)
    CloseHandle(inRead);

  if (!created)
  {
    CloseHandle(outRead);
    if (errRead)
      CloseHandle(errRead);
    if (inWrite)
      CloseHandle(inWrite);
    if (job)
      CloseHandle(job);
    LOG_ERROR("Can't start " + args.front());
    return false;
  }
  CloseHandle(info.hThread);

  m_platform = std::make_unique<Platform>();
  m_platform->process = info.hProcess;
  m_platform->job = job;
  m_platform->stdinWrite = inWrite;
  m_platform->outputs.push_back(outRead);
  m_readers.emplace_back(readUntilClosed, outRead, std::move(onStdout));
  if (separateStderr)
  {
    m_platform->outputs.push_back(errRead);
    m_readers.emplace_back(readUntilClosed, errRead, std::move(options.onStderr));
  }
  m_started = true;
  return true;
}

bool ChildProcess::write(std::string_view data)
{
  const std::lock_guard lock(m_writeMutex);
  if (!m_started || !m_platform->stdinWrite)
    return false;

  while (!data.empty())
  {
    DWORD written = 0;
    if (!WriteFile(m_platform->stdinWrite, data.data(), static_cast<DWORD>(data.size()),
                   &written, nullptr))
      return false;
    data.remove_prefix(written);
  }
  return true;
}

bool ChildProcess::running()
{
  if (!m_started)
    return false;
  if (WaitForSingleObject(m_platform->process, 0) == WAIT_TIMEOUT)
    return true;
  finish();
  return false;
}

void ChildProcess::finish()
{
  if (!m_started)
    return;
  if (m_platform->stdinWrite)
  {
    CloseHandle(m_platform->stdinWrite);
    m_platform->stdinWrite = nullptr;
  }
  WaitForSingleObject(m_platform->process, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(m_platform->process, &code);
  m_exitCode = static_cast<int>(code);
  for (std::thread& reader : m_readers)
    if (reader.joinable())
      reader.join(); // pipes reach EOF once the child exits
  m_readers.clear();
  CloseHandle(m_platform->process);
  if (m_platform->job)
    CloseHandle(m_platform->job);
  for (HANDLE output : m_platform->outputs)
    CloseHandle(output);
  m_started = false;
}

void ChildProcess::terminate()
{
  if (!m_started)
    return;
  if (m_platform->job)
    TerminateJobObject(m_platform->job, 1);
  else if (WaitForSingleObject(m_platform->process, 0) == WAIT_TIMEOUT)
    TerminateProcess(m_platform->process, 1);
  finish();
}

ChildProcess::~ChildProcess() { terminate(); }

#else // POSIX

struct ChildProcess::Platform
{
  pid_t pid = -1;
  int stdinWrite = -1;
  std::vector<int> outputs; // read ends: stdout, then stderr if separate
};

ChildProcess::ChildProcess() = default;

namespace
{
void readUntilClosed(int pipe, const ChildProcess::OutputHandler& onOutput)
{
  char buffer[8192];
  ssize_t read = 0;
  while ((read = ::read(pipe, buffer, sizeof(buffer))) > 0)
    onOutput(std::string_view(buffer, static_cast<std::size_t>(read)));
}
} // namespace

bool ChildProcess::start(const std::vector<std::string>& args,
                         OutputHandler onStdout,
                         Options options)
{
  const bool separateStderr = static_cast<bool>(options.onStderr);
  int out[2] = {-1, -1}, err[2] = {-1, -1}, in[2] = {-1, -1};
  if (pipe(out) != 0 || (separateStderr && pipe(err) != 0) ||
      (options.pipeStdin && pipe(in) != 0))
  {
    LOG_ERROR("Can't create pipes for " + args.front());
    return false;
  }

  std::vector<char*> argv;
  for (const std::string& arg : args)
    argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);

  const pid_t pid = fork();
  if (pid == 0)
  {
    setpgid(0, 0); // a group of its own, for terminate()
    dup2(out[1], STDOUT_FILENO);
    dup2(separateStderr ? err[1] : out[1], STDERR_FILENO);
    if (options.pipeStdin)
      dup2(in[0], STDIN_FILENO);
    for (const int fd : {out[0], out[1], err[0], err[1], in[0], in[1]})
      if (fd >= 0)
        close(fd);
    execvp(argv[0], argv.data());
    _exit(127); // exec failed
  }

  close(out[1]);
  if (err[1] >= 0)
    close(err[1]);
  if (in[0] >= 0)
    close(in[0]);
  if (pid < 0)
  {
    for (const int fd : {out[0], err[0], in[1]})
      if (fd >= 0)
        close(fd);
    LOG_ERROR("Can't start " + args.front());
    return false;
  }

  setpgid(pid, pid); // as the child does: whichever runs first
  m_platform = std::make_unique<Platform>();
  m_platform->pid = pid;
  m_platform->stdinWrite = in[1];
  m_platform->outputs.push_back(out[0]);
  m_readers.emplace_back(readUntilClosed, out[0], std::move(onStdout));
  if (separateStderr)
  {
    m_platform->outputs.push_back(err[0]);
    m_readers.emplace_back(readUntilClosed, err[0], std::move(options.onStderr));
  }
  m_started = true;
  return true;
}

bool ChildProcess::write(std::string_view data)
{
  const std::lock_guard lock(m_writeMutex);
  if (!m_started || m_platform->stdinWrite < 0)
    return false;

  while (!data.empty())
  {
    const ssize_t written = ::write(m_platform->stdinWrite, data.data(), data.size());
    if (written <= 0)
      return false;
    data.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}

bool ChildProcess::running()
{
  if (!m_started)
    return false;
  int status = 0;
  if (waitpid(m_platform->pid, &status, WNOHANG) == 0)
    return true;
  m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  m_platform->pid = -1; // reaped
  finish();
  return false;
}

void ChildProcess::finish()
{
  if (!m_started)
    return;
  if (m_platform->stdinWrite >= 0)
  {
    close(m_platform->stdinWrite);
    m_platform->stdinWrite = -1;
  }
  if (m_platform->pid > 0)
  {
    int status = 0;
    waitpid(m_platform->pid, &status, 0);
    m_exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }
  for (std::thread& reader : m_readers)
    if (reader.joinable())
      reader.join();
  m_readers.clear();
  for (const int fd : m_platform->outputs)
    close(fd);
  m_started = false;
}

void ChildProcess::terminate()
{
  if (!m_started)
    return;
  if (m_platform->pid > 0)
    kill(-m_platform->pid, SIGTERM); // its whole group
  finish();
}

ChildProcess::~ChildProcess()
{
  terminate();
}

#endif
