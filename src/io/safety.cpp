#include "fsx/safety.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#elif defined(__NetBSD__)
#include <sys/param.h>
#include <sys/statvfs.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__APPLE__) || defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#endif

namespace fsx {
namespace {
std::string canonical_or_self(const std::string& path) {
    char b[4096]{};
    return ::realpath(path.c_str(), b) ? std::string(b) : path;
}

#if defined(__linux__)
std::filesystem::path sysfs_device_path(unsigned long maj, unsigned long min) {
    std::error_code ec;
    const auto p = std::filesystem::path("/sys/dev/block") /
        (std::to_string(maj) + ":" + std::to_string(min));
    const auto c = std::filesystem::canonical(p, ec);
    return ec ? std::filesystem::path{} : c;
}

bool same_or_descendant(const std::filesystem::path& base,
                        const std::filesystem::path& candidate) {
    if (base.empty() || candidate.empty()) return false;
    auto bi = base.begin();
    auto ci = candidate.begin();
    for (; bi != base.end(); ++bi, ++ci) {
        if (ci == candidate.end() || *bi != *ci) return false;
    }
    return true;
}

bool parse_major_minor(const std::string& text,
                       unsigned long& maj,
                       unsigned long& min) {
    const auto colon = text.find(':');
    if (colon == std::string::npos) return false;
    char* e1 = nullptr;
    char* e2 = nullptr;
    maj = std::strtoul(text.substr(0, colon).c_str(), &e1, 10);
    min = std::strtoul(text.substr(colon + 1).c_str(), &e2, 10);
    return e1 && *e1 == '\0' && e2 && *e2 == '\0';
}

void collect_backing_devices(const std::filesystem::path& sys,
                             std::set<std::filesystem::path>& out,
                             std::set<std::filesystem::path>& visiting) {
    if (sys.empty()) return;
    std::error_code ec;
    auto canonical = std::filesystem::canonical(sys, ec);
    if (ec) canonical = sys;
    if (!visiting.insert(canonical).second) return;

    const auto slaves = canonical / "slaves";
    bool had_slave = false;
    ec.clear();
    if (std::filesystem::exists(slaves, ec) && !ec) {
        std::filesystem::directory_iterator it(slaves, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            had_slave = true;
            std::error_code sec;
            auto child = std::filesystem::canonical(it->path(), sec);
            if (!sec) collect_backing_devices(child, out, visiting);
        }
    }
    // A device with no slaves is a physical/leaf backing object.  Keep the
    // exact leaf here: sibling partitions on the same disk are disjoint and
    // must not be rejected merely because they share a parent disk.  The
    // ancestry comparison below still catches whole-disk <-> partition
    // overlap, including through dm/md slave chains.
    if (!had_slave) {
        out.insert(canonical);
    }
    visiting.erase(canonical);
}

std::set<std::filesystem::path> backing_set(dev_t d) {
    std::set<std::filesystem::path> out, visiting;
    collect_backing_devices(sysfs_device_path(
                                              static_cast<unsigned long>(major(d)),
                                              static_cast<unsigned long>(minor(d))),
                            out, visiting);
    return out;
}
#endif
} // namespace

Result<DeviceSafety> inspect_device_safety(const BlockDevice& dev) {
    DeviceSafety out;
    struct stat st{};
    if (::fstat(dev.native_fd(), &st) != 0)
        return from_errno(Errc::io,
                          "fstat failed while checking device safety: " +
                          std::string(std::strerror(errno)),
                          errno);
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
    out.block_device = S_ISBLK(st.st_mode) || S_ISCHR(st.st_mode);
#else
    out.block_device = S_ISBLK(st.st_mode);
#endif
    out.regular_file = S_ISREG(st.st_mode);

#if defined(__linux__)
    if (!out.block_device) return out;
    const auto want_major = static_cast<unsigned long>(major(st.st_rdev));
    const auto want_minor = static_cast<unsigned long>(minor(st.st_rdev));
    const auto want_sys = sysfs_device_path(want_major, want_minor);

    std::ifstream in("/proc/self/mountinfo");
    if (!in) return Error{Errc::io, 0, "cannot read /proc/self/mountinfo"};
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string id, parent, majmin, root, mountpoint;
        if (!(ss >> id >> parent >> majmin >> root >> mountpoint)) continue;
        unsigned long ma = 0;
        unsigned long mi = 0;
        if (!parse_major_minor(majmin, ma, mi)) continue;
        if (ma == want_major && mi == want_minor) {
            out.mounted = true;
            out.mountpoint = mountpoint;
            continue;
        }
        const auto mounted_sys = sysfs_device_path(ma, mi);
        if (same_or_descendant(want_sys, mounted_sys)) {
            out.descendant_mounted = true;
            out.active_descendants.push_back(majmin + " mounted at " + mountpoint);
        }
    }

    std::ifstream swaps("/proc/swaps");
    if (swaps) {
        std::getline(swaps, line);
        const auto want = canonical_or_self(dev.path());
        while (std::getline(swaps, line)) {
            std::istringstream ss(line);
            std::string source;
            if (!(ss >> source)) continue;
            if (canonical_or_self(source) == want) {
                out.active_swap = true;
                continue;
            }
            struct stat swst{};
            if (::stat(source.c_str(), &swst) == 0 && S_ISBLK(swst.st_mode)) {
                const auto swap_sys = sysfs_device_path(
                                                        static_cast<unsigned long>(major(swst.st_rdev)),
                                                        static_cast<unsigned long>(minor(swst.st_rdev)));
                if (same_or_descendant(want_sys, swap_sys)) {
                    out.descendant_swap = true;
                    out.active_descendants.push_back(source + " active as swap");
                }
            }
        }
    }

    const std::filesystem::path holders =
        "/sys/dev/block/" + std::to_string(want_major) + ":" +
        std::to_string(want_minor) + "/holders";
    std::error_code ec;
    if (std::filesystem::exists(holders, ec) && !ec) {
        for (const auto& e : std::filesystem::directory_iterator(holders, ec)) {
            if (ec) break;
            out.holders.push_back(e.path().filename().string());
        }
        out.has_holders = !out.holders.empty();
    }
#elif defined(__NetBSD__)
    if (!out.block_device) return out;
    struct statvfs* mounts = nullptr;
    const int n = ::getmntinfo(&mounts, MNT_NOWAIT);
    if (n < 0) return from_errno(Errc::io, "getmntinfo failed", errno);
    const std::string want = canonical_or_self(dev.path());
    for (int i = 0; i < n; ++i) {
        if (canonical_or_self(mounts[i].f_mntfromname) == want) {
            out.mounted = true;
            out.mountpoint = mounts[i].f_mntonname;
            return out;
        }
    }
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__APPLE__) || defined(__OpenBSD__)
    if (!out.block_device) return out;
    struct statfs* mounts = nullptr;
    const int n = ::getmntinfo(&mounts, MNT_NOWAIT);
    if (n < 0) return from_errno(Errc::io, "getmntinfo failed", errno);
    const std::string want = canonical_or_self(dev.path());
    for (int i = 0; i < n; ++i) {
        if (canonical_or_self(mounts[i].f_mntfromname) == want) {
            out.mounted = true;
            out.mountpoint = mounts[i].f_mntonname;
            return out;
        }
    }
#else
    if (out.block_device)
        return Error{Errc::unsupported, 0,
            "mount-state detection is not implemented on this Unix; refusing destructive access"};
#endif
    return out;
}

Result<void> require_offline_for_write(const BlockDevice& dev) {
    auto sr = inspect_device_safety(dev);
    if (!sr) return sr.error();
    const auto& s = sr.value();
    if (s.mounted)
        return Error{Errc::unsafe, 0,
            "device is mounted at " + s.mountpoint +
                "; offline filesystem writes are refused"};
    if (s.active_swap)
        return Error{Errc::unsafe, 0, "device is active swap; writes are refused"};
    if (s.descendant_mounted || s.descendant_swap) {
        std::string names;
        for (std::size_t i = 0; i < s.active_descendants.size(); ++i) {
            if (i) names += ", ";
            names += s.active_descendants[i];
        }
        return Error{Errc::unsafe, 0,
            "device has active child partitions (" + names +
                "); whole-device writes are refused"};
    }
    if (s.has_holders) {
        std::string names;
        for (std::size_t i = 0; i < s.holders.size(); ++i) {
            if (i) names += ",";
            names += s.holders[i];
        }
        return Error{Errc::unsafe, 0,
            "device has active kernel holders (" + names +
                "); writes are refused"};
    }
    if (::flock(dev.native_fd(), LOCK_EX | LOCK_NB) != 0)
        return from_errno(Errc::unsafe,
                          "cannot acquire exclusive device lock: " +
                          std::string(std::strerror(errno)),
                          errno);
    return {};
}

Result<void> require_disk_offline_for_write(const BlockDevice& dev) {
    auto base = require_offline_for_write(dev);
    if (!base) return base.error();
#if defined(__linux__)
    // Linux descendant activity is already resolved through sysfs ancestry by
    // inspect_device_safety(), including mounted partitions and swap children.
    return {};
#elif defined(__NetBSD__)
    struct statvfs* mounts = nullptr;
    const int n = ::getmntinfo(&mounts, MNT_NOWAIT);
    if (n < 0) return from_errno(Errc::io, "getmntinfo failed", errno);
    const auto want = canonical_or_self(dev.path());
    for (int i = 0; i < n; ++i) {
        const auto source = canonical_or_self(mounts[i].f_mntfromname);
        if (source.size() > want.size() && source.compare(0, want.size(), want) == 0)
            return Error{
                Errc::unsafe, 0, "disk has mounted child device " + source + "; partition-table writes are refused"
            }
        ;
    }
    return {};
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__APPLE__) || defined(__OpenBSD__)
    struct statfs* mounts = nullptr;
    const int n = ::getmntinfo(&mounts, MNT_NOWAIT);
    if (n < 0) return from_errno(Errc::io, "getmntinfo failed", errno);
    const auto want = canonical_or_self(dev.path());
    for (int i = 0; i < n; ++i) {
        const auto source = canonical_or_self(mounts[i].f_mntfromname);
        if (source.size() > want.size() && source.compare(0, want.size(), want) == 0)
            return Error{
                Errc::unsafe, 0, "disk has mounted child device " + source + "; partition-table writes are refused"
            }
        ;
    }
    return {};
#else
    return Error{Errc::unsupported, 0, "whole-disk child mount detection is unavailable on this Unix"};
#endif
}

Result<void> require_nonoverlapping_devices(const BlockDevice& source,
                                            const BlockDevice& target) {
    struct stat ss{}, ts{};
    if (::fstat(source.native_fd(), &ss) != 0)
        return from_errno(Errc::io,
                          "fstat failed for clone source: " +
                          std::string(std::strerror(errno)),
                          errno);
    if (::fstat(target.native_fd(), &ts) != 0)
        return from_errno(Errc::io,
                          "fstat failed for clone target: " +
                          std::string(std::strerror(errno)),
                          errno);

    if (ss.st_dev == ts.st_dev && ss.st_ino == ts.st_ino)
        return Error{Errc::unsafe, 0,
            "source and target refer to the same object"};

#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
    const bool sblock = S_ISBLK(ss.st_mode) || S_ISCHR(ss.st_mode);
    const bool tblock = S_ISBLK(ts.st_mode) || S_ISCHR(ts.st_mode);
#else
    const bool sblock = S_ISBLK(ss.st_mode);
    const bool tblock = S_ISBLK(ts.st_mode);
#endif
    if (!sblock || !tblock) return {};

    if (ss.st_rdev == ts.st_rdev)
        return Error{Errc::unsafe, 0,
            "source and target refer to the same block device"};

#if defined(__linux__)
    const auto sp = sysfs_device_path(
                                      static_cast<unsigned long>(major(ss.st_rdev)),
                                      static_cast<unsigned long>(minor(ss.st_rdev)));
    const auto tp = sysfs_device_path(
                                      static_cast<unsigned long>(major(ts.st_rdev)),
                                      static_cast<unsigned long>(minor(ts.st_rdev)));
    if (sp.empty() || tp.empty())
        return Error{Errc::unsupported, 0,
            "cannot resolve block-device ancestry; refusing potentially overlapping copy"};
    if (same_or_descendant(sp, tp) || same_or_descendant(tp, sp))
        return Error{Errc::unsafe, 0,
            "source and target are a disk/partition overlap"};

    const auto sb = backing_set(ss.st_rdev);
    const auto tb = backing_set(ts.st_rdev);
    if (sb.empty() || tb.empty())
        return Error{Errc::unsupported, 0,
            "cannot resolve block-device backing storage; refusing potentially overlapping copy"};
    for (const auto& a : sb) {
        for (const auto& b : tb) {
            if (a == b || same_or_descendant(a, b) || same_or_descendant(b, a))
                return Error{Errc::unsafe, 0,
                    "source and target share overlapping block storage"};
        }
    }
#endif
    return {};
}

} // namespace fsx
