#include "TerritoryProcess.h"
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
std::wstring quote_windows_argument(std::wstring_view argument) {
  std::wstring result = L"\"";
  std::size_t slashes = 0;
  for (wchar_t c : argument) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (c == L'"')
      result += L'\\';
    result += c;
  }
  result.append(slashes * 2, L'\\');
  result += L'"';
  return result;
}
std::string wide_to_utf8(std::wstring_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size();) {
    char32_t point = static_cast<char32_t>(value[index]);
    ++index;
    if constexpr (sizeof(wchar_t) == 2) {
      // Combine a valid UTF-16 surrogate pair; unpaired surrogates fall
      // through and encode as their own value, mirroring the input garbage.
      if (point >= 0xD800 && point <= 0xDBFF && index < value.size() &&
          value[index] >= 0xDC00 && value[index] <= 0xDFFF) {
        point = 0x10000 + ((point - 0xD800) << 10) +
                (static_cast<char32_t>(value[index]) - 0xDC00);
        ++index;
      }
    }
    if (point < 0x80) {
      result.push_back(static_cast<char>(point));
    } else if (point < 0x800) {
      result.push_back(static_cast<char>(0xC0 | (point >> 6)));
      result.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else if (point < 0x10000) {
      result.push_back(static_cast<char>(0xE0 | (point >> 12)));
      result.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    } else {
      result.push_back(static_cast<char>(0xF0 | (point >> 18)));
      result.push_back(static_cast<char>(0x80 | ((point >> 12) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | ((point >> 6) & 0x3F)));
      result.push_back(static_cast<char>(0x80 | (point & 0x3F)));
    }
  }
  return result;
}
namespace {
#ifdef _WIN32
class WindowsProcess final : public TerritoryProcess {
public:
  explicit WindowsProcess(std::wstring option) : job_option(std::move(option)) {}
  ~WindowsProcess() override { close(); }
  void start(const std::filesystem::path &exe,
             const std::filesystem::path &directory) override {
    if (process && !exit_code())
      throw std::logic_error("Territory worker is already running");
    close();
    if (!exe.is_absolute() || !directory.is_absolute())
      throw std::invalid_argument("Worker paths must be absolute");
    job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
      fail("CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
      fail("SetInformationJobObject");
    auto command = quote_windows_argument(exe.wstring()) + L" " + job_option + L" " +
                   quote_windows_argument(directory.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        exe.parent_path().c_str(), &startup, &child))
      fail("CreateProcess");
    process = child.hProcess;
    if (!AssignProcessToJobObject(job, process)) {
      const auto error = GetLastError();
      TerminateProcess(process, 3);
      CloseHandle(child.hThread);
      fail("AssignProcessToJobObject", error);
    }
    if (ResumeThread(child.hThread) == DWORD(-1)) {
      const auto error = GetLastError();
      CloseHandle(child.hThread);
      fail("ResumeThread", error);
    }
    CloseHandle(child.hThread);
  }
  std::optional<int> exit_code() override {
    if (!process)
      throw std::logic_error("No territory process");
    const auto state = WaitForSingleObject(process, 0);
    if (state == WAIT_TIMEOUT)
      return std::nullopt;
    if (state != WAIT_OBJECT_0)
      throw std::system_error(GetLastError(), std::system_category(),
                              "WaitForSingleObject");
    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code))
      throw std::system_error(GetLastError(), std::system_category(),
                              "GetExitCodeProcess");
    return static_cast<int>(code);
  }
  void terminate_owned() override {
    if (job && !TerminateJobObject(job, 130))
      throw std::system_error(GetLastError(), std::system_category(),
                              "TerminateJobObject");
  }

private:
  std::wstring job_option;
  HANDLE process{}, job{};
  void close() noexcept {
    if (job) {
      CloseHandle(job);
      job = nullptr;
    }
    if (process) {
      CloseHandle(process);
      process = nullptr;
    }
  }
  [[noreturn]] void fail(const char *operation, DWORD error = GetLastError()) {
    close();
    throw std::system_error(error, std::system_category(), operation);
  }
};
#else
class WorkerProcess final : public TerritoryProcess {
public:
  explicit WorkerProcess(std::wstring option)
      : job_option(wide_to_utf8(option)) {}
  ~WorkerProcess() override { shutdown(); }
  void start(const std::filesystem::path &exe,
             const std::filesystem::path &directory) override {
    if (pid > 0 && !exit_code())
      throw std::logic_error("Territory worker is already running");
    shutdown();
    if (!exe.is_absolute() || !directory.is_absolute())
      throw std::invalid_argument("Worker paths must be absolute");
    std::string executable = exe.string();
    std::string job = directory.string();
    std::string working = exe.parent_path().string();
    int report[2];
    if (pipe2(report, O_CLOEXEC) != 0)
      throw std::system_error(errno, std::generic_category(), "pipe");
    const pid_t child = fork();
    if (child < 0) {
      const int error = errno;
      close(report[0]);
      close(report[1]);
      throw std::system_error(error, std::generic_category(), "fork");
    }
    if (child == 0) {
      close(report[0]);
      auto fail_child = [write_end = report[1]](int error) {
        ssize_t ignored = write(write_end, &error, sizeof(error));
        (void)ignored;
        _exit(126);
      };
      // A new session makes the worker a process group leader, so
      // terminate_owned() can kill the whole tree with one signal.
      if (setsid() < 0)
        fail_child(errno);
      if (chdir(working.c_str()) != 0)
        fail_child(errno);
      char *arguments[] = {executable.data(), job_option.data(), job.data(),
                           nullptr};
      execv(executable.c_str(), arguments);
      fail_child(errno);
    }
    close(report[1]);
    int failure = 0;
    ssize_t got;
    while ((got = read(report[0], &failure, sizeof(failure))) == -1 &&
           errno == EINTR) {
    }
    close(report[0]);
    if (got == static_cast<ssize_t>(sizeof(failure))) {
      int status = 0;
      while (waitpid(child, &status, 0) == -1 && errno == EINTR) {
      }
      throw std::system_error(failure, std::generic_category(),
                              "Start territory worker");
    }
    pid = child;
  }
  std::optional<int> exit_code() override {
    if (code)
      return code;
    if (pid < 0)
      throw std::logic_error("No territory process");
    return wait_status();
  }
  void terminate_owned() override {
    // Killing the group covers descendants; ESRCH means the tree is gone.
    if (pid > 0 && !code)
      kill(-pid, SIGKILL);
  }

private:
  std::string job_option;
  pid_t pid{-1};
  std::optional<int> code;
  std::optional<int> wait_status() {
    int status = 0;
    const auto reaped = waitpid(pid, &status, WNOHANG);
    if (reaped == 0)
      return std::nullopt;
    if (reaped < 0)
      throw std::system_error(errno, std::generic_category(), "waitpid");
    if (WIFEXITED(status))
      code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
      code = 128 + WTERMSIG(status);
    else
      code = 1;
    return code;
  }
  // Mirror the Windows KILL_ON_JOB_CLOSE job: descendants cannot outlive
  // this owner.
  void shutdown() noexcept {
    if (pid > 0 && !code) {
      kill(-pid, SIGKILL);
      int status = 0;
      while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
      }
    }
    pid = -1;
  }
};
#endif
} // namespace
std::unique_ptr<TerritoryProcess> make_territory_process(std::wstring option) {
  if (option != L"--render-job" && option != L"--geodata-job")
    throw std::invalid_argument("Unsupported worker mode");
#ifdef _WIN32
  return std::make_unique<WindowsProcess>(std::move(option));
#else
  return std::make_unique<WorkerProcess>(std::move(option));
#endif
}
