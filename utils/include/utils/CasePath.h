#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace utils {

// ASCII case fold; client package names and directories are ASCII on disk.
inline auto ascii_lower(std::string_view value) -> std::string {
  std::string result;
  result.reserve(value.size());
  for (char c : value)
    result.push_back(c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c);
  return result;
}

// Returns root/name when it exists; otherwise resolves a case-insensitive
// match among the immediate children of root. Clients unpacked onto
// case-sensitive filesystems keep various directory spellings (MAPS, maps,
// Maps); Windows clients resolve them through the case-insensitive FAT/NTFS
// layer. Returns an empty path when nothing matches.
inline auto resolve_directory(const std::filesystem::path &root,
                              std::string_view name) -> std::filesystem::path {
  const auto exact = root / name;
  std::error_code error;
  if (std::filesystem::is_directory(exact, error) && !error)
    return exact;
  const auto folded = ascii_lower(name);
  for (const auto &entry : std::filesystem::directory_iterator{root, error}) {
    if (error)
      break;
    if (entry.is_directory(error) && !error &&
        ascii_lower(entry.path().filename().string()) == folded)
      return entry.path();
  }
  return {};
}

} // namespace utils
