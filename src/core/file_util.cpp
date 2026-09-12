#include "fsx/file_util.hpp"
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif
#endif

namespace fsx {
namespace {

bool no_replace_unavailable(int e) noexcept {
    return e == ENOSYS || e == EINVAL || e == EOPNOTSUPP
#ifdef ENOTSUP
        || e == ENOTSUP
#endif
        ;
}

Result<void> publish_with_link(const std::string& temp_path,
                               const std::string& final_path,
                               std::string_view object_name) {
    if (::link(temp_path.c_str(), final_path.c_str()) != 0) {
        if (errno == EEXIST)
            return Error{Errc::unsafe, 0, "refusing to overwrite existing " + std::string(object_name)};
        return from_errno(Errc::write_failed,
                          "cannot publish " + std::string(object_name) +
                          " without replacement: " + std::string(std::strerror(errno)),
                          errno);
    }

    // Persist the new name before deleting the temporary name. If power fails
    // between the two directory fsyncs, two names can remain, but an already
    // published object is never intentionally left with zero durable names.
    auto sync = fsync_parent_directory(final_path, object_name);
    if (!sync) return sync.error();
    if (::unlink(temp_path.c_str()) != 0)
        return from_errno(Errc::write_failed,
                          std::string(object_name) +
                          " was published but its temporary link could not be removed: " +
                          std::string(std::strerror(errno)),
                          errno);
    sync = fsync_parent_directory(final_path, object_name);
    if (!sync) return sync.error();
    return {};
}

} // namespace

Result<void> fsync_parent_directory(const std::string& path, std::string_view object_name) {
    std::filesystem::path p(path);
    auto parent = p.parent_path();
    if (parent.empty()) parent = ".";
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    const int fd = ::open(parent.c_str(), flags);
    if (fd < 0)
        return from_errno(Errc::open_failed,
                          "cannot open " + std::string(object_name) +
                          " parent directory: " + std::string(std::strerror(errno)),
                          errno);
    const int rc = ::fsync(fd);
    const int saved = errno;
    (void)::close(fd);
    if (rc != 0)
        return from_errno(Errc::flush_failed,
                          "cannot fsync " + std::string(object_name) +
                          " parent directory: " + std::string(std::strerror(saved)),
                          saved);
    return {};
}

Result<void> publish_file_noreplace(const std::string& temp_path,
                                    const std::string& final_path,
                                    std::string_view object_name) {
#if defined(__linux__) && defined(SYS_renameat2)
    // Linux VFS provides true atomic no-replace rename semantics, including
    // filesystems that do not implement hard links (important for removable
    // FAT/exFAT media used for rescue images).
    if (::syscall(SYS_renameat2, AT_FDCWD, temp_path.c_str(),
                  AT_FDCWD, final_path.c_str(), RENAME_NOREPLACE) == 0) {
        auto sync = fsync_parent_directory(final_path, object_name);
        if (!sync) return sync.error();
        return {};
    }
    const int rename_error = errno;
    if (rename_error == EEXIST)
        return Error{Errc::unsafe, 0, "refusing to overwrite existing " + std::string(object_name)};
    if (!no_replace_unavailable(rename_error))
        return from_errno(Errc::write_failed,
                          "cannot atomically publish " + std::string(object_name) +
                          ": " + std::string(std::strerror(rename_error)),
                          rename_error);
#endif

    return publish_with_link(temp_path, final_path, object_name);
}

} // namespace fsx
