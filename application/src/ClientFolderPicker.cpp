#include "ClientFolderPicker.h"

#include "TerritoryProcess.h"

#include <utils/Log.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#ifdef _WIN32
#include <ShObjIdl.h>
#include <Windows.h>

namespace {

class ComApartment {
public:
  ComApartment()
      : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)),
        m_uninitialize(SUCCEEDED(m_result)) {}

  ~ComApartment() {
    if (m_uninitialize) {
      CoUninitialize();
    }
  }

  auto ready() const -> bool {
    return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
  }

  auto result() const -> HRESULT { return m_result; }

private:
  HRESULT m_result;
  bool m_uninitialize;
};

template <typename T> class ComPointer {
public:
  explicit ComPointer(T *pointer = nullptr) : m_pointer(pointer) {}
  ~ComPointer() {
    if (m_pointer != nullptr) {
      m_pointer->Release();
    }
  }

  ComPointer(const ComPointer &) = delete;
  auto operator=(const ComPointer &) -> ComPointer & = delete;

  auto operator->() const -> T * { return m_pointer; }
  explicit operator bool() const { return m_pointer != nullptr; }

private:
  T *m_pointer;
};

void log_failure(std::string_view operation, HRESULT result) {
  utils::Log(utils::LOG_ERROR, "App")
      << operation << " failed with HRESULT "
      << static_cast<unsigned long>(result) << std::endl;
}

auto utf8_to_wide(std::string_view value) -> std::wstring {
  if (value.empty()) {
    return {};
  }
  const auto size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    return L"Unable to use the selected Lineage II client directory.";
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), size);
  return result;
}

} // namespace
#else
namespace {

std::string shell_quote(const std::string &value) {
  std::string result = "'";
  for (char c : value)
    result += c == '\'' ? "'\\''" : std::string(1, c);
  result += "'";
  return result;
}

bool helper_available(const char *name) {
  const std::string probe =
      std::string("command -v ") + name + " >/dev/null 2>&1";
  return std::system(probe.c_str()) == 0;
}

// Runs a picker helper and returns its first stdout line; empty output or a
// nonzero exit means the user cancelled or the helper failed. Helper
// diagnostics stay on our stderr.
std::optional<std::string> capture_first_line(const std::string &command) {
  FILE *process = ::popen(command.c_str(), "r");
  if (!process) {
    utils::Log(utils::LOG_ERROR, "App")
        << "Cannot run the file picker helper" << std::endl;
    return std::nullopt;
  }
  std::string line;
  std::array<char, 512> buffer{};
  while (line.size() < 4096 &&
         std::fgets(buffer.data(), static_cast<int>(buffer.size()), process)) {
    line += buffer.data();
    if (!line.empty() && line.back() == '\n')
      break;
  }
  const int status = ::pclose(process);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();
  if (status != 0 || line.empty())
    return std::nullopt;
  return line;
}

std::optional<std::string> helper_directory(const std::string &title) {
  const auto quoted = shell_quote(title);
  if (helper_available("zenity"))
    return capture_first_line("zenity --file-selection --directory --title=" +
                              quoted);
  if (helper_available("kdialog"))
    return capture_first_line("kdialog --getexistingdirectory $HOME --title " +
                              quoted);
  utils::Log(utils::LOG_ERROR, "App")
      << "No folder picker helper found; install zenity or kdialog"
      << std::endl;
  return std::nullopt;
}

std::optional<std::string> helper_file(const std::string &title, bool save_new,
                                       const std::string &extension) {
  const auto quoted = shell_quote(title);
  if (helper_available("zenity")) {
    std::string command = "zenity --file-selection --title=" + quoted;
    if (!extension.empty())
      command += " --file-filter=" +
                 shell_quote(extension + " files | *." + extension) +
                 " --file-filter=" + shell_quote("All files | *");
    if (save_new)
      command += " --save --confirm-overwrite";
    return capture_first_line(command);
  }
  if (helper_available("kdialog")) {
    const std::string filter =
        extension.empty() ? std::string("*|All files")
                          : "*." + extension + "|" + extension + " files";
    return capture_first_line(
        std::string(save_new ? "kdialog --getsavefilename $HOME "
                             : "kdialog --getopenfilename $HOME ") +
        shell_quote(filter) + " --title " + quoted);
  }
  utils::Log(utils::LOG_ERROR, "App")
      << "No file picker helper found; install zenity or kdialog" << std::endl;
  return std::nullopt;
}

} // namespace
#endif

auto choose_client_root_folder() -> std::optional<std::filesystem::path> {
  return choose_directory(L"Choose the Lineage II client sam directory");
}

auto choose_directory(std::wstring_view title) -> std::optional<std::filesystem::path> {
#ifdef _WIN32
  const ComApartment apartment;
  if (!apartment.ready()) {
    log_failure("CoInitializeEx", apartment.result());
    return std::nullopt;
  }

  IFileOpenDialog *raw_dialog = nullptr;
  auto result =
      CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(&raw_dialog));
  const ComPointer<IFileOpenDialog> dialog{raw_dialog};
  if (FAILED(result) || !dialog) {
    log_failure("CoCreateInstance(CLSID_FileOpenDialog)", result);
    return std::nullopt;
  }

  FILEOPENDIALOGOPTIONS options{};
  result = dialog->GetOptions(&options);
  if (FAILED(result)) {
    log_failure("IFileDialog::GetOptions", result);
    return std::nullopt;
  }
  result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  if (FAILED(result)) {
    log_failure("IFileDialog::SetOptions", result);
    return std::nullopt;
  }
  result = dialog->SetTitle(std::wstring(title).c_str());
  if (FAILED(result)) {
    log_failure("IFileDialog::SetTitle", result);
    return std::nullopt;
  }

  result = dialog->Show(GetActiveWindow());
  if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    return std::nullopt;
  }
  if (FAILED(result)) {
    log_failure("IFileDialog::Show", result);
    return std::nullopt;
  }

  IShellItem *raw_item = nullptr;
  result = dialog->GetResult(&raw_item);
  const ComPointer<IShellItem> item{raw_item};
  if (FAILED(result) || !item) {
    log_failure("IFileDialog::GetResult", result);
    return std::nullopt;
  }

  PWSTR raw_path = nullptr;
  result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path);
  if (FAILED(result) || raw_path == nullptr) {
    log_failure("IShellItem::GetDisplayName", result);
    return std::nullopt;
  }

  const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> selected_path{
      raw_path, &CoTaskMemFree};
  return std::filesystem::path{selected_path.get()};
#else
  const auto picked = helper_directory(wide_to_utf8(title));
  if (!picked)
    return std::nullopt;
  return std::filesystem::path{*picked};
#endif
}

void show_client_selection_error(std::string_view message) {
#ifdef _WIN32
  const auto wide_message = utf8_to_wide(message);
  MessageBoxW(GetActiveWindow(), wide_message.c_str(), L"l2mapconv",
              MB_OK | MB_ICONERROR | MB_TASKMODAL);
#else
  utils::Log(utils::LOG_ERROR, "App") << message << std::endl;
#endif
}

auto choose_file(std::wstring_view title, bool save_new,
                 std::wstring_view default_extension) -> std::optional<std::filesystem::path> {
#ifdef _WIN32
  const ComApartment apartment;
  if(!apartment.ready()) return {};
  IFileDialog *raw{};
  const auto clsid=save_new?CLSID_FileSaveDialog:CLSID_FileOpenDialog;
  if(FAILED(CoCreateInstance(clsid,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&raw)))) return {};
  const ComPointer<IFileDialog> dialog{raw};
  FILEOPENDIALOGOPTIONS options{};
  if(FAILED(dialog->GetOptions(&options)) ||
     FAILED(dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|(save_new?FOS_OVERWRITEPROMPT:FOS_FILEMUSTEXIST))) ||
     FAILED(dialog->SetTitle(std::wstring(title).c_str()))) return {};
  if(save_new) dialog->SetDefaultExtension(std::wstring(default_extension).c_str());
  if(FAILED(dialog->Show(GetActiveWindow()))) return {};
  IShellItem *item_raw{};if(FAILED(dialog->GetResult(&item_raw))) return {};
  const ComPointer<IShellItem> item{item_raw};
  PWSTR path{};if(FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) return {};
  const std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> owned{path,&CoTaskMemFree};
  return std::filesystem::path{path};
#else
  const std::string extension = wide_to_utf8(default_extension);
  auto picked =
      helper_file(wide_to_utf8(title), save_new, extension);
  if (!picked)
    return std::nullopt;
  // Mirror the Windows default-extension handling when saving.
  const auto slash = picked->find_last_of('/');
  const auto dot = picked->find_last_of('.');
  if (save_new && !extension.empty() &&
      (dot == std::string::npos ||
       (slash != std::string::npos && dot < slash)))
    *picked += "." + extension;
  return std::filesystem::path{*picked};
#endif
}
