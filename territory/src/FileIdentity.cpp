#include <array>
#include <stdexcept>
#include <system_error>
#include <territory/FileIdentity.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>
#else
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace territory {
#ifndef _WIN32
namespace {
// SHA-256 per FIPS 180-4, replacing the Windows BCrypt provider.
class Sha256 {
public:
  Sha256()
      : state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
              0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}
  void update(const unsigned char *data, std::size_t size) {
    bits += static_cast<std::uint64_t>(size) * 8;
    while (size > 0) {
      const std::size_t take = std::min(size, std::size_t{64} - used);
      std::memcpy(buffer + used, data, take);
      used += take;
      data += take;
      size -= take;
      if (used == 64) {
        compress(buffer);
        used = 0;
      }
    }
  }
  void finish(unsigned char *digest) {
    const std::uint64_t message_bits = bits;
    const unsigned char padding = 0x80;
    update(&padding, 1);
    const unsigned char zero = 0;
    while (used != 56)
      update(&zero, 1);
    const unsigned char tail[8] = {
        static_cast<unsigned char>(message_bits >> 56),
        static_cast<unsigned char>(message_bits >> 48),
        static_cast<unsigned char>(message_bits >> 40),
        static_cast<unsigned char>(message_bits >> 32),
        static_cast<unsigned char>(message_bits >> 24),
        static_cast<unsigned char>(message_bits >> 16),
        static_cast<unsigned char>(message_bits >> 8),
        static_cast<unsigned char>(message_bits)};
    update(tail, sizeof(tail));
    for (int index = 0; index < 8; ++index) {
      digest[index * 4] = static_cast<unsigned char>(state[index] >> 24);
      digest[index * 4 + 1] = static_cast<unsigned char>(state[index] >> 16);
      digest[index * 4 + 2] = static_cast<unsigned char>(state[index] >> 8);
      digest[index * 4 + 3] = static_cast<unsigned char>(state[index]);
    }
  }

private:
  static std::uint32_t rotate_right(std::uint32_t value, unsigned amount) {
    return (value >> amount) | (value << (32 - amount));
  }
  void compress(const unsigned char *block) {
    static constexpr std::uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t w[64];
    for (int index = 0; index < 16; ++index)
      w[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                 (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                 (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                 static_cast<std::uint32_t>(block[index * 4 + 3]);
    for (int index = 16; index < 64; ++index) {
      const std::uint32_t s0 =
          rotate_right(w[index - 15], 7) ^ rotate_right(w[index - 15], 18) ^
          (w[index - 15] >> 3);
      const std::uint32_t s1 =
          rotate_right(w[index - 2], 17) ^ rotate_right(w[index - 2], 19) ^
          (w[index - 2] >> 10);
      w[index] = w[index - 16] + s0 + w[index - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int index = 0; index < 64; ++index) {
      const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                               rotate_right(e, 25);
      const std::uint32_t choice = (e & f) ^ (~e & g);
      const std::uint32_t t1 = h + s1 + choice + K[index] + w[index];
      const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                               rotate_right(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = s0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }
  std::uint32_t state[8];
  std::uint64_t bits{};
  unsigned char buffer[64]{};
  std::size_t used{};
};
struct Descriptor {
  int value{-1};
  ~Descriptor() {
    if (value >= 0)
      ::close(value);
  }
};
} // namespace
#endif
FileIdentity file_identity(const std::filesystem::path &path) {
#ifdef _WIN32
  struct File {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~File() {
      if (value != INVALID_HANDLE_VALUE)
        CloseHandle(value);
    }
  };
  File file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
  if (file.value == INVALID_HANDLE_VALUE)
    throw std::system_error(GetLastError(), std::system_category(),
                            "open hash input");
  BY_HANDLE_FILE_INFORMATION before{}, after{};
  if (!GetFileInformationByHandle(file.value, &before) ||
      (before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    throw std::runtime_error("Invalid hash input file");
  auto checked = [](NTSTATUS status, const char *what) {
    if (status < 0)
      throw std::runtime_error(
          std::string(what) +
          " NTSTATUS=" + std::to_string(static_cast<unsigned long>(status)));
  };
  struct Algorithm {
    BCRYPT_ALG_HANDLE value{};
    ~Algorithm() {
      if (value)
        BCryptCloseAlgorithmProvider(value, 0);
    }
  } algorithm;
  struct Hash {
    BCRYPT_HASH_HANDLE value{};
    ~Hash() {
      if (value)
        BCryptDestroyHash(value);
    }
  } hash;
  checked(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM,
                                      nullptr, 0),
          "SHA-256 provider");
  checked(
      BCryptCreateHash(algorithm.value, &hash.value, nullptr, 0, nullptr, 0, 0),
      "SHA-256 create");
  std::array<unsigned char, 65536> buffer{};
  std::uintmax_t total = 0;
  for (;;) {
    DWORD count = 0;
    if (!ReadFile(file.value, buffer.data(), buffer.size(), &count, nullptr))
      throw std::system_error(GetLastError(), std::system_category(),
                              "read hash input");
    if (!count)
      break;
    checked(BCryptHashData(hash.value, buffer.data(), count, 0),
            "SHA-256 update");
    total += count;
  }
  if (!GetFileInformationByHandle(file.value, &after))
    throw std::runtime_error("Cannot recheck hash input");
  const auto size =
      (std::uintmax_t(before.nFileSizeHigh) << 32) | before.nFileSizeLow;
  if (total != size || before.nFileSizeHigh != after.nFileSizeHigh ||
      before.nFileSizeLow != after.nFileSizeLow ||
      CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime))
    throw std::runtime_error("Hash input changed during read");
  std::array<unsigned char, 32> digest{};
  checked(BCryptFinishHash(hash.value, digest.data(), digest.size(), 0),
          "SHA-256 finish");
  const char hex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(64);
  for (auto value : digest) {
    encoded.push_back(hex[value >> 4]);
    encoded.push_back(hex[value & 15]);
  }
  return {std::filesystem::absolute(path).lexically_normal(), total, encoded};
#else
  Descriptor file{::open(path.c_str(), O_RDONLY)};
  if (file.value < 0)
    throw std::system_error(errno, std::generic_category(), "open hash input");
  struct stat before{};
  if (::fstat(file.value, &before) != 0 || S_ISDIR(before.st_mode))
    throw std::runtime_error("Invalid hash input file");
  Sha256 sha;
  std::array<unsigned char, 65536> buffer{};
  std::uintmax_t total = 0;
  for (;;) {
    const auto count = ::read(file.value, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR)
        continue;
      throw std::system_error(errno, std::generic_category(), "read hash input");
    }
    if (!count)
      break;
    sha.update(buffer.data(), static_cast<std::size_t>(count));
    total += static_cast<std::uintmax_t>(count);
  }
  struct stat after{};
  if (::fstat(file.value, &after) != 0)
    throw std::runtime_error("Cannot recheck hash input");
#ifdef __APPLE__
  const bool unchanged =
      before.st_size == after.st_size &&
      before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
      before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec;
#else
  const bool unchanged = before.st_size == after.st_size &&
                         before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
                         before.st_mtim.tv_nsec == after.st_mtim.tv_nsec;
#endif
  if (total != static_cast<std::uintmax_t>(before.st_size) || !unchanged)
    throw std::runtime_error("Hash input changed during read");
  std::array<unsigned char, 32> digest{};
  sha.finish(digest.data());
  const char hex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(64);
  for (auto value : digest) {
    encoded.push_back(hex[value >> 4]);
    encoded.push_back(hex[value & 15]);
  }
  return {std::filesystem::absolute(path).lexically_normal(), total, encoded};
#endif
}
} // namespace territory
