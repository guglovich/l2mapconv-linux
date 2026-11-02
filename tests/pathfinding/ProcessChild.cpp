#define NOMINMAX
#include "TerritoryProcess.h"
#include <filesystem>
#include <iostream>
#include <territory/PathIO.h>
#ifdef _WIN32
#include <windows.h>

int wmain(int argc, wchar_t **argv) try {
  if (argc < 3)
    return 2;
  const std::filesystem::path output = argv[2];
  if (std::wstring_view(argv[1]) == L"--echo") {
    auto args = territory::Json::array();
    for (int i = 3; i < argc; ++i)
      args.push_back(territory::path_utf8(std::filesystem::path{argv[i]}));
    territory::write_json_atomic(output, args, false);
    std::cout << "child log marker\n";
    return 7;
  }
  if (std::wstring_view(argv[1]) == L"--hold") {
    territory::write_json_atomic(output, {{"pid", GetCurrentProcessId()}},
                                 false);
    Sleep(30000);
    return 0;
  }
  if (std::wstring_view(argv[1]) == L"--tree") {
    wchar_t self[32768]{};
    GetModuleFileNameW(nullptr, self, 32768);
    auto command = quote_windows_argument(self) + L" --hold " +
                   quote_windows_argument(output.wstring());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
      return 3;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Sleep(30000);
    return 0;
  }
  return 2;
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 3;
}
#else
#include <signal.h>
#include <string_view>
#include <unistd.h>

int main(int argc, char **argv) try {
  if (argc < 3)
    return 2;
  const std::filesystem::path output = argv[2];
  if (std::string_view(argv[1]) == "--echo") {
    auto args = territory::Json::array();
    for (int i = 3; i < argc; ++i)
      args.push_back(territory::path_utf8(std::filesystem::path{argv[i]}));
    territory::write_json_atomic(output, args, false);
    std::cout << "child log marker\n";
    return 7;
  }
  if (std::string_view(argv[1]) == "--hold") {
    territory::write_json_atomic(output, {{"pid", static_cast<int>(getpid())}},
                                 false);
    sleep(30);
    return 0;
  }
  if (std::string_view(argv[1]) == "--tree") {
    // The parent session was created by OwnedChildProcess; keep the grandchild
    // in it so that terminate_tree_owned() covers both.
    const pid_t pid = fork();
    if (pid < 0)
      return 3;
    if (pid == 0) {
      const char *arguments[] = {argv[0], "--hold", argv[2], nullptr};
      execv("/proc/self/exe", const_cast<char *const *>(arguments));
      _exit(3);
    }
    sleep(30);
    return 0;
  }
  return 2;
} catch (const std::exception &e) {
  std::cerr << e.what() << '\n';
  return 3;
}
#endif
