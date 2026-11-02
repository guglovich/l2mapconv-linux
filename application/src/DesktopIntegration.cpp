#include "DesktopIntegration.h"

#include <cstdlib>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <shellapi.h>
#else
namespace {
std::string shell_quote(const std::string &value) {
  std::string result = "'";
  for (char c : value)
    result += c == '\'' ? "'\\''" : std::string(1, c);
  result += "'";
  return result;
}
} // namespace
#endif

bool open_path_externally(const std::filesystem::path &path) {
#ifdef _WIN32
  return reinterpret_cast<std::intptr_t>(ShellExecuteW(
             nullptr, L"open", path.c_str(), nullptr, nullptr,
             SW_SHOWNORMAL)) > 32;
#else
  // Detached: xdg-open may stay alive while the file manager runs.
  const std::string command =
      "xdg-open " + shell_quote(path.string()) + " >/dev/null 2>&1 &";
  return std::system(command.c_str()) == 0;
#endif
}
