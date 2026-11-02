#include "OwnedChildProcess.h"
#include "TerritoryProcess.h"
#include <cerrno>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <linux/close_range.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef _WIN32
namespace {
struct Handle {
  HANDLE value{};
  Handle() = default;
  explicit Handle(HANDLE h) : value(h) {}
  ~Handle() { reset(); }
  void reset(HANDLE h = nullptr) {
    if (value && value != INVALID_HANDLE_VALUE)
      CloseHandle(value);
    value = h;
  }
  Handle(const Handle &) = delete;
};
[[noreturn]] void fail(const char *what, DWORD error = GetLastError()) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          what);
}
} // namespace
struct OwnedChildProcess::Impl {
  // Destroy job first: descendants cannot outlive this owner.
  Handle process, job;
};
#else
struct OwnedChildProcess::Impl {
  pid_t pid{-1};
  std::optional<int> code;
  // Reaps the child once; blocking mode is for the owner destructor.
  std::optional<int> collect(bool blocking) {
    if (code)
      return code;
    if (pid < 0)
      return std::nullopt;
    int status = 0;
    const auto reaped = waitpid(pid, &status, blocking ? 0 : WNOHANG);
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
  // The child owns a session, so its group covers descendants; ESRCH means
  // the tree is already gone.
  void terminate_tree() noexcept {
    if (pid > 0 && !code)
      kill(-pid, SIGKILL);
  }
};
#endif
OwnedChildProcess::OwnedChildProcess() : m_impl(std::make_unique<Impl>()) {}
#ifdef _WIN32
OwnedChildProcess::~OwnedChildProcess() = default;
#else
OwnedChildProcess::~OwnedChildProcess() {
  // Mirror the Windows KILL_ON_JOB_CLOSE job: descendants cannot outlive
  // this owner.
  m_impl->terminate_tree();
  try {
    while (m_impl->pid > 0 && !m_impl->collect(true))
      ;
  } catch (...) {
  }
}
#endif
void OwnedChildProcess::start(const std::filesystem::path &exe,
                              const std::vector<std::wstring> &arguments,
                              const std::filesystem::path &log) {
#ifdef _WIN32
  if (m_impl->process.value && !exit_code())
    throw std::logic_error("Owned child is already running");
  if (!exe.is_absolute() || !log.is_absolute())
    throw std::invalid_argument("Owned child paths must be absolute");
  auto candidate = std::make_unique<Impl>();
  candidate->job.reset(CreateJobObjectW(nullptr, nullptr));
  if (!candidate->job.value)
    fail("CreateJobObject");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(candidate->job.value,
                               JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits)))
    fail("SetInformationJobObject");
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  Handle output{CreateFileW(log.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, &security,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (output.value == INVALID_HANDLE_VALUE)
    fail("Create child log");
  Handle input{CreateFileW(L"NUL", GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                           OPEN_EXISTING, 0, nullptr)};
  if (input.value == INVALID_HANDLE_VALUE)
    fail("Create child stdin");
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<std::max_align_t> storage((bytes + sizeof(std::max_align_t) - 1) /
                                        sizeof(std::max_align_t));
  auto *attributes =
      reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
    fail("Initialize process attributes");
  struct Attributes {
    LPPROC_THREAD_ATTRIBUTE_LIST value;
    ~Attributes() { DeleteProcThreadAttributeList(value); }
  } owned{attributes};
  HANDLE inherited[]{output.value, input.value};
  if (!UpdateProcThreadAttribute(attributes, 0,
                                 PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                 sizeof(inherited), nullptr, nullptr))
    fail("Set inherited handles");
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = attributes;
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = output.value;
  startup.StartupInfo.hStdError = output.value;
  std::wstring command = quote_windows_argument(exe.wstring());
  for (const auto &arg : arguments) {
    if (arg.find(L'\0') != std::wstring::npos)
      throw std::invalid_argument("NUL in child argument");
    command += L" " + quote_windows_argument(arg);
  }
  if (command.size() >= 32767)
    throw std::invalid_argument("Child command line exceeds Windows limit");
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(
          exe.c_str(), command.data(), nullptr, nullptr, TRUE,
          CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
          nullptr, log.parent_path().c_str(), &startup.StartupInfo, &child))
    fail("Create owned child");
  candidate->process.reset(child.hProcess);
  Handle thread{child.hThread};
  if (!AssignProcessToJobObject(candidate->job.value, child.hProcess)) {
    const auto error = GetLastError();
    TerminateProcess(child.hProcess, 3);
    fail("Assign owned child", error);
  }
  if (ResumeThread(child.hThread) == DWORD(-1))
    fail("Resume owned child");
  m_impl = std::move(candidate);
#else
  if (m_impl->pid > 0 && !exit_code())
    throw std::logic_error("Owned child is already running");
  if (!exe.is_absolute() || !log.is_absolute())
    throw std::invalid_argument("Owned child paths must be absolute");
  for (const auto &argument : arguments)
    if (argument.find(L'\0') != std::wstring::npos)
      throw std::invalid_argument("NUL in child argument");
  std::string executable = exe.string();
  std::string working = log.parent_path().string();
  // Everything the child touches is built before fork: after fork only
  // async-signal-safe calls happen there.
  std::vector<std::string> utf8_arguments;
  utf8_arguments.reserve(arguments.size());
  for (const auto &argument : arguments)
    utf8_arguments.push_back(wide_to_utf8(argument));
  std::vector<char *> pointers;
  pointers.reserve(utf8_arguments.size() + 2);
  pointers.push_back(executable.data());
  for (auto &argument : utf8_arguments)
    pointers.push_back(argument.data());
  pointers.push_back(nullptr);
  const int log_file = open(log.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (log_file < 0)
    throw std::system_error(errno, std::generic_category(), "Create child log");
  const int null_file = open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (null_file < 0) {
    const int error = errno;
    close(log_file);
    throw std::system_error(error, std::generic_category(), "Create child stdin");
  }
  int report[2];
  if (pipe2(report, O_CLOEXEC) != 0) {
    const int error = errno;
    close(log_file);
    close(null_file);
    throw std::system_error(error, std::generic_category(), "pipe");
  }
  const pid_t child = fork();
  if (child < 0) {
    const int error = errno;
    close(log_file);
    close(null_file);
    close(report[0]);
    close(report[1]);
    throw std::system_error(error, std::generic_category(), "fork");
  }
  if (child == 0) {
    close(report[0]);
    auto fail_child = [write_end = report[1]](int error) {
      ssize_t ignored = write(write_end, &error, sizeof(error));
      (void)ignored;
      _exit(127);
    };
    // A new session makes the child a process group leader, so
    // terminate_tree_owned() can kill the whole tree with one signal.
    if (setsid() < 0)
      fail_child(errno);
    if (chdir(working.c_str()) != 0)
      fail_child(errno);
    if (dup2(null_file, STDIN_FILENO) < 0 ||
        dup2(log_file, STDOUT_FILENO) < 0 ||
        dup2(log_file, STDERR_FILENO) < 0)
      fail_child(errno);
    // Mark the owner's descriptors exec-close; on success the exec also
    // clears the report pipe, on failure it stays readable below.
    if (close_range(3, ~0U, CLOSE_RANGE_CLOEXEC) != 0 && errno != ENOSYS &&
        errno != EINVAL)
      fail_child(errno);
    execv(executable.c_str(), pointers.data());
    fail_child(errno);
  }
  close(report[1]);
  close(log_file);
  close(null_file);
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
                            "Create owned child");
  }
  auto replacement = std::make_unique<Impl>();
  replacement->pid = child;
  m_impl = std::move(replacement);
#endif
}
auto OwnedChildProcess::exit_code() -> std::optional<int> {
#ifdef _WIN32
  if (!m_impl->process.value)
    throw std::logic_error("No owned child");
  const auto wait = WaitForSingleObject(m_impl->process.value, 0);
  if (wait == WAIT_TIMEOUT)
    return std::nullopt;
  if (wait != WAIT_OBJECT_0)
    fail("Wait for owned child");
  DWORD code{};
  if (!GetExitCodeProcess(m_impl->process.value, &code))
    fail("Get owned child exit code");
  return static_cast<int>(code);
#else
  if (m_impl->pid < 0)
    throw std::logic_error("No owned child");
  return m_impl->collect(false);
#endif
}
void OwnedChildProcess::terminate_tree_owned() {
#ifdef _WIN32
  if (m_impl->job.value && !TerminateJobObject(m_impl->job.value, 130))
    fail("Terminate owned child tree");
#else
  m_impl->terminate_tree();
#endif
}
