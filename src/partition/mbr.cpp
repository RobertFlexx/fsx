#include "fsx/mbr.hpp"
#include "fsx/endian.hpp"
#include "fsx/file_util.hpp"
#include "fsx/safety.hpp"
#include "fsx/uuid.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <unistd.h>

namespace fsx::mbr {
namespace {
constexpr std::size_t PART_OFF = 446;
Result<std::array<std::byte, 512>> read_sector(BlockDevice&dev) {
    std::array<std::byte, 512>b{};
    auto r = dev.read_exact(0, b);
    if (!r)return r.error();
    return b;
}
Result<Table> decode(std::span<const std::byte>b) {
    if (b.size()<512)return Error{
        Errc::corrupt, 0,"MBR sector truncated"
    }
    ;
    Table t;
    t.signature_ok = std::to_integer<unsigned>(b[510]) == 0x55U && std::to_integer<unsigned>(b[511]) == 0xaaU;
    t.disk_signature = read_le<std::uint32_t>(b, 440);
    unsigned nonprotect = 0, protect = 0;
    for (unsigned i = 0; i<4; ++i) {
        const auto o = PART_OFF+i*16;
        const auto type = std::to_integer<std::uint8_t>(b[o+4]);
        const auto count = read_le<std::uint32_t>(b, o+12);
        if (type == 0 || count == 0)continue;
        Partition p;
        p.index = i+1;
        p.bootable = std::to_integer<unsigned>(b[o]) == 0x80U;
        p.type = type;
        p.first_lba = read_le<std::uint32_t>(b, o+8);
        p.sectors = count;
        t.partitions.push_back(p);
        if (type == 0xeeU)++protect;
        else ++nonprotect;
    }
    t.protective = protect>0;
    t.hybrid = protect>0 && nonprotect>0;
    return t;
}
Result<void> validate(const Table&t, std::uint64_t disk_sectors) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>>r;
    for (const auto&p:t.partitions) {
        if (p.sectors == 0)continue;
        const auto last = p.last_lba();
        if (last >= disk_sectors)return Error{
            Errc::invalid_argument, 0,"MBR partition exceeds disk"
        }
        ;
        if (p.type != 0xeeU)r.emplace_back(p.first_lba, last);
    }
    std::sort(r.begin(), r.end());
    for (std::size_t i = 1; i<r.size(); ++i)if (r[i].first <= r[i-1].second)return Error{
        Errc::invalid_argument, 0,"MBR partitions overlap"
    }
    ;
    return {};
}
}
Result<Table> read_table(BlockDevice&dev) {
    auto b = read_sector(dev);
    if (!b)return b.error();
    auto t = decode(b.value());
    if (!t)return t.error();
    if (!t.value().signature_ok)return Error{
        Errc::unsupported, 0,"not an MBR partition table"
    }
    ;
    return t;
}
Result<void> resize_partition(BlockDevice&dev, std::uint32_t index,
                              std::uint32_t first, std::uint32_t sectors, const MutationOptions&o) {
    if (index<1 || index>4 || sectors == 0)return Error{
        Errc::invalid_argument, 0,"invalid MBR resize arguments"
    }
    ;
    if (o.backup_sector_path.empty())return Error{
        Errc::invalid_argument, 0,"MBR mutation requires --backup-sector=FILE"
    }
    ;
    auto safe = require_disk_offline_for_write(dev);
    if (!safe)return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds)return ds.error();
    if (ds.value().block_device && !o.allow_block_device)return Error{
        Errc::unsafe, 0,"MBR mutation on block device requires --allow-block-device"
    }
    ;
    auto b = read_sector(dev);
    if (!b)return b.error();
    auto old = decode(b.value());
    if (!old)return old.error();
    if (!old.value().signature_ok)return Error{
        Errc::corrupt, 0,"MBR signature invalid"
    }
    ;
    if (old.value().protective || old.value().hybrid)return Error{
        Errc::unsafe, 0,"refusing MBR mutation on protective/hybrid GPT media; use GPT commands"
    }
    ;
    const auto off = PART_OFF+static_cast<std::size_t>(index-1)*16;
    if (std::to_integer<std::uint8_t>(b.value()[off+4]) == 0)return Error{
        Errc::not_found, 0,"MBR partition entry unused"
    }
    ;
    write_le32(b.value(), off+8, first);
    write_le32(b.value(), off+12, sectors);
    auto nt = decode(b.value());
    if (!nt)return nt.error();
    auto v = validate(nt.value(),
                      dev.geometry().size_bytes/(dev.geometry().logical_sector?dev.geometry().logical_sector:512U));
    if (!v)return v.error();
    // Publish a durable undo sector before touching the disk. The final backup
    // path is never exposed as a partially written file and is never clobbered.
    auto orig = read_sector(dev);
    if (!orig)return orig.error();
    const auto temp = o.backup_sector_path+".part."+format_uuid(random_uuid_v4());
    struct Cleanup {
        std::string path;
        bool keep{
            false
        }
        ;
        ~Cleanup() {
            if (!keep) (void)::unlink(path.c_str());
        }
    }
    cleanup{
        temp
    }
    ;
    auto backup = BlockDevice::create_file(temp, 512);
    if (!backup)return backup.error();
    auto wb = backup.value().write_exact(0, orig.value());
    if (!wb)return wb.error();
    auto bf = backup.value().flush();
    if (!bf)return bf.error();
    auto pub = publish_file_noreplace(temp, o.backup_sector_path,"MBR undo sector");
    if (!pub)return pub.error();
    cleanup.keep = true;
    auto wr = dev.write_exact(0, b.value());
    if (!wr)return wr.error();
    return dev.flush();
}
Result<void> restore_sector(BlockDevice&dev, const std::string&path, bool allow) {
    auto safe = require_disk_offline_for_write(dev);
    if (!safe)return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds)return ds.error();
    if (ds.value().block_device && !allow)return Error{
        Errc::unsafe, 0,"MBR restore on block device requires --allow-block-device"
    }
    ;
    auto b = BlockDevice::open_read(path);
    if (!b)return b.error();
    if (b.value().geometry().size_bytes != 512)return Error{
        Errc::corrupt, 0,"MBR backup must contain exactly one 512-byte sector"
    }
    ;
    std::array<std::byte, 512>sec{};
    auto rr = b.value().read_exact(0, sec);
    if (!rr)return rr.error();
    auto t = decode(sec);
    if (!t || !t.value().signature_ok)return Error{
        Errc::corrupt, 0,"MBR backup sector is invalid"
    }
    ;
    auto wr = dev.write_exact(0, sec);
    if (!wr)return wr.error();
    return dev.flush();
}
}
