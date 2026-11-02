#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <stdexcept>
#include <territory/PngOutput.h>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <atomic>
#include <wincodec.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
#endif
namespace territory {
namespace {
std::uint32_t big_endian(const unsigned char *p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint32_t crc(std::uint32_t value, std::span<const unsigned char> bytes) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      auto v = i;
      for (int bit = 0; bit < 8; ++bit)
        v = (v >> 1) ^ ((v & 1) ? 0xedb88320u : 0);
      t[i] = v;
    }
    return t;
  }();
  for (auto byte : bytes)
    value = table[(value ^ byte) & 255] ^ (value >> 8);
  return value;
}
#ifdef _WIN32
void checked(HRESULT result, const char *operation) {
  if (FAILED(result))
    throw std::runtime_error(
        std::string(operation) +
        " HRESULT=" + std::to_string(static_cast<std::uint32_t>(result)));
}
template <class T> struct Com {
  T *value{};
  ~Com() { reset(); }
  Com() = default;
  Com(const Com &) = delete;
  void reset() {
    if (value) {
      value->Release();
      value = nullptr;
    }
  }
  T **put() {
    reset();
    return &value;
  }
  T *operator->() const { return value; }
};
struct Apartment {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Apartment() {
    if (FAILED(result) && result != RPC_E_CHANGED_MODE)
      checked(result, "COM initialization");
  }
  ~Apartment() {
    if (SUCCEEDED(result))
      CoUninitialize();
  }
};
struct Temporary {
  std::filesystem::path path;
  bool owned{};
  ~Temporary() {
    if (owned) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
};
class FileStream final : public IStream {
public:
  explicit FileStream(HANDLE file) : file(file) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(iid, __uuidof(IUnknown)) ||
        IsEqualIID(iid, __uuidof(ISequentialStream)) ||
        IsEqualIID(iid, __uuidof(IStream))) {
      *out = static_cast<IStream *>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
  ULONG STDMETHODCALLTYPE Release() override {
    auto n = --refs;
    if (!n)
      delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE Read(void *data, ULONG size,
                                 ULONG *actual) override {
    DWORD n = 0;
    const bool ok = ReadFile(file, data, size, &n, nullptr);
    if (actual)
      *actual = n;
    return ok ? (n == size ? S_OK : S_FALSE)
              : HRESULT_FROM_WIN32(GetLastError());
  }
  HRESULT STDMETHODCALLTYPE Write(const void *data, ULONG size,
                                  ULONG *actual) override {
    DWORD n = 0;
    const bool ok = WriteFile(file, data, size, &n, nullptr);
    if (actual)
      *actual = n;
    return ok && n == size
               ? S_OK
               : (ok ? STG_E_MEDIUMFULL : HRESULT_FROM_WIN32(GetLastError()));
  }
  HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER offset, DWORD origin,
                                 ULARGE_INTEGER *position) override {
    if (origin > STREAM_SEEK_END)
      return STG_E_INVALIDFUNCTION;
    LARGE_INTEGER result{};
    if (!SetFilePointerEx(file, offset, &result, origin))
      return HRESULT_FROM_WIN32(GetLastError());
    if (position)
      position->QuadPart = result.QuadPart;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
    if (size.QuadPart > static_cast<ULONGLONG>(MAXLONGLONG))
      return STG_E_INVALIDFUNCTION;
    LARGE_INTEGER zero{}, old{}, end;
    end.QuadPart = size.QuadPart;
    if (!SetFilePointerEx(file, zero, &old, FILE_CURRENT) ||
        !SetFilePointerEx(file, end, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(file) ||
        !SetFilePointerEx(file, old, nullptr, FILE_BEGIN))
      return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *,
                                   ULARGE_INTEGER *) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Commit(DWORD) override {
    return FlushFileBuffers(file) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
  }
  HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_INVALIDFUNCTION; }
  HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                       DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER,
                                         DWORD) override {
    return STG_E_INVALIDFUNCTION;
  }
  HRESULT STDMETHODCALLTYPE Stat(STATSTG *stat, DWORD) override {
    if (!stat)
      return E_POINTER;
    *stat = {};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size))
      return HRESULT_FROM_WIN32(GetLastError());
    stat->type = STGTY_STREAM;
    stat->cbSize.QuadPart = size.QuadPart;
    stat->grfMode = STGM_READWRITE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IStream **) override { return E_NOTIMPL; }

private:
  ~FileStream() { CloseHandle(file); }
  std::atomic<ULONG> refs{1};
  HANDLE file;
};
#else
struct Temporary {
  std::filesystem::path path;
  bool owned{};
  ~Temporary() {
    if (owned) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
};
void write_all(int file, const unsigned char *data, std::size_t size) {
  std::size_t total = 0;
  while (total < size) {
    const auto written = ::write(file, data + total, size - total);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      throw std::system_error(errno, std::generic_category(), "write PNG");
    }
    total += static_cast<std::size_t>(written);
  }
}
void write_chunk(int file, const char *type,
                 const std::vector<unsigned char> &data) {
  const std::uint32_t count = static_cast<std::uint32_t>(data.size());
  const std::array<unsigned char, 4> length{
      static_cast<unsigned char>(count >> 24),
      static_cast<unsigned char>(count >> 16),
      static_cast<unsigned char>(count >> 8),
      static_cast<unsigned char>(count)};
  const std::array<unsigned char, 4> type_bytes{
      static_cast<unsigned char>(type[0]), static_cast<unsigned char>(type[1]),
      static_cast<unsigned char>(type[2]), static_cast<unsigned char>(type[3])};
  write_all(file, length.data(), length.size());
  write_all(file, type_bytes.data(), type_bytes.size());
  write_all(file, data.data(), data.size());
  auto value = crc(0xffffffffu, std::span(type_bytes));
  value = crc(value, data);
  const std::uint32_t checksum = value ^ 0xffffffffu;
  const std::array<unsigned char, 4> encoded{
      static_cast<unsigned char>(checksum >> 24),
      static_cast<unsigned char>(checksum >> 16),
      static_cast<unsigned char>(checksum >> 8),
      static_cast<unsigned char>(checksum)};
  write_all(file, encoded.data(), encoded.size());
}
#endif
} // namespace
bool png_has_dimensions(const std::filesystem::path &path, int width,
                        int height) {
  if (width < 1 || height < 1)
    return false;
  std::ifstream input(path, std::ios::binary);
  std::array<unsigned char, 8> header{};
  const std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
  if (!input.read(reinterpret_cast<char *>(header.data()), 8) ||
      header != signature)
    return false;
  bool ihdr = false, idat = false;
  std::array<unsigned char, 65536> buffer{};
  while (input.read(reinterpret_cast<char *>(header.data()), 8)) {
    auto count = big_endian(header.data());
    if (count > 512u * 1024 * 1024)
      return false;
    const std::string_view type(
        reinterpret_cast<const char *>(header.data() + 4), 4);
    if (!ihdr && type != "IHDR")
      return false;
    auto checksum = crc(0xffffffffu, std::span(header).subspan(4));
    std::uint32_t left = count;
    while (left) {
      auto n = std::min<std::uint32_t>(left, buffer.size());
      if (!input.read(reinterpret_cast<char *>(buffer.data()), n))
        return false;
      if (type == "IHDR" &&
          (ihdr || count != 13 ||
           big_endian(buffer.data()) != static_cast<unsigned>(width) ||
           big_endian(buffer.data() + 4) != static_cast<unsigned>(height) ||
           buffer[8] != 8 || buffer[9] != 2 || buffer[10] != 0 ||
           buffer[11] != 0 || buffer[12] != 0))
        return false;
      checksum = crc(checksum, std::span(buffer).first(n));
      left -= n;
    }
    std::array<unsigned char, 4> expected{};
    if (!input.read(reinterpret_cast<char *>(expected.data()), 4) ||
        (checksum ^ 0xffffffffu) != big_endian(expected.data()))
      return false;
    if (type == "IHDR") {
      if (count != 13)
        return false;
      ihdr = true;
    }
    if (type == "IDAT")
      idat = true;
    if (type == "IEND")
      return count == 0 && ihdr && idat &&
             input.peek() == std::char_traits<char>::eof();
  }
  return false;
}
struct PngOutput::Impl {
  std::filesystem::path final_path;
  int width{}, height{}, next{};
  bool finished{}, failed{};
  Checkpoint checkpoint;
  std::thread::id owner = std::this_thread::get_id();
#ifdef _WIN32
  Apartment apartment;
  Temporary temporary;
  Com<IStream> stream;
  Com<IWICImagingFactory> factory;
  Com<IWICBitmapEncoder> encoder;
  Com<IWICBitmapFrameEncode> frame;
#else
  ~Impl() {
    if (file >= 0)
      ::close(file);
    if (zlib.state)
      deflateEnd(&zlib);
  }
  Temporary temporary;
  int file{-1};
  z_stream zlib{};
  std::vector<unsigned char> row_buffer;
  std::vector<unsigned char> idat;
#endif
  Impl(const std::filesystem::path &path, int w, int h, Checkpoint hook)
      : final_path(std::filesystem::absolute(path).lexically_normal()),
        width(w), height(h), checkpoint(std::move(hook)) {
    if (w < 1 || h < 1 || w > 16384 || h > 16384)
      throw std::invalid_argument("PNG dimensions must be 1..16384");
    if (std::filesystem::exists(final_path))
      throw std::runtime_error("PNG destination already exists");
    if (!std::filesystem::is_directory(final_path.parent_path()))
      throw std::runtime_error("PNG parent directory does not exist");
#ifdef _WIN32
    std::random_device random;
    temporary.path =
        final_path.parent_path() / (".png-" + std::to_string(random()) + "-" +
                                    std::to_string(random()) + ".tmp");
    HANDLE handle =
        CreateFileW(temporary.path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      throw std::system_error(GetLastError(), std::system_category(),
                              "create PNG temporary");
    temporary.owned = true;
    try {
      stream.value = new FileStream(handle);
    } catch (...) {
      CloseHandle(handle);
      throw;
    }
    checked(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                             CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                             reinterpret_cast<void **>(factory.put())),
            "create WIC factory");
    checked(
        factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()),
        "create PNG encoder");
    checked(encoder->Initialize(stream.value, WICBitmapEncoderNoCache),
            "initialize PNG stream");
    Com<IPropertyBag2> properties;
    checked(encoder->CreateNewFrame(frame.put(), properties.put()),
            "create PNG frame");
    checked(frame->Initialize(properties.value), "initialize PNG frame");
    checked(frame->SetSize(w, h), "set PNG size");
    // WIC's native PNG encoder accepts BGR24 memory, and writes RGB24 PNG.
    auto format = GUID_WICPixelFormat24bppBGR;
    checked(frame->SetPixelFormat(&format), "set PNG BGR24 memory format");
    if (!IsEqualGUID(format, GUID_WICPixelFormat24bppBGR))
      throw std::runtime_error(
          "PNG encoder did not accept lossless 24-bit color");
    Com<IWICMetadataQueryWriter> metadata;
    checked(frame->GetMetadataQueryWriter(metadata.put()),
            "PNG metadata writer");
    PROPVARIANT value{};
    value.vt = VT_UI1;
    value.bVal = 0;
    checked(metadata->SetMetadataByName(L"/sRGB/RenderingIntent", &value),
            "PNG sRGB metadata");
#else
    std::random_device random;
    temporary.path =
        final_path.parent_path() / (".png-" + std::to_string(random()) + "-" +
                                    std::to_string(random()) + ".tmp");
    file = ::open(temporary.path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (file < 0)
      throw std::system_error(errno, std::generic_category(),
                              "create PNG temporary");
    temporary.owned = true;
    const std::array<unsigned char, 8> signature{137, 80, 78, 71,
                                                 13, 10, 26, 10};
    write_all(file, signature.data(), signature.size());
    // IHDR: 8-bit truecolor RGB, no interlace.
    const std::array<unsigned char, 13> header{
        static_cast<unsigned char>(w >> 24), static_cast<unsigned char>(w >> 16),
        static_cast<unsigned char>(w >> 8), static_cast<unsigned char>(w),
        static_cast<unsigned char>(h >> 24), static_cast<unsigned char>(h >> 16),
        static_cast<unsigned char>(h >> 8), static_cast<unsigned char>(h),
        8, 2, 0, 0, 0};
    std::vector<unsigned char> chunk(header.begin(), header.end());
    write_chunk(file, "IHDR", chunk);
    // Perceptual sRGB intent, mirroring the WIC metadata above.
    chunk.assign(1, 0);
    write_chunk(file, "sRGB", chunk);
    if (deflateInit(&zlib, Z_DEFAULT_COMPRESSION) != Z_OK)
      throw std::runtime_error("PNG deflate initialization failed");
    row_buffer.resize(1 + static_cast<std::size_t>(w) * 3);
#endif
  }
#ifndef _WIN32
  void deflate_feed(std::span<const std::uint8_t> data) {
    zlib.next_in =
        const_cast<Bytef *>(reinterpret_cast<const Bytef *>(data.data()));
    zlib.avail_in = static_cast<uInt>(data.size());
    std::array<unsigned char, 65536> out;
    while (zlib.avail_in > 0) {
      zlib.next_out = out.data();
      zlib.avail_out = static_cast<uInt>(out.size());
      const int state = deflate(&zlib, Z_NO_FLUSH);
      const auto produced = out.size() - zlib.avail_out;
      if (produced) {
        idat.insert(idat.end(), out.data(), out.data() + produced);
        if (idat.size() >= 1024 * 1024) {
          write_chunk(file, "IDAT", idat);
          idat.clear();
        }
      }
      if (state != Z_OK && state != Z_BUF_ERROR)
        throw std::runtime_error("PNG deflate failed");
      if (state == Z_BUF_ERROR && !produced)
        throw std::runtime_error("PNG deflate stalled");
    }
  }
#endif
  void check() {
    if (std::this_thread::get_id() != owner)
      throw std::runtime_error("PNG writer used from a different thread");
    if (failed || finished)
      throw std::runtime_error("PNG writer is no longer writable");
  }
  void rows(int y, int w, int count, std::span<const std::uint8_t> bytes) {
    check();
    if (y != next || w != width || count <= 0 || count > height - next ||
        bytes.size() != static_cast<std::size_t>(width) * count * 3)
      throw std::invalid_argument(
          "PNG rows must be complete, sequential RGB24 bands");
    try {
      if (checkpoint)
        checkpoint("write");
#ifdef _WIN32
      // Shuffle one row only; never duplicate the image-width tile band.
      std::vector<BYTE> bgr(static_cast<std::size_t>(width) * 3);
      for (int row = 0; row < count; ++row) {
        const auto rgb =
            bytes.subspan(static_cast<std::size_t>(row) * width * 3, width * 3);
        for (int x = 0; x < width; ++x) {
          bgr[x * 3] = rgb[x * 3 + 2];
          bgr[x * 3 + 1] = rgb[x * 3 + 1];
          bgr[x * 3 + 2] = rgb[x * 3];
        }
        checked(frame->WritePixels(1, width * 3, static_cast<UINT>(bgr.size()),
                                   bgr.data()),
                "write PNG row");
      }
#else
      // One filter-0 scanline per row; RGB24 goes into the stream as-is.
      for (int row = 0; row < count; ++row) {
        const auto rgb = bytes.subspan(
            static_cast<std::size_t>(row) * width * 3,
            static_cast<std::size_t>(width) * 3);
        row_buffer[0] = 0;
        std::copy(rgb.begin(), rgb.end(), row_buffer.begin() + 1);
        deflate_feed(row_buffer);
      }
#endif
      next += count;
    } catch (...) {
      failed = true;
      throw;
    }
  }
  void finish(const Cancel &cancel) {
    check();
    if (next != height)
      throw std::runtime_error("PNG frame has incomplete rows");
    try {
      check_cancel(cancel);
      if (checkpoint)
        checkpoint("commit");
#ifdef _WIN32
      checked(frame->Commit(), "commit PNG frame");
      checked(encoder->Commit(), "commit PNG encoder");
      checked(stream->Commit(STGC_DEFAULT), "flush PNG");
      frame.reset();
      encoder.reset();
      stream.reset();
      if (!png_has_dimensions(temporary.path, width, height))
        throw std::runtime_error("PNG verification failed before publication");
      check_cancel(cancel);
      if (!MoveFileExW(temporary.path.c_str(), final_path.c_str(),
                       MOVEFILE_WRITE_THROUGH))
        throw std::system_error(GetLastError(), std::system_category(),
                                "publish PNG without replacement");
      temporary.owned = false;
#else
      std::array<unsigned char, 65536> out;
      int state = Z_OK;
      do {
        zlib.next_out = out.data();
        zlib.avail_out = static_cast<uInt>(out.size());
        state = deflate(&zlib, Z_FINISH);
        const auto produced = out.size() - zlib.avail_out;
        if (produced) {
          idat.insert(idat.end(), out.data(), out.data() + produced);
          if (idat.size() >= 1024 * 1024) {
            write_chunk(file, "IDAT", idat);
            idat.clear();
          }
        }
        if (state != Z_OK && state != Z_STREAM_END)
          throw std::runtime_error("PNG deflate failed");
      } while (state != Z_STREAM_END);
      deflateEnd(&zlib);
      if (!idat.empty()) {
        write_chunk(file, "IDAT", idat);
        idat.clear();
      }
      const std::vector<unsigned char> empty;
      write_chunk(file, "IEND", empty);
      if (::fsync(file) != 0)
        throw std::system_error(errno, std::generic_category(), "flush PNG");
      if (::close(file) != 0) {
        file = -1;
        throw std::system_error(errno, std::generic_category(), "close PNG");
      }
      file = -1;
      if (!png_has_dimensions(temporary.path, width, height))
        throw std::runtime_error("PNG verification failed before publication");
      check_cancel(cancel);
      // Exclusive publication: link cannot replace an existing destination.
      if (::link(temporary.path.c_str(), final_path.c_str()) != 0)
        throw std::system_error(errno, std::generic_category(),
                                "publish PNG without replacement");
      std::error_code ignored;
      std::filesystem::remove(temporary.path, ignored);
      temporary.owned = false;
#endif
      finished = true;
    } catch (...) {
      failed = true;
      throw;
    }
  }
};
PngOutput::PngOutput(const std::filesystem::path &p, int w, int h,
                     Checkpoint hook)
    : impl(std::make_unique<Impl>(p, w, h, std::move(hook))) {}
PngOutput::~PngOutput() = default;
void PngOutput::rows(int y, int w, int count,
                     std::span<const std::uint8_t> bytes) {
  impl->rows(y, w, count, bytes);
}
void PngOutput::finish(const Cancel &cancel) { impl->finish(cancel); }
} // namespace territory
