#include "fsx/image.hpp"
#include "fsx/cancel.hpp"
#include "fsx/crc.hpp"
#include "fsx/endian.hpp"
#include "fsx/file_util.hpp"
#include "fsx/fault.hpp"
#include "fsx/safety.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace fsx::image {
namespace {

constexpr std::size_t kHeaderSize = 4096;
constexpr std::size_t kRecordSize = 48;
constexpr std::size_t kFooterSize = 4096;
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kHeaderComplete = 1U;
constexpr std::uint32_t kChunkZero = 1U;
constexpr std::array<char, 8> kHeaderMagic{{'F','S','X','I','M','G','1','\0'}};
constexpr std::array<char, 8> kRecordMagic{{'F','S','X','C','H','N','1','\0'}};
constexpr std::array<char, 8> kFooterMagic{{'F','S','X','E','N','D','1','\0'}};

struct Fd {
    int n{-1};
    Fd() = default;
    explicit Fd(int x) : n(x) {
    }
    Fd(const Fd&) = delete;
    Fd& operator = (const Fd&) = delete;
    Fd(Fd && o) noexcept : n(o.n) {
        o.n = -1;
    }
    ~Fd() {
        if (n >= 0) ::close(n);
    }
};

bool magic_eq(std::span<const std::byte> b, const std::array<char, 8>& magic) {
    if (b.size() < magic.size()) return false;
    for (std::size_t i = 0; i < magic.size(); ++i)
        if (std::to_integer<unsigned char>(b[i]) != static_cast<unsigned char>(magic[i])) return false;
    return true;
}

void write_magic(std::span<std::byte> b, const std::array<char, 8>& magic) {
    for (std::size_t i = 0; i < magic.size(); ++i) b[i] = std::byte(static_cast<unsigned char>(magic[i]));
}

std::uint32_t checksum(std::span<const std::byte> b) {
    return crc32c(b) ^ 0xffffffffU;
}

std::uint64_t ceil_div_u64(std::uint64_t n, std::uint64_t d) noexcept {
    return n / d + (n % d != 0 ? 1U : 0U);
}

Result<void> fd_write_exact(int fd, std::span<const std::byte> data) {
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::write, fault::Timing::before);
        if (!f)return f.error();
    }
    std::size_t done = 0;
    while (done < data.size()) {
        const auto n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return from_errno(Errc::write_failed,
                              "image write failed: " + std::string(std::strerror(errno)), errno);
        }
        if (n == 0) return Error{Errc::short_io, 0, "image write returned zero"};
        done += static_cast<std::size_t>(n);
    }
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::write, fault::Timing::after);
        if (!f)return f.error();
    }
    return {};
}

Result<void> fd_pwrite_exact(int fd, std::uint64_t off, std::span<const std::byte> data) {
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::write, fault::Timing::before);
        if (!f)return f.error();
    }
    std::size_t done = 0;
    while (done < data.size()) {
        const auto pos = off + done;
        if (pos > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            return Error{Errc::unsupported, 0, "image offset exceeds host off_t"};
        const auto n = ::pwrite(fd, data.data() + done, data.size() - done, static_cast<off_t>(pos));
        if (n < 0) {
            if (errno == EINTR) continue;
            return from_errno(Errc::write_failed,
                              "image pwrite failed: " + std::string(std::strerror(errno)), errno);
        }
        if (n == 0) return Error{Errc::short_io, 0, "image pwrite returned zero"};
        done += static_cast<std::size_t>(n);
    }
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::write, fault::Timing::after);
        if (!f)return f.error();
    }
    return {};
}

Result<void> fd_read_exact(int fd, std::span<std::byte> data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const auto n = ::read(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return from_errno(Errc::read_failed,
                              "image read failed: " + std::string(std::strerror(errno)), errno);
        }
        if (n == 0) return Error{Errc::short_io, 0, "unexpected end of image"};
        done += static_cast<std::size_t>(n);
    }
    return {};
}

bool all_zero(std::span<const std::byte> b) {
    const auto* p = reinterpret_cast<const unsigned char*>(b.data());
    std::size_t i = 0;
    // Word-at-a-time zero scan without alignment assumptions.
    for (; i + sizeof(std::uint64_t) <= b.size(); i += sizeof(std::uint64_t)) {
        std::uint64_t w = 0;
        std::memcpy(&w, p + i, sizeof(w));
        if (w != 0) return false;
    }
    for (; i < b.size(); ++i) if (p[i] != 0) return false;
    return true;
}

std::array<std::byte, kHeaderSize> make_header(const ImageInfo& info, bool complete) {
    std::array<std::byte, kHeaderSize> h{};
    write_magic(h, kHeaderMagic);
    write_le32(h, 8, kVersion);
    write_le32(h, 12, static_cast<std::uint32_t>(kHeaderSize));
    write_le64(h, 16, info.source_bytes);
    write_le32(h, 24, info.logical_sector);
    write_le32(h, 28, info.chunk_bytes);
    write_le64(h, 32, info.chunks);
    write_le32(h, 40, complete ? kHeaderComplete : 0U);
    write_le32(h, 44, 0);
    write_le32(h, 44, checksum(h));
    return h;
}

std::array<std::byte, kRecordSize> make_record(std::uint64_t index,
                                               std::uint64_t source_offset,
                                               std::uint32_t logical_len,
                                               std::uint32_t stored_len,
                                               std::uint32_t flags,
                                               std::uint32_t payload_crc) {
    std::array<std::byte, kRecordSize> r{};
    write_magic(r, kRecordMagic);
    write_le64(r, 8, index);
    write_le64(r, 16, source_offset);
    write_le32(r, 24, logical_len);
    write_le32(r, 28, stored_len);
    write_le32(r, 32, flags);
    write_le32(r, 36, payload_crc);
    write_le32(r, 40, 0);
    write_le32(r, 40, checksum(r));
    return r;
}

std::array<std::byte, kFooterSize> make_footer(const ImageInfo& info) {
    std::array<std::byte, kFooterSize> f{};
    write_magic(f, kFooterMagic);
    write_le64(f, 8, info.chunks);
    write_le64(f, 16, info.data_chunks);
    write_le64(f, 24, info.zero_chunks);
    write_le64(f, 32, info.stored_payload_bytes);
    write_le32(f, 40, 0);
    write_le32(f, 40, checksum(f));
    return f;
}

struct ParsedHeader { ImageInfo info;
    std::uint32_t flags{};
};

Result<ParsedHeader> parse_header(std::span<const std::byte> h) {
    if (h.size() != kHeaderSize || !magic_eq(h, kHeaderMagic))
        return Error{Errc::corrupt, 0, "not an FSX image"};
    if (read_le<std::uint32_t>(h, 8) != kVersion || read_le<std::uint32_t>(h, 12) != kHeaderSize)
        return Error{Errc::unsupported, 0, "unsupported FSX image format version"};
    std::array<std::byte, kHeaderSize> tmp{};
    std::copy(h.begin(), h.end(), tmp.begin());
    const auto stored_crc = read_le<std::uint32_t>(tmp, 44);
    write_le32(tmp, 44, 0);
    if (checksum(tmp) != stored_crc) return Error{Errc::corrupt, 0, "FSX image header checksum failed"};
    ParsedHeader p;
    p.info.source_bytes = read_le<std::uint64_t>(h, 16);
    p.info.logical_sector = read_le<std::uint32_t>(h, 24);
    p.info.chunk_bytes = read_le<std::uint32_t>(h, 28);
    p.info.chunks = read_le<std::uint64_t>(h, 32);
    p.flags = read_le<std::uint32_t>(h, 40);
    if ((p.flags & ~kHeaderComplete) != 0)
        return Error{Errc::unsupported, 0, "FSX image header uses unknown flags"};
    p.info.complete = (p.flags & kHeaderComplete) != 0;
    const auto sector = p.info.logical_sector;
    const bool sector_pow2 = sector != 0 && (sector & (sector - 1U)) == 0;
    if (p.info.source_bytes == 0 || !sector_pow2 || sector < 512U || sector > 65536U ||
        p.info.chunk_bytes < 64U * 1024U || p.info.chunk_bytes > 64U * 1024U * 1024U ||
        (p.info.chunk_bytes % sector) != 0)
        return Error{Errc::corrupt, 0, "invalid FSX image geometry"};
    const auto expected_chunks = ceil_div_u64(p.info.source_bytes, p.info.chunk_bytes);
    if (p.info.chunks != expected_chunks) return Error{
        Errc::corrupt, 0, "FSX image chunk count is inconsistent"
    }
    ;
    return p;
}

struct ParsedRecord {
    std::uint64_t index{};
    std::uint64_t source_offset{};
    std::uint32_t logical_len{};
    std::uint32_t stored_len{};
    std::uint32_t flags{};
    std::uint32_t payload_crc{};
};

Result<ParsedRecord> parse_record(std::span<const std::byte> r,
                                  const ImageInfo& info, std::uint64_t expected_index) {
    if (r.size() != kRecordSize || !magic_eq(r, kRecordMagic))
        return Error{Errc::corrupt, 0, "invalid FSX image chunk record"};
    std::array<std::byte, kRecordSize> tmp{};
    std::copy(r.begin(), r.end(), tmp.begin());
    const auto stored_crc = read_le<std::uint32_t>(tmp, 40);
    write_le32(tmp, 40, 0);
    if (checksum(tmp) != stored_crc) return Error{Errc::corrupt, 0, "FSX image chunk header checksum failed"};
    ParsedRecord p;
    p.index = read_le<std::uint64_t>(r, 8);
    p.source_offset = read_le<std::uint64_t>(r, 16);
    p.logical_len = read_le<std::uint32_t>(r, 24);
    p.stored_len = read_le<std::uint32_t>(r, 28);
    p.flags = read_le<std::uint32_t>(r, 32);
    p.payload_crc = read_le<std::uint32_t>(r, 36);
    if (expected_index > std::numeric_limits<std::uint64_t>::max() / static_cast<std::uint64_t>(info.chunk_bytes))
        return Error{Errc::corrupt, 0, "FSX image chunk offset overflows"};
    const auto expected_offset = expected_index * static_cast<std::uint64_t>(info.chunk_bytes);
    if (p.index != expected_index || p.source_offset != expected_offset
        || p.source_offset >= info.source_bytes)
        return Error{Errc::corrupt, 0, "FSX image chunks are out of order"};
    const auto remain = info.source_bytes - p.source_offset;
    const auto expected_len = static_cast<std::uint32_t>(std::min<std::uint64_t>(info.chunk_bytes, remain));
    if (p.logical_len != expected_len || p.logical_len == 0)
        return Error{Errc::corrupt, 0, "FSX image chunk length is invalid"};
    if ((p.flags & ~kChunkZero) != 0)
        return Error{Errc::unsupported, 0, "FSX image uses unknown chunk flags"};
    if ((p.flags & kChunkZero) != 0) {
        if (p.stored_len != 0 || p.payload_crc != 0)
            return Error{Errc::corrupt, 0, "zero chunk contains payload metadata"};
    } else if (p.stored_len != p.logical_len) {
        return Error{Errc::corrupt, 0, "uncompressed chunk stored length differs from logical length"};
    }
    return p;
}

Result<void> parse_footer(std::span<const std::byte> f, ImageInfo& info) {
    if (f.size() != kFooterSize || !magic_eq(f, kFooterMagic))
        return Error{Errc::corrupt, 0, "FSX image footer is missing"};
    std::array<std::byte, kFooterSize> tmp{};
    std::copy(f.begin(), f.end(), tmp.begin());
    const auto stored = read_le<std::uint32_t>(tmp, 40);
    write_le32(tmp, 40, 0);
    if (checksum(tmp) != stored) return Error{Errc::corrupt, 0, "FSX image footer checksum failed"};
    if (read_le<std::uint64_t>(f, 8) != info.chunks)
        return Error{Errc::corrupt, 0, "FSX image footer chunk count differs from header"};
    const auto data = read_le<std::uint64_t>(f, 16);
    const auto zero = read_le<std::uint64_t>(f, 24);
    if (data > info.chunks || zero != info.chunks - data)
        return Error{Errc::corrupt, 0, "FSX image footer chunk totals are invalid"};
    info.data_chunks = data;
    info.zero_chunks = zero;
    info.stored_payload_bytes = read_le<std::uint64_t>(f, 32);
    return {};
}

Result<Fd> open_image_read(const std::string& path) {
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    const int fd = ::open(path.c_str(), flags);
    if (fd < 0) return from_errno(Errc::open_failed,
                                  "cannot open image " + path + ": " + std::strerror(errno), errno);
    return Fd(fd);
}

Result<ParsedHeader> read_header(int fd) {
    std::array<std::byte, kHeaderSize> h{};
    auto r = fd_read_exact(fd, h);
    if (!r) return r.error();
    return parse_header(h);
}

Result<ImageInfo> walk_image(int fd, bool verify_payload, Progress* progress) {
    auto hr = read_header(fd);
    if (!hr) return hr.error();
    ImageInfo info = hr.value().info;
    if (!info.complete) return Error{Errc::corrupt, 0, "FSX image is incomplete"};
    std::vector<std::byte> payload(info.chunk_bytes);
    std::uint64_t data_chunks = 0, zero_chunks = 0, payload_bytes = 0;
    for (std::uint64_t i = 0; i < info.chunks; ++i) {
        if (Cancellation::requested()) return Error{Errc::unsafe, 0, "image verification cancelled"};
        std::array<std::byte, kRecordSize> raw{};
        auto rr = fd_read_exact(fd, raw);
        if (!rr) return rr.error();
        auto rec = parse_record(raw, info, i);
        if (!rec) return rec.error();
        if ((rec.value().flags & kChunkZero) != 0) {
            ++zero_chunks;
        } else {
            ++data_chunks;
            if (payload_bytes > info.source_bytes - rec.value().stored_len)
                return Error{Errc::corrupt, 0, "FSX image payload accounting overflows source size"};
            payload_bytes += rec.value().stored_len;
            auto span = std::span<std::byte>(payload).first(rec.value().stored_len);
            auto pr = fd_read_exact(fd, span);
            if (!pr) return pr.error();
            if (verify_payload && checksum(span) != rec.value().payload_crc)
                return Error{
                    Errc::corrupt, 0, "FSX image payload checksum failed at chunk " + std::to_string(i)
                }
            ;
        }
        if (progress) progress->update({
                                       "verifying image",
                                       "chunk " + std::to_string(i+1) + "/" + std::to_string(info.chunks),
                                       rec.value().source_offset + rec.value().logical_len,
                                       info.source_bytes,
                                       payload_bytes, 0, i+1, info.chunks, payload_bytes});
    }
    std::array<std::byte, kFooterSize> f{};
    auto fr = fd_read_exact(fd, f);
    if (!fr) return fr.error();
    auto pf = parse_footer(f, info);
    if (!pf) return pf.error();
    if (info.data_chunks != data_chunks || info.zero_chunks != zero_chunks
        || info.stored_payload_bytes != payload_bytes)
        return Error{Errc::corrupt, 0, "FSX image footer accounting does not match records"};
    char extra{};
    const auto n = ::read(fd, &extra, 1);
    if (n < 0 && errno != EINTR) return from_errno(Errc::read_failed, "cannot check image end", errno);
    if (n > 0) return Error{Errc::corrupt, 0, "FSX image has unexpected trailing data"};
    return info;
}

} // namespace

Result<ImageInfo> create(BlockDevice& source,
                         const std::string& image_path,
                         const CreateOptions& options,
                         Progress* progress) {
    if (options.chunk_bytes < 64U * 1024U || options.chunk_bytes > 64U * 1024U * 1024U)
        return Error{Errc::invalid_argument, 0, "image chunk size must be between 64 KiB and 64 MiB"};
    if (source.geometry().logical_sector == 0)
        return Error{Errc::invalid_argument, 0, "source reports a zero logical sector size"};
    if ((options.chunk_bytes % source.geometry().logical_sector) != 0)
        return Error{
            Errc::invalid_argument, 0, "image chunk size must be a multiple of the source logical sector"
        }
    ;
    struct stat existing{};
    if (::lstat(image_path.c_str(), &existing) == 0)
        return Error{Errc::unsafe, 0, "refusing to overwrite existing image file"};
    if (errno != ENOENT) return from_errno(Errc::open_failed, "cannot inspect output image path", errno);

    // mkstemp gives us a unique sibling inode without predictable-name
    // collisions across PID reuse or stale files from an interrupted run.
    std::string temp_pattern = image_path + ".part.XXXXXX";
    std::vector<char> temp_name(temp_pattern.begin(), temp_pattern.end());
    temp_name.push_back('\0');
    const int temp_fd = ::mkstemp(temp_name.data());
    if (temp_fd < 0)
        return from_errno(Errc::open_failed,
                          "cannot create temporary image near " + image_path + ": " + std::strerror(errno), errno);
#ifdef FD_CLOEXEC
    const int old_fd_flags = ::fcntl(temp_fd, F_GETFD);
    if (old_fd_flags >= 0) (void)::fcntl(temp_fd, F_SETFD, old_fd_flags | FD_CLOEXEC);
#endif
    const std::string temp(temp_name.data());
    Fd out(temp_fd);
    struct Cleanup {
        std::string p;
        bool keep{
            false
        }
        ;
        ~Cleanup() {
            if (!keep) ::unlink(p.c_str());
        }
    }
    cleanup{
        temp
    }
    ;

    ImageInfo info;
    info.source_bytes = source.geometry().size_bytes;
    if (info.source_bytes == 0) return Error{Errc::invalid_argument, 0, "cannot image an empty source"};
    info.logical_sector = source.geometry().logical_sector;
    info.chunk_bytes = options.chunk_bytes;
    info.chunks = ceil_div_u64(info.source_bytes, info.chunk_bytes);
    auto h = make_header(info, false);
    auto hw = fd_write_exact(out.n, h);
    if (!hw) return hw.error();

    std::vector<std::byte> buffer(info.chunk_bytes);
    std::uint64_t off = 0;
    for (std::uint64_t i = 0; i < info.chunks; ++i) {
        if (Cancellation::requested()) return Error{
            Errc::unsafe, 0, "image creation cancelled; partial file removed"
        }
        ;
        const auto n64 = std::min<std::uint64_t>(info.chunk_bytes, info.source_bytes - off);
        const auto n = static_cast<std::size_t>(n64);
        auto span = std::span<std::byte>(buffer).first(n);
        auto r = source.read_exact(off, span);
        if (!r) return r.error();
        const bool zero = options.sparse_zero_chunks && all_zero(span);
        const auto payload_crc = zero ? 0U : checksum(span);
        auto record = make_record(i, off, static_cast<std::uint32_t>(n),
                                  zero ? 0U : static_cast<std::uint32_t>(n), zero ? kChunkZero : 0U, payload_crc);
        auto rw = fd_write_exact(out.n, record);
        if (!rw) return rw.error();
        if (zero) ++info.zero_chunks;
        else {
            auto pw = fd_write_exact(out.n, span);
            if (!pw) return pw.error();
            ++info.data_chunks;
            info.stored_payload_bytes += n64;
        }
        off += n64;
        if (progress) progress->update({
                                       "creating image",
                                       "chunk " + std::to_string(i+1) + "/" + std::to_string(info.chunks),
                                       off, info.source_bytes,
                                       off, info.stored_payload_bytes, i+1, info.chunks
                                       }
                                      );
    }
    auto footer = make_footer(info);
    auto fw = fd_write_exact(out.n, footer);
    if (!fw) return fw.error();
    info.complete = true;
    h = make_header(info, true);
    auto final_header = fd_pwrite_exact(out.n, 0, h);
    if (!final_header) return final_header.error();
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::flush, fault::Timing::before);
        if (!f)return f.error();
    }
    if (::fsync(out.n) != 0) return from_errno(Errc::flush_failed, "cannot flush completed image", errno);
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::flush, fault::Timing::after);
        if (!f)return f.error();
    }
    auto published = publish_file_noreplace(temp, image_path, "image file");
    if (!published) return published.error();
    cleanup.keep = true;
    return info;
}

Result<ImageInfo> inspect(const std::string& image_path) {
    auto fd = open_image_read(image_path);
    if (!fd) return fd.error();
    auto hr = read_header(fd.value().n);
    if (!hr) return hr.error();
    return hr.value().info;
}

Result<ImageInfo> verify(const std::string& image_path, Progress* progress) {
    auto fd = open_image_read(image_path);
    if (!fd) return fd.error();
    return walk_image(fd.value().n, true, progress);
}

Result<ImageInfo> restore(const std::string& image_path,
                          BlockDevice& target,
                          bool allow_block_device,
                          Progress* progress) {
    auto fd = open_image_read(image_path);
    if (!fd) return fd.error();
    struct stat source_stat{}, target_stat{};
    if (::fstat(fd.value().n, &source_stat) != 0)
        return from_errno(Errc::io,
                          "cannot stat image source: " + std::string(std::strerror(errno)),
                          errno);
    if (::fstat(target.native_fd(), &target_stat) != 0)
        return from_errno(Errc::io,
                          "cannot stat restore target: " + std::string(std::strerror(errno)),
                          errno);
    if (source_stat.st_dev == target_stat.st_dev &&
        source_stat.st_ino == target_stat.st_ino)
        return Error{Errc::unsafe, 0,
            "image source and restore target refer to the same object"};
    auto hr = read_header(fd.value().n);
    if (!hr) return hr.error();
    const auto info = hr.value().info;
    if (!info.complete) return Error{Errc::corrupt, 0, "FSX image is incomplete"};
    if (target.geometry().size_bytes < info.source_bytes)
        return Error{Errc::unsafe, 0, "restore target is smaller than source image"};
    if (target.geometry().is_block_device && !allow_block_device)
        return Error{Errc::unsafe, 0, "restoring to a block device requires --allow-block-device"};
    auto safe = require_offline_for_write(target);
    if (!safe) return safe.error();

    std::vector<std::byte> buffer(info.chunk_bytes);
    std::vector<std::byte> zeros(info.chunk_bytes, std::byte{0});
    std::uint64_t written = 0;
    for (std::uint64_t i = 0; i < info.chunks; ++i) {
        if (Cancellation::requested()) return Error{
            Errc::unsafe, 0, "restore cancelled; target contains a partial restore"
        }
        ;
        std::array<std::byte, kRecordSize> raw{};
        auto rr = fd_read_exact(fd.value().n, raw);
        if (!rr) return rr.error();
        auto rec = parse_record(raw, info, i);
        if (!rec) return rec.error();
        const auto n = static_cast<std::size_t>(rec.value().logical_len);
        if ((rec.value().flags & kChunkZero) != 0) {
            auto wr = target.write_exact(rec.value().source_offset,
                                         std::span<const std::byte>(zeros).first(n));
            if (!wr) return wr.error();
        } else {
            auto span = std::span<std::byte>(buffer).first(n);
            auto pr = fd_read_exact(fd.value().n, span);
            if (!pr) return pr.error();
            if (checksum(span) != rec.value().payload_crc)
                return Error{
                    Errc::corrupt, 0, "image payload checksum failed before restore at chunk " + std::to_string(i)
                }
            ;
            auto wr = target.write_exact(rec.value().source_offset, span);
            if (!wr) return wr.error();
        }
        written += n;
        if (progress) progress->update({
                                       "restoring image", "write pass", written, info.source_bytes, written,
                                       written, i+1, info.chunks
                                       }
                                      );
    }
    std::array<std::byte, kFooterSize> footer{};
    auto fr = fd_read_exact(fd.value().n, footer);
    if (!fr) return fr.error();
    ImageInfo checked = info;
    auto pf = parse_footer(footer, checked);
    if (!pf) return pf.error();
    auto fl = target.flush();
    if (!fl) return fl.error();

    // Durable read-back verification. Rewind the image and replay record
    // metadata rather than retaining a per-chunk checksum table in memory.
    if (::lseek(fd.value().n, static_cast<off_t>(kHeaderSize), SEEK_SET) < 0)
        return from_errno(Errc::read_failed, "cannot rewind image for restore verification", errno);
    std::uint64_t verified = 0;
    for (std::uint64_t i = 0; i < info.chunks; ++i) {
        if (Cancellation::requested()) return Error{Errc::unsafe, 0, "restore verification cancelled"};
        std::array<std::byte, kRecordSize> raw{};
        auto rr = fd_read_exact(fd.value().n, raw);
        if (!rr) return rr.error();
        auto rec = parse_record(raw, info, i);
        if (!rec) return rec.error();
        const auto n = static_cast<std::size_t>(rec.value().logical_len);
        auto target_span = std::span<std::byte>(buffer).first(n);
        auto tr = target.read_exact(rec.value().source_offset, target_span);
        if (!tr) return tr.error();
        if ((rec.value().flags & kChunkZero) != 0) {
            if (!all_zero(target_span)) return Error{
                Errc::io, 0, "restore read-back mismatch in zero chunk " + std::to_string(i)
            }
            ;
        } else {
            // Skip source payload after validating target against the record CRC.
            if (checksum(target_span) != rec.value().payload_crc)
                return Error{
                    Errc::io, 0, "restore read-back checksum mismatch at chunk " + std::to_string(i)
                }
            ;
            // stored_len is bounded by the validated 64 MiB chunk limit, so
            // it is representable by off_t on every supported Unix ABI.
            const auto skip = static_cast<off_t>(rec.value().stored_len);
            if (::lseek(fd.value().n, skip, SEEK_CUR) < 0)
                return from_errno(Errc::read_failed, "cannot skip image payload during verification", errno);
        }
        verified += n;
        if (progress) progress->update({
                                       "verifying restore", "read-back pass", verified, info.source_bytes,
                                       written + verified, written, i+1, info.chunks, verified
                                       }
                                      );
    }
    return checked;
}

} // namespace fsx::image
