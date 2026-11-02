#pragma once
#include <filesystem>

// Opens a path with the desktop default handler: ShellExecuteW on Windows,
// xdg-open on POSIX. Returns false when no handler accepted the request.
bool open_path_externally(const std::filesystem::path &path);
