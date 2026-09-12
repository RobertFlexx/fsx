#include "fsx/ext4_maintenance.hpp"
#include "fsx/cancel.hpp"
#include "fsx/endian.hpp"
#include "fsx/ext4_metadata.hpp"
#include "fsx/safety.hpp"
#include "internal.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace fsx::ext4 {
namespace {
constexpr std::uint32_t COMPAT_SPARSE_SUPER2 = 0x0200U;
constexpr std::uint32_t RO_SPARSE_SUPER = 0x0001U;

bool is_power(std::uint64_t n, std::uint64_t base) {
    if (n < 1) return false;
    while (n % base == 0) n /= base;
    return n == 1;
}

bool group_has_super(std::span<const std::byte> raw, std::uint64_t group) {
    if (group == 0) return true;
    const auto compat = read_le<std::uint32_t>(raw, 0x5c);
    const auto ro = read_le<std::uint32_t>(raw, 0x64);
    if ((compat & COMPAT_SPARSE_SUPER2) != 0) {
        const auto a = read_le<std::uint32_t>(raw, 0x24c);
        const auto b = read_le<std::uint32_t>(raw, 0x250);
        return group == a || group == b;
    }
    if ((ro & RO_SPARSE_SUPER) == 0) return true;
    if (group == 1) return true;
    return is_power(group, 3) || is_power(group, 5) || is_power(group, 7);
}

std::uint64_t count_set(std::span<const std::byte> bitmap, std::uint64_t bits) {
    std::uint64_t n = 0;
    for (std::uint64_t i = 0; i < bits; ++i)
        if (detail::bit_set(bitmap, i)) ++n;
    return n;
}

void set_desc_free_blocks(std::span<std::byte> gd, std::uint32_t v) {
    write_le16(gd, 12, static_cast<std::uint16_t>(v));
    if (gd.size() >= 46) write_le16(gd, 44, static_cast<std::uint16_t>(v >> 16U));
}

void set_desc_free_inodes(std::span<std::byte> gd, std::uint32_t v) {
    write_le16(gd, 14, static_cast<std::uint16_t>(v));
    if (gd.size() >= 48) write_le16(gd, 46, static_cast<std::uint16_t>(v >> 16U));
}

void set_u64_lohi(std::span<std::byte> raw,
                  std::size_t lo,
                  std::size_t hi,
                  std::uint64_t value) {
    write_le32(raw, lo, static_cast<std::uint32_t>(value));
    write_le32(raw, hi, static_cast<std::uint32_t>(value >> 32U));
}

Result<void> write_superblock_copies(BlockDevice& dev,
                                     const MetadataContext& ctx,
                                     std::span<const std::byte> raw) {
    if (raw.size() != 1024) return Error{Errc::invalid_argument, 0, "superblock copy must be 1024 bytes"};
    auto primary = std::vector<std::byte>(raw.begin(), raw.end());
    auto wr = write_raw_superblock(dev, primary);
    if (!wr) return wr.error();
    for (std::uint64_t group = 1; group < ctx.sb.groups_count(); ++group) {
        if (!group_has_super(primary, group)) continue;
        auto copy = primary;
        write_le16(copy, 0x5a, static_cast<std::uint16_t>(group));
        auto cs = update_superblock_checksum(copy);
        if (!cs) return cs.error();
        const auto off = detail::group_start_block(ctx.sb, group) * ctx.sb.block_size();
        wr = dev.write_exact(off, copy);
        if (!wr) return wr.error();
    }
    return dev.flush();
}

Result<void> write_group_descriptor_copies(BlockDevice& dev,
                                           const MetadataContext& ctx,
                                           std::span<const std::byte> super,
                                           std::uint64_t descriptor_group,
                                           std::span<const std::byte> descriptor) {
    if ((ctx.sb.feature_incompat & detail::INCOMPAT_META_BG) != 0)
        return Error{Errc::unsupported, 0, "meta_bg group-descriptor backup repair is not implemented"};
    auto primary = write_group_descriptor_raw(dev, ctx, descriptor_group, descriptor);
    if (!primary) return primary.error();
    const auto ds = static_cast<std::uint64_t>(ctx.sb.desc_size >= 32 ? ctx.sb.desc_size : 32);
    for (std::uint64_t backup_group = 1; backup_group < ctx.sb.groups_count(); ++backup_group) {
        if (!group_has_super(super, backup_group)) continue;
        const auto table_block = detail::group_start_block(ctx.sb, backup_group) + 1;
        const auto off = table_block * ctx.sb.block_size() + descriptor_group * ds;
        auto wr = dev.write_exact(off, descriptor);
        if (!wr) return wr.error();
    }
    return {};
}
} // namespace

Result<RepairReport> repair_metadata_counters(BlockDevice& dev,
                                              const RepairOptions& options,
                                              Progress* progress) {
    auto safe = require_offline_for_write(dev);
    if (!safe) return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds) return ds.error();
    if (ds.value().block_device && !options.allow_block_device)
        return Error{Errc::unsafe, 0, "block-device metadata repair requires --allow-block-device"};

    auto ctxr = load_metadata_context(dev, true);
    if (!ctxr) return ctxr.error();
    const auto ctx = ctxr.value();
    if ((ctx.sb.feature_incompat & detail::INCOMPAT_META_BG) != 0)
        return Error{Errc::unsupported, 0, "metadata repair currently refuses meta_bg"};
    if ((ctx.sb.feature_ro_compat & detail::RO_COMPAT_BIGALLOC) != 0)
        return Error{Errc::unsupported, 0, "metadata repair currently refuses bigalloc"};
    if ((ctx.sb.feature_incompat & detail::INCOMPAT_RECOVER) != 0)
        return Error{Errc::unsafe, 0, "filesystem journal recovery is required before offline repair"};

    auto raw_super = read_raw_superblock(dev);
    if (!raw_super) return raw_super.error();

    RepairReport report;
    const auto groups = ctx.sb.groups_count();
    for (std::uint64_t group = 0; group < groups; ++group) {
        if (Cancellation::requested())
            return Error{Errc::unsafe, 0, "metadata repair cancelled before the next group write"};
        auto gd = detail::read_group_desc(dev, ctx.sb, group);
        if (!gd) return gd.error();
        auto gd_raw = read_group_descriptor_raw(dev, ctx, group);
        if (!gd_raw) return gd_raw.error();
        auto bb = detail::read_block_bitmap(dev, ctx.sb, gd.value());
        if (!bb) return bb.error();
        auto ib = detail::read_inode_bitmap(dev, ctx.sb, gd.value());
        if (!ib) return ib.error();

        const auto valid_blocks = detail::group_block_count(ctx.sb, group);
        const auto allocated_blocks = count_set(bb.value(), valid_blocks);
        const auto free_blocks = valid_blocks - allocated_blocks;
        if (free_blocks > std::numeric_limits<std::uint32_t>::max())
            return Error{Errc::internal, 0, "group free-block count overflow"};

        const auto first_inode = group * static_cast<std::uint64_t>(ctx.sb.inodes_per_group) + 1;
        const auto remain = ctx.sb.inodes_count >= first_inode ? ctx.sb.inodes_count - first_inode + 1 : 0;
        const auto valid_inodes = std::min<std::uint64_t>(ctx.sb.inodes_per_group, remain);
        const auto allocated_inodes = count_set(ib.value(), valid_inodes);
        const auto free_inodes = valid_inodes - allocated_inodes;
        if (free_inodes > std::numeric_limits<std::uint32_t>::max())
            return Error{Errc::internal, 0, "group free-inode count overflow"};

        set_desc_free_blocks(gd_raw.value(), static_cast<std::uint32_t>(free_blocks));
        set_desc_free_inodes(gd_raw.value(), static_cast<std::uint32_t>(free_inodes));
        auto bcs = update_block_bitmap_checksum(ctx, gd_raw.value(), bb.value());
        if (!bcs) return bcs.error();
        auto ics = update_inode_bitmap_checksum(ctx, gd_raw.value(), ib.value());
        if (!ics) return ics.error();
        auto gcs = update_group_descriptor_checksum(ctx, group, gd_raw.value());
        if (!gcs) return gcs.error();
        auto wr = write_group_descriptor_copies(dev, ctx, raw_super.value(), group, gd_raw.value());
        if (!wr) return wr.error();

        report.free_blocks += free_blocks;
        report.free_inodes += free_inodes;
        ++report.groups_rewritten;
        if (progress)
            progress->update({
                             "repair-metadata", "group " + std::to_string(group + 1) + "/" + std::to_string(groups),
                             group + 1, groups, 0, 0, group + 1, groups});
    }

    set_u64_lohi(raw_super.value(), 0x0c, 0x158, report.free_blocks);
    write_le32(raw_super.value(), 0x10, static_cast<std::uint32_t>(report.free_inodes));
    auto sc = update_superblock_checksum(raw_super.value());
    if (!sc) return sc.error();
    auto sw = write_superblock_copies(dev, ctx, raw_super.value());
    if (!sw) return sw.error();
    report.superblock_rewritten = true;

    if (options.verify_after) {
        auto verify = verify_metadata(dev, false, progress);
        if (!verify) return verify.error();
        if (!verify.value().clean)
            return Error{
                Errc::corrupt, 0, "metadata verification still reports errors after conservative repair"
            }
        ;
        report.verified = true;
    }
    return report;
}

Result<void> set_volume_label(BlockDevice& dev,
                              const std::string& label,
                              bool allow_block_device) {
    if (label.size() > 16)
        return Error{Errc::invalid_argument, 0, "ext volume label is limited to 16 bytes"};
    auto safe = require_offline_for_write(dev);
    if (!safe) return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds) return ds.error();
    if (ds.value().block_device && !allow_block_device)
        return Error{Errc::unsafe, 0, "block-device tuning requires --allow-block-device"};
    auto ctxr = load_metadata_context(dev);
    if (!ctxr) return ctxr.error();
    const auto ctx = ctxr.value();
    auto raw = read_raw_superblock(dev);
    if (!raw) return raw.error();
    std::fill(raw.value().begin() + 0x78, raw.value().begin() + 0x88, std::byte{0});
    for (std::size_t i = 0; i < label.size(); ++i)
        raw.value()[0x78 + i] = std::byte(static_cast<unsigned char>(label[i]));
    auto c = update_superblock_checksum(raw.value());
    if (!c) return c.error();
    return write_superblock_copies(dev, ctx, raw.value());
}

} // namespace fsx::ext4
