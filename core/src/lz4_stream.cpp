#include "lz4_stream.hpp"
#include "lz4.h"
#include "lz4frame.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace archiver {
namespace {
constexpr uint32_t kFrame = 0x184D2204;
constexpr uint32_t kLegacy = 0x184C2102;
constexpr uint64_t kOutputLimit = 64ull << 30;
uint32_t little32(const unsigned char *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool skippable(uint32_t value) { return (value & 0xFFFFFFF0u) == 0x184D2A50u; }

class Reader {
 public:
  explicit Reader(FILE *input) : input_(input), buffer_(64 << 10) {}
  bool available() {
    if (pos_ < end_) return true;
    pos_ = 0;
    end_ = fread(buffer_.data(), 1, buffer_.size(), input_);
    return end_ != 0;
  }
  const unsigned char *data() const { return buffer_.data() + pos_; }
  size_t size() const { return end_ - pos_; }
  void consume(size_t n) { pos_ += n; }
  bool read(void *output, size_t n) {
    auto *p = static_cast<unsigned char *>(output);
    while (n) {
      if (!available()) return false;
      const size_t take = std::min(n, size());
      if (p) { std::copy(data(), data() + take, p); p += take; }
      consume(take); n -= take;
    }
    return true;
  }
 private:
  FILE *input_;
  std::vector<unsigned char> buffer_;
  size_t pos_ = 0, end_ = 0;
};
}

ArchiverResult decode_lz4(FILE *input, SafeDest::File *output,
    ArchiverProgressCallback progress, void *user, std::string &message) {
  Reader reader(input);
  uint64_t written = 0;
  bool saw_frame = false;
  auto fail = [&]() { message = "invalid or truncated LZ4 stream"; return ARCHIVER_ERR_FORMAT; };
  auto emit = [&](const void *data, size_t n) -> ArchiverResult {
    if (n > kOutputLimit - written) { message = "output byte limit"; return ARCHIVER_ERR_LIMIT; }
    if (progress && progress(written + n, 0, user)) return ARCHIVER_ERR_CANCELLED;
    if (output && !output->write(data, n, message)) return ARCHIVER_ERR_IO;
    written += n;
    return ARCHIVER_OK;
  };
  uint32_t pending = 0;
  bool have_pending = false;
  while (have_pending || reader.available()) {
    if (progress && progress(written, 0, user)) return ARCHIVER_ERR_CANCELLED;
    unsigned char magic_bytes[4];
    if (!have_pending && !reader.read(magic_bytes, sizeof(magic_bytes))) return fail();
    const uint32_t magic = have_pending ? pending : little32(magic_bytes);
    have_pending = false;
    if (skippable(magic)) {
      unsigned char length[4];
      if (!reader.read(length, sizeof(length))) return fail();
      uint32_t left = little32(length);
      while (left) {
        if (progress && progress(written, 0, user)) return ARCHIVER_ERR_CANCELLED;
        const size_t step = std::min<uint32_t>(left, 64 << 10);
        if (!reader.read(nullptr, step)) return fail();
        left -= static_cast<uint32_t>(step);
      }
      continue;
    }
    if (magic == kFrame) {
      LZ4F_dctx *raw = nullptr;
      if (LZ4F_isError(LZ4F_createDecompressionContext(&raw, LZ4F_VERSION))) {
        message = "could not allocate LZ4 decoder"; return ARCHIVER_ERR_IO;
      }
      std::unique_ptr<LZ4F_dctx, decltype(&LZ4F_freeDecompressionContext)> ctx(raw, LZ4F_freeDecompressionContext);
      // Feed the magic already consumed by Reader back to the frame decoder.
      const unsigned char header[4] = {4, 0x22, 0x4D, 0x18};
      unsigned char output_buffer[64 << 10];
      size_t src_size = 4, dest_size = sizeof(output_buffer);
      size_t next = LZ4F_decompress(ctx.get(), output_buffer, &dest_size, header, &src_size, nullptr);
      if (LZ4F_isError(next)) return fail();
      while (next) {
        if (progress && progress(written, 0, user)) return ARCHIVER_ERR_CANCELLED;
        if (!reader.available()) return fail();
        src_size = reader.size(); dest_size = sizeof(output_buffer);
        next = LZ4F_decompress(ctx.get(), output_buffer, &dest_size, reader.data(), &src_size, nullptr);
        if (LZ4F_isError(next) || (!src_size && !dest_size)) return fail();
        reader.consume(src_size);
        ArchiverResult result = emit(output_buffer, dest_size);
        if (result != ARCHIVER_OK) return result;
      }
      saw_frame = true;
    } else if (magic == kLegacy) {
      constexpr int block_size = 8 << 20;
      std::vector<char> decoded(block_size), compressed;
      saw_frame = true;
      while (reader.available()) {
        if (progress && progress(written, 0, user)) return ARCHIVER_ERR_CANCELLED;
        unsigned char length[4];
        if (!reader.read(length, sizeof(length))) return fail();
        const uint32_t size = little32(length);
        if (size == kFrame || size == kLegacy || skippable(size)) {
          pending = size; have_pending = true; break;
        }
        // The Linux legacy variant adds a zero-sized end marker.
        if (!size) break;
        if (size > static_cast<uint32_t>(LZ4_compressBound(block_size))) return fail();
        compressed.resize(size);
        if (!reader.read(compressed.data(), size)) return fail();
        const int n = LZ4_decompress_safe(compressed.data(), decoded.data(), static_cast<int>(size), block_size);
        if (n < 0) return fail();
        ArchiverResult result = emit(decoded.data(), static_cast<size_t>(n));
        if (result != ARCHIVER_OK) return result;
      }
    } else return fail();
  }
  if (ferror(input)) { message = "could not read LZ4 stream"; return ARCHIVER_ERR_IO; }
  return saw_frame ? ARCHIVER_OK : fail();
}
}
