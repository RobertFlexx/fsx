#include "fsx/device.hpp"
#include "fsx/fault.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/disk.h>
#include <sys/ioctl.h>
#elif defined(__APPLE__)
#include <sys/disk.h>
#include <sys/ioctl.h>
#endif

namespace fsx {
Result<BlockDevice> open_device_impl(std::string path, bool write);
namespace {
Result<DeviceGeometry> geometry_for(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) return from_errno(Errc::io, "fstat failed", errno);
    DeviceGeometry g{};
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
    g.is_block_device = S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode);
#else
    g.is_block_device = S_ISBLK(st.st_mode);
#endif
    g.read_only = (::fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY;

#if defined(__linux__)
    if (g.is_block_device) {
        unsigned long long bytes = 0;
        int logical = 512, physical = 512;
        if (::ioctl(fd, BLKGETSIZE64, &bytes) == 0) g.size_bytes = static_cast<std::uint64_t>(bytes);
        if (::ioctl(fd, BLKSSZGET, &logical) == 0
            && logical > 0) g.logical_sector = static_cast<std::uint32_t>(logical);
#ifdef BLKPBSZGET
        if (::ioctl(fd, BLKPBSZGET, &physical) == 0
            && physical > 0) g.physical_sector = static_cast<std::uint32_t>(physical);
#endif
    }
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    if (g.is_block_device) {
        off_t bytes = 0;
        u_int sectorsz = 512;
        if (::ioctl(fd, DIOCGMEDIASIZE, &bytes) == 0) g.size_bytes = static_cast<std::uint64_t>(bytes);
        if (::ioctl(fd, DIOCGSECTORSIZE,
                    &sectorsz) == 0) g.logical_sector = static_cast<std::uint32_t>(sectorsz);
        g.physical_sector = g.logical_sector;
    }
#elif defined(__APPLE__)
    if (g.is_block_device) {
        std::uint32_t block_size = 512;
        std::uint64_t block_count = 0;
        if (::ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) == 0 && block_size > 0) g.logical_sector = block_size;
        if (::ioctl(fd, DKIOCGETBLOCKCOUNT,
                    &block_count) == 0) g.size_bytes = block_count * static_cast<std::uint64_t>(g.logical_sector);
        g.physical_sector = g.logical_sector;
    }
#endif
    if (!g.is_block_device || g.size_bytes == 0) {
        if (S_ISREG(st.st_mode)) g.size_bytes = static_cast<std::uint64_t>(st.st_size);
        else {
            off_t end = ::lseek(fd, 0, SEEK_END);
            if (end >= 0) {
                g.size_bytes = static_cast<std::uint64_t>(end);
                (void)::lseek(fd, 0, SEEK_SET);
            }
        }
    }
    return g;
}

Result<BlockDevice> open_impl(std::string path, bool write) {
    return open_device_impl(std::move(path), write);
}

}


Result<BlockDevice> open_device_impl(std::string path, bool write) {
    int flags = write ? O_RDWR : O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = ::open(path.c_str(), flags);
    if (fd < 0) {
        Errc c = errno == EACCES || errno == EPERM ? Errc::permission : Errc::open_failed;
        return from_errno(c, "cannot open " + path + ": " + std::strerror(errno), errno);
    }
    auto g = geometry_for(fd);
    if (!g) {
        ::close(fd);
        return g.error();
    }
    BlockDevice d;
    d.fd_ = fd;
    d.path_ = std::move(path);
    d.geometry_ = g.value();
    d.writable_ = write;
    return d;
}

BlockDevice::BlockDevice(BlockDevice && o) noexcept : fd_(o.fd_),
    path_(std::move(o.path_)), geometry_(o.geometry_), writable_(o.writable_) {
        o.fd_ = -1;
    }
BlockDevice& BlockDevice::operator = (BlockDevice && o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = o.fd_;
        path_ = std::move(o.path_);
        geometry_ = o.geometry_;
        writable_ = o.writable_;
        o.fd_ = -1;
    }
    return *this;
}
BlockDevice::~BlockDevice() {
    if (fd_ >= 0) ::close(fd_);
}
Result<BlockDevice> BlockDevice::open_read(std::string path) {
    return open_impl(std::move(path), false);
}
Result<BlockDevice> BlockDevice::open_write(std::string path) {
    return open_impl(std::move(path), true);
}
Result<BlockDevice> BlockDevice::create_file(std::string path, std::uint64_t size_bytes) {
    if (size_bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
        return Error{Errc::unsupported, 0,"requested file size exceeds host off_t"};
    int flags = O_RDWR | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) return from_errno(Errc::open_failed,
                                  "cannot create " + path + ": " + std::strerror(errno), errno);
    if (::ftruncate(fd, static_cast<off_t>(size_bytes)) != 0) {
        const auto e = errno;
        (void)::close(fd);
        (void)::unlink(path.c_str());
        return from_errno(Errc::write_failed, "cannot size " + path + ": " + std::strerror(e), e);
    }
    auto g = geometry_for(fd);
    if (!g) {
        (void)::close(fd);
        (void)::unlink(path.c_str());
        return g.error();
    }
    BlockDevice d;
    d.fd_ = fd;
    d.path_ = std::move(path);
    d.geometry_ = g.value();
    d.writable_ = true;
    return d;
}

Result<void> BlockDevice::read_exact(std::uint64_t off, std::span<std::byte> out) const {
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::read, fault::Timing::before);
        if (!f) return f.error();
    }
    if (off > geometry_.size_bytes || static_cast<std::uint64_t>(out.size()) > geometry_.size_bytes - off)
        return Error{Errc::short_io, 0, "read range exceeds device geometry"};
    if (off > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        static_cast<std::uint64_t>(out.size()) > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - off)
        return Error{Errc::unsupported, 0, "read range exceeds host off_t"};
    std::size_t done = 0;
    while (done < out.size()) {
        auto want = std::min<std::size_t>(out.size()-done,
                                          static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto pos = off + done;
        ssize_t n = ::pread(fd_, out.data()+done, want, static_cast<off_t>(pos));
        if (n < 0) {
            if (errno == EINTR) continue;
            return from_errno(Errc::read_failed,
                              "read failed at byte " + std::to_string(pos) + ": " + std::strerror(errno), errno);
        }
        if (n == 0) return Error{Errc::short_io, 0,"unexpected end of device at byte " + std::to_string(pos)};
        done += static_cast<std::size_t>(n);
    }
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::read, fault::Timing::after);
        if (!f) return f.error();
    }
    return {};
}
Result<void> BlockDevice::write_exact(std::uint64_t off, std::span<const std::byte> in) {
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::write, fault::Timing::before);
        if (!f) return f.error();
    }
    if (!writable_) return Error{Errc::permission, 0,"device is read-only"};
    if (off > geometry_.size_bytes || static_cast<std::uint64_t>(in.size()) > geometry_.size_bytes - off)
        return Error{Errc::short_io, 0, "write range exceeds device geometry"};
    if (off > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        static_cast<std::uint64_t>(in.size()) > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - off)
        return Error{Errc::unsupported, 0, "write range exceeds host off_t"};
    std::size_t done = 0;
    while (done < in.size()) {
        auto want = std::min<std::size_t>(in.size()-done,
                                          static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const auto pos = off + done;
        ssize_t n = ::pwrite(fd_, in.data()+done, want, static_cast<off_t>(pos));
        if (n<0) {
            if (errno == EINTR) continue;
            return from_errno(Errc::write_failed,
                              "write failed at byte " + std::to_string(pos) + ": " + std::strerror(errno), errno);
        }
        if (n == 0) return Error{Errc::short_io, 0,"short write"};
        done += static_cast<std::size_t>(n);
    }
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::write, fault::Timing::after);
        if (!f) return f.error();
    }
    return {};
}
Result<void> BlockDevice::flush() {
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::flush, fault::Timing::before);
        if (!f) return f.error();
    }
    if (::fsync(fd_) != 0) return from_errno(Errc::flush_failed,
                                             "fsync failed: " + std::string(std::strerror(errno)), errno);
    if (fault::enabled()) {
        auto f = fault::point(fault::Operation::flush, fault::Timing::after);
        if (!f) return f.error();
    }
    return {};
}
}
