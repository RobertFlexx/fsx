#include "fsx/gpt.hpp"
#include "fsx/crc.hpp"
#include "fsx/endian.hpp"
#include "fsx/file_util.hpp"
#include "fsx/safety.hpp"
#include "fsx/uuid.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

namespace fsx::gpt {
namespace {
constexpr std::array<char, 8> SIG = {'E','F','I',' ','P','A','R','T'};

struct RawSide {
    bool signature_ok{false};
    bool header_crc_ok{false};
    bool entries_crc_ok{false};
    std::uint64_t lba{};
    std::uint64_t current_lba{};
    std::uint64_t backup_lba{};
    std::uint64_t first_usable{};
    std::uint64_t last_usable{};
    std::uint64_t entries_lba{};
    std::uint32_t revision{};
    std::uint32_t header_size{};
    std::uint32_t entry_count{};
    std::uint32_t entry_size{};
    std::array<std::byte, 16> disk_guid{};
    std::vector<std::byte> header;
    std::vector<std::byte> entries;
};

Result<void> validate_side_geometry(const RawSide& s, std::uint64_t total, std::uint32_t ss);

std::string utf16le_name(std::span<const std::byte> b) {
    std::string out;
    for (std::size_t i = 0; i+1<b.size(); i+=2) {
        const auto c = read_le<std::uint16_t>(b, i);
        if (c == 0)break;
        if (c<0x80)out.push_back(static_cast<char>(c));
        else out.push_back('?');
    }
    return out;
}
bool zero_guid(const std::array<std::byte, 16>& g) {
    return std::all_of(g.begin(), g.end(), [](std::byte b) {
                       return b == std::byte{
                       0
                       }
                       ;
                       }
                      );
}

bool protective_mbr_ok(BlockDevice& dev) {
    const auto ss = dev.geometry().logical_sector?dev.geometry().logical_sector:512U;
    if (ss<512)return false;
    std::vector<std::byte>b(ss);
    if (!dev.read_exact(0, b))return false;
    if (std::to_integer<unsigned>(b[510]) != 0x55U || std::to_integer<unsigned>(b[511]) != 0xaaU)return false;
    for (unsigned i = 0; i<4; ++i) {
        const auto off = 446U+i*16U;
        if (std::to_integer<unsigned>(b[off+4]) == 0xeeU)return true;
    }
    return false;
}

Result<RawSide> read_side(BlockDevice& dev, std::uint64_t lba) {
    const auto ss = dev.geometry().logical_sector?dev.geometry().logical_sector:512U;
    const auto total = dev.geometry().size_bytes/ss;
    if (lba >= total)return Error{Errc::corrupt, 0,"GPT header LBA is outside device"};
    RawSide out;
    out.lba = lba;
    out.header.resize(ss);
    auto rr = dev.read_exact(lba*ss, out.header);
    if (!rr)return rr.error();
    out.signature_ok = true;
    for (std::size_t i = 0; i<SIG.size(); ++i)if (std::to_integer<char>(out.header[i]) != SIG[i]) {
        out.signature_ok = false;
        break;
    }
    if (!out.signature_ok)return out;
    out.revision = read_le<std::uint32_t>(out.header, 8);
    out.header_size = read_le<std::uint32_t>(out.header, 12);
    if (out.header_size<92 || out.header_size>ss)return Error{Errc::corrupt, 0,"invalid GPT header size"};
    const auto stored = read_le<std::uint32_t>(out.header, 16);
    auto hc = out.header;
    write_le32(hc, 16, 0);
    const auto calc = crc32_ieee(std::span<const std::byte>(hc).first(out.header_size))^0xffffffffU;
    out.header_crc_ok = stored == calc;
    out.current_lba = read_le<std::uint64_t>(out.header, 24);
    out.backup_lba = read_le<std::uint64_t>(out.header, 32);
    out.first_usable = read_le<std::uint64_t>(out.header, 40);
    out.last_usable = read_le<std::uint64_t>(out.header, 48);
    std::copy_n(out.header.begin()+56, 16, out.disk_guid.begin());
    out.entries_lba = read_le<std::uint64_t>(out.header, 72);
    out.entry_count = read_le<std::uint32_t>(out.header, 80);
    out.entry_size = read_le<std::uint32_t>(out.header, 84);
    const auto entries_crc = read_le<std::uint32_t>(out.header, 88);
    if (out.current_lba != lba)return Error{
        Errc::corrupt, 0,"GPT header current-LBA field does not match its location"
    }
    ;
    if (out.entry_size<128 || out.entry_size>4096 || out.entry_count == 0 || out.entry_count>16384)return Error{
        Errc::corrupt, 0,"unreasonable GPT entry geometry"
    }
    ;
    const auto bytes = static_cast<std::uint64_t>(out.entry_count)*out.entry_size;
    if (bytes>64ULL*1024ULL*1024ULL)return Error{
        Errc::corrupt, 0,"GPT entry array too large"
    }
    ;
    const auto sectors = (bytes+ss-1)/ss;
    if (out.entries_lba >= total || sectors>total-out.entries_lba)return Error{
        Errc::corrupt, 0,"GPT entry array lies outside device"
    }
    ;
    out.entries.resize(static_cast<std::size_t>(bytes));
    rr = dev.read_exact(out.entries_lba*ss, out.entries);
    if (!rr)return rr.error();
    out.entries_crc_ok = (crc32_ieee(out.entries)^0xffffffffU) == entries_crc;
    auto vg = validate_side_geometry(out, total, ss);
    if (!vg)return vg.error();
    return out;
}

Table table_from_side(const RawSide& side, bool pmbr) {
    Table t;
    t.current_lba = side.current_lba;
    t.backup_lba = side.backup_lba;
    t.first_usable_lba = side.first_usable;
    t.last_usable_lba = side.last_usable;
    t.header_size = side.header_size;
    t.entry_count = side.entry_count;
    t.entry_size = side.entry_size;
    t.disk_guid = side.disk_guid;
    t.protective_mbr_ok = pmbr;
    for (std::uint32_t i = 0; i<side.entry_count; ++i) {
        auto ent = std::span<const std::byte>(side.entries).subspan(static_cast<std::size_t>(i)*side.entry_size,
                                                                    side.entry_size);
        Partition p;
        std::copy_n(ent.begin(), 16, p.type_guid.begin());
        if (zero_guid(p.type_guid))continue;
        std::copy_n(ent.begin()+16, 16, p.unique_guid.begin());
        p.index = i+1;
        p.first_lba = read_le<std::uint64_t>(ent, 32);
        p.last_lba = read_le<std::uint64_t>(ent, 40);
        p.attributes = read_le<std::uint64_t>(ent, 48);
        p.name = utf16le_name(ent.subspan(56, std::min<std::size_t>(72, side.entry_size-56)));
        t.partitions.push_back(std::move(p));
    }
    return t;
}

Result<void> validate_entries(const RawSide& s) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    for (std::uint32_t i = 0; i<s.entry_count; ++i) {
        auto ent = std::span<const std::byte>(s.entries).subspan(static_cast<std::size_t>(i)*s.entry_size,
                                                                 s.entry_size);
        std::array<std::byte, 16> type{};
        std::copy_n(ent.begin(), 16, type.begin());
        if (zero_guid(type))continue;
        const auto first = read_le<std::uint64_t>(ent, 32), last = read_le<std::uint64_t>(ent, 40);
        if (first>s.last_usable || last<s.first_usable || first>last)return Error{
            Errc::corrupt, 0,"GPT partition lies outside usable LBA range"
        }
        ;
        ranges.emplace_back(first, last);
    }
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i<ranges.size(); ++i)if (ranges[i].first <= ranges[i-1].second)return Error{
        Errc::corrupt, 0,"GPT partitions overlap"
    }
    ;
    return {};
}

bool sides_equal(const RawSide& a, const RawSide& b) {
    return a.revision == b.revision && a.header_size == b.header_size &&
        a.backup_lba == b.current_lba && b.backup_lba == a.current_lba &&
        a.first_usable == b.first_usable && a.last_usable == b.last_usable &&
        a.entry_count == b.entry_count && a.entry_size == b.entry_size &&
        a.disk_guid == b.disk_guid && a.entries == b.entries;
}

Result<void> validate_side_geometry(const RawSide&s, std::uint64_t total, std::uint32_t ss) {
    if (s.backup_lba >= total || s.backup_lba == s.current_lba)return Error{
        Errc::corrupt, 0,"GPT alternate-header LBA is invalid"
    }
    ;
    if (s.first_usable>s.last_usable || s.last_usable >= total)return Error{
        Errc::corrupt, 0,"GPT usable LBA range is invalid"
    }
    ;
    const auto bytes = static_cast<std::uint64_t>(s.entry_count)*s.entry_size;
    const auto sectors = (bytes+ss-1U)/ss;
    const auto arr_first = s.entries_lba, arr_last = s.entries_lba+sectors-1U;
    if (!(arr_last<s.first_usable || arr_first>s.last_usable))return Error{
        Errc::corrupt, 0,"GPT entry array overlaps usable partition space"
    }
    ;
    if (s.current_lba >= s.first_usable && s.current_lba <= s.last_usable)return Error{
        Errc::corrupt, 0,"GPT header overlaps usable partition space"
    }
    ;
    return {};
}


Result<RawSide> side_from_backup_bytes(std::span<const std::byte> header,
                                       std::span<const std::byte> entries,
                                       std::uint64_t lba,
                                       std::uint64_t total,
                                       std::uint32_t ss) {
    if (header.size()<ss)return Error{Errc::corrupt, 0,"GPT backup header sector is truncated"};
    RawSide out;
    out.lba = lba;
    out.header.assign(header.begin(), header.begin()+static_cast<std::ptrdiff_t>(ss));
    out.signature_ok = true;
    for (std::size_t i = 0; i<SIG.size(); ++i)if (std::to_integer<char>(out.header[i]) != SIG[i]) {
        out.signature_ok = false;
        break;
    }
    if (!out.signature_ok)return Error{Errc::corrupt, 0,"GPT backup contains a header with invalid signature"};
    out.revision = read_le<std::uint32_t>(out.header, 8);
    out.header_size = read_le<std::uint32_t>(out.header, 12);
    if (out.header_size<92 || out.header_size>ss)return Error{
        Errc::corrupt, 0,"GPT backup contains invalid header size"
    }
    ;
    const auto stored = read_le<std::uint32_t>(out.header, 16);
    auto hc = out.header;
    write_le32(hc, 16, 0);
    out.header_crc_ok = ((crc32_ieee(std::span<const std::byte>(hc).first(out.header_size))^0xffffffffU) == stored);
    if (!out.header_crc_ok)return Error{
        Errc::corrupt, 0,"GPT backup embedded header checksum mismatch"
    }
    ;
    out.current_lba = read_le<std::uint64_t>(out.header, 24);
    out.backup_lba = read_le<std::uint64_t>(out.header, 32);
    out.first_usable = read_le<std::uint64_t>(out.header, 40);
    out.last_usable = read_le<std::uint64_t>(out.header, 48);
    std::copy_n(out.header.begin()+56, 16, out.disk_guid.begin());
    out.entries_lba = read_le<std::uint64_t>(out.header, 72);
    out.entry_count = read_le<std::uint32_t>(out.header, 80);
    out.entry_size = read_le<std::uint32_t>(out.header, 84);
    const auto stored_entries = read_le<std::uint32_t>(out.header, 88);
    if (out.current_lba != lba)
        return Error{Errc::corrupt, 0,"GPT backup embedded header current-LBA mismatch"};
    if (out.entry_size<128 || out.entry_size>4096 || out.entry_count == 0 || out.entry_count>16384)
        return Error{Errc::corrupt, 0,"GPT backup embedded entry geometry is unreasonable"};
    const auto bytes = static_cast<std::uint64_t>(out.entry_count)*out.entry_size;
    if (bytes != entries.size())
        return Error{Errc::corrupt, 0,"GPT backup embedded entry array size disagrees with header"};
    out.entries.assign(entries.begin(), entries.end());
    out.entries_crc_ok = ((crc32_ieee(out.entries)^0xffffffffU) == stored_entries);
    if (!out.entries_crc_ok)
        return Error{Errc::corrupt, 0,"GPT backup embedded entry-array checksum mismatch"};
    auto vg = validate_side_geometry(out, total, ss);
    if (!vg)return vg.error();
    auto ve = validate_entries(out);
    if (!ve)return ve.error();
    return out;
}

std::vector<std::byte> make_header(const RawSide& canonical, std::uint64_t current,
                                   std::uint64_t backup, std::uint64_t entries_lba, std::uint32_t entries_crc,
                                   std::uint32_t ss) {
    std::vector<std::byte> h(ss);
    for (std::size_t i = 0; i<SIG.size(); ++i)h[i] = std::byte(static_cast<unsigned char>(SIG[i]));
    write_le32(h, 8, canonical.revision?canonical.revision:0x00010000U);
    write_le32(h, 12, canonical.header_size?canonical.header_size:92U);
    write_le32(h, 16, 0);
    write_le32(h, 20, 0);
    write_le64(h, 24, current);
    write_le64(h, 32, backup);
    write_le64(h, 40, canonical.first_usable);
    write_le64(h, 48, canonical.last_usable);
    std::copy(canonical.disk_guid.begin(), canonical.disk_guid.end(), h.begin()+56);
    write_le64(h, 72, entries_lba);
    write_le32(h, 80, canonical.entry_count);
    write_le32(h, 84, canonical.entry_size);
    write_le32(h, 88, entries_crc);
    const auto crc = crc32_ieee(std::span<const std::byte>(h).first(canonical.header_size))^0xffffffffU;
    write_le32(h, 16, crc);
    return h;
}

Result<MutationReport> write_pair(BlockDevice& dev, const RawSide& canonical,
                                  const std::vector<std::byte>& entries, const MutationOptions& options) {
    auto safe = require_disk_offline_for_write(dev);
    if (!safe)return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds)return ds.error();
    if (ds.value().block_device && !options.allow_block_device)return Error{
        Errc::unsafe, 0,"GPT mutation on a block device requires --allow-block-device"
    }
    ;
    const auto ss = dev.geometry().logical_sector?dev.geometry().logical_sector:512U;
    const auto total = dev.geometry().size_bytes/ss;
    if (total<100)return Error{
        Errc::invalid_argument, 0,"device is too small for GPT"
    }
    ;
    const auto last = total-1;
    const auto bytes = static_cast<std::uint64_t>(canonical.entry_count)*canonical.entry_size;
    const auto sectors = (bytes+ss-1)/ss;
    const auto backup_entries = last-sectors;
    std::uint64_t primary_entries = 2;
    if (canonical.first_usable>sectors
        && canonical.first_usable-sectors >= 2)primary_entries = canonical.first_usable-sectors;
    const auto ecrc = crc32_ieee(entries)^0xffffffffU;
    auto bh = make_header(canonical, last, 1, backup_entries, ecrc, ss);
    auto ph = make_header(canonical, 1, last, primary_entries, ecrc, ss);
    MutationReport r;
    auto wr = dev.write_exact(backup_entries*ss, entries);
    if (!wr)return wr.error();
    wr = dev.write_exact(last*ss, bh);
    if (!wr)return wr.error();
    auto fl = dev.flush();
    if (!fl)return fl.error();
    r.backup_written = true;
    wr = dev.write_exact(primary_entries*ss, entries);
    if (!wr)return wr.error();
    wr = dev.write_exact(ss, ph);
    if (!wr)return wr.error();
    fl = dev.flush();
    if (!fl)return fl.error();
    r.primary_written = true;
    auto verify = read_redundant_table(dev);
    if (!verify)return verify.error();
    if (!verify.value().primary_header_crc_ok || !verify.value().entries_crc_ok
        || !verify.value().backup_header_crc_ok || !verify.value().backup_entries_crc_ok)return Error{
        Errc::io, 0,"GPT post-write verification failed"
    }
    ;
    r.verified = true;
#if defined(__linux__)
    if (options.reread_kernel_table && dev.geometry().is_block_device) {
        r.kernel_reread_requested = (::ioctl(dev.native_fd(), BLKRRPART) == 0);
    }
#else
    (void)options;
#endif
    return r;
}
}

Result<bool> probe(BlockDevice& dev) {
    const auto ss = dev.geometry().logical_sector?dev.geometry().logical_sector:512U;
    std::vector<std::byte>b(ss);
    auto r = dev.read_exact(ss, b);
    if (!r)return r.error();
    for (std::size_t i = 0; i<8; ++i)if (std::to_integer<char>(b[i]) != SIG[i])return false;
    return true;
}

Result<Table> read_redundant_table(BlockDevice& dev) {
    const auto ss = dev.geometry().logical_sector?dev.geometry().logical_sector:512U;
    const auto total = dev.geometry().size_bytes/ss;
    if (total<2)return Error{
        Errc::corrupt, 0,"device too small for GPT"
    }
    ;
    auto p = read_side(dev, 1);
    if (!p)return p.error();
    auto b = read_side(dev, total-1);
    if (!b)return b.error();
    const bool pv = p.value().signature_ok && p.value().header_crc_ok && p.value().entries_crc_ok;
    const bool bv = b.value().signature_ok && b.value().header_crc_ok && b.value().entries_crc_ok;
    if (!pv && !bv)return Error{
        Errc::corrupt, 0,"both GPT copies are invalid"
    }
    ;
    if (pv) {
        auto v = validate_entries(p.value());
        if (!v)return v.error();
    }
    if (bv) {
        auto v = validate_entries(b.value());
        if (!v)return v.error();
    }
    if (pv && bv && !sides_equal(p.value(), b.value()))return Error{
        Errc::corrupt, 0,"primary and backup GPT copies are both valid but disagree (split-brain GPT)"
    }
    ;
    const auto&c = pv?p.value():b.value();
    auto t = table_from_side(c, protective_mbr_ok(dev));
    t.primary_header_crc_ok = p.value().header_crc_ok;
    t.entries_crc_ok = p.value().entries_crc_ok;
    t.backup_header_crc_ok = b.value().header_crc_ok;
    t.backup_entries_crc_ok = b.value().entries_crc_ok;
    t.primary_entries_lba = p.value().entries_lba;
    t.backup_entries_lba = b.value().entries_lba;
    return t;
}
Result<Table> read_table(BlockDevice& dev) {
    return read_redundant_table(dev);
}

Result<MutationReport> resize_partition(BlockDevice& dev, std::uint32_t index,
                                        std::uint64_t new_first, std::uint64_t new_last,
                                        const MutationOptions& options, Progress* progress) {
    const auto ss = dev.geometry().logical_sector?dev.geometry().logical_sector:512U;
    const auto total = dev.geometry().size_bytes/ss;
    auto p = read_side(dev, 1);
    if (!p)return p.error();
    auto b = read_side(dev, total-1);
    if (!b)return b.error();
    const bool pv = p.value().signature_ok && p.value().header_crc_ok && p.value().entries_crc_ok;
    const bool bv = b.value().signature_ok && b.value().header_crc_ok && b.value().entries_crc_ok;
    if (pv && bv && !sides_equal(p.value(), b.value()))return Error{
        Errc::corrupt, 0,"refusing GPT mutation because valid primary and backup copies disagree"
    }
    ;
    RawSide c;
    if (pv)c = p.value();
    else if (bv)c = b.value();
    else return Error{
        Errc::corrupt, 0,"no valid GPT copy is available for mutation"
    }
    ;
    if (index == 0 || index>c.entry_count)return Error{
        Errc::invalid_argument, 0,"partition index outside GPT array"
    }
    ;
    if (new_first<c.first_usable || new_last>c.last_usable || new_first>new_last)return Error{
        Errc::invalid_argument, 0,"requested partition range is outside usable GPT space"
    }
    ;
    auto ent = std::span<std::byte>(c.entries).subspan(static_cast<std::size_t>(index-1)*c.entry_size, c.entry_size);
    std::array<std::byte, 16> type{};
    std::copy_n(ent.begin(), 16, type.begin());
    if (zero_guid(type))return Error{
        Errc::not_found, 0,"requested GPT partition entry is unused"
    }
    ;
    write_le64(ent, 32, new_first);
    write_le64(ent, 40, new_last);
    auto v = validate_entries(c);
    if (!v)return v.error();
    if (progress)progress->update({
                                  "gpt-write","backup table", 0, 2, 0, 0, 0, 2
                                  }
                                 );
    auto r = write_pair(dev, c, c.entries, options);
    if (progress && r)progress->update({
                                       "gpt-write","primary table", 2, 2, 0, 0, 2, 2
                                       }
                                      );
    return r;
}

Result<MutationReport> repair_redundancy(BlockDevice& dev, const MutationOptions& options) {
    const auto ss = dev.geometry().logical_sector?dev.geometry().logical_sector:512U;
    const auto total = dev.geometry().size_bytes/ss;
    auto p = read_side(dev, 1);
    if (!p)return p.error();
    auto b = read_side(dev, total-1);
    if (!b)return b.error();
    const bool pv = p.value().signature_ok && p.value().header_crc_ok && p.value().entries_crc_ok;
    const bool bv = b.value().signature_ok && b.value().header_crc_ok && b.value().entries_crc_ok;
    if (!pv && !bv)return Error{
        Errc::corrupt, 0,"neither GPT copy can be used as repair source"
    }
    ;
    if (pv && bv && !sides_equal(p.value(), b.value()))return Error{
        Errc::corrupt, 0,"both GPT copies are valid but disagree; repair source is ambiguous"
    }
    ;
    const auto&c = pv?p.value():b.value();
    auto v = validate_entries(c);
    if (!v)return v.error();
    return write_pair(dev, c, c.entries, options);
}


Result<void> backup_metadata(BlockDevice& dev, const std::string& backup_path) {
    auto table = read_redundant_table(dev);
    if (!table)return table.error();
    const auto&t = table.value();
    if (!t.primary_header_crc_ok || !t.entries_crc_ok || !t.backup_header_crc_ok ||
        !t.backup_entries_crc_ok)
        return Error{Errc::corrupt, 0, "GPT metadata backup requires two valid GPT copies"};
    const auto ss = static_cast<std::uint64_t>(
                                               dev.geometry().logical_sector ? dev.geometry().logical_sector : 512U);
    if (ss<512 || ss>65536)return Error{Errc::unsupported, 0,"unsupported logical sector size for GPT backup"};
    const auto entry_bytes = static_cast<std::uint64_t>(t.entry_count)*t.entry_size;
    if (entry_bytes>64ULL*1024ULL*1024ULL)return Error{Errc::corrupt, 0,"GPT entry array too large to back up"};
    if (dev.geometry().size_bytes/ss<2)return Error{Errc::corrupt, 0,"disk too small for GPT backup"};
    const auto last = dev.geometry().size_bytes/ss-1;
    const auto payload_size = 3*ss+2*entry_bytes;
    if (payload_size>std::numeric_limits<std::size_t>::max())return Error{
        Errc::unsupported, 0,"GPT backup payload exceeds address space"
    }
    ;
    std::vector<std::byte> payload(static_cast<std::size_t>(payload_size));
    std::uint64_t off = 0;
    auto read_piece = [&](std::uint64_t disk_off, std::uint64_t bytes)->Result<void>{
        if (bytes>payload_size-off)return Error{Errc::internal, 0,"GPT backup payload layout overflow"};
        auto span = std::span<std::byte>(payload).subspan(static_cast<std::size_t>(off),
                                                          static_cast<std::size_t>(bytes));
        auto r = dev.read_exact(disk_off, span);
        if (!r)return r.error();
        off+=bytes;
        return {};
    };
    auto r = read_piece(0, ss);
    if (!r)return r.error();
    r = read_piece(ss, ss);
    if (!r)return r.error();
    r = read_piece(t.primary_entries_lba*ss, entry_bytes);
    if (!r)return r.error();
    r = read_piece(t.backup_entries_lba*ss, entry_bytes);
    if (!r)return r.error();
    r = read_piece(last*ss, ss);
    if (!r)return r.error();
    if (off != payload_size)return Error{Errc::internal, 0,"GPT backup payload size mismatch"};

    constexpr std::size_t header_bytes = 128;
    std::array<std::byte, header_bytes> h{};
    constexpr char magic[8] = {'F','S','X','G','P','T','B','1'};
    for (std::size_t i = 0; i<8; ++i)h[i] = std::byte(static_cast<unsigned char>(magic[i]));
    write_le32(h, 8, 1);
    write_le32(h, 12, static_cast<std::uint32_t>(ss));
    write_le64(h, 16, dev.geometry().size_bytes);
    write_le64(h, 24, entry_bytes);
    write_le64(h, 32, t.primary_entries_lba);
    write_le64(h, 40, t.backup_entries_lba);
    write_le64(h, 48, last);
    write_le64(h, 56, payload_size);
    const auto pcrc = crc32c(payload)^0xffffffffU;
    write_le32(h, 64, pcrc);
    write_le32(h, 68, 0);
    const auto hcrc = crc32c(h)^0xffffffffU;
    write_le32(h, 68, hcrc);
    const auto temp = backup_path+".part."+format_uuid(random_uuid_v4());
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
    auto out = BlockDevice::create_file(temp, header_bytes+payload_size);
    if (!out)return out.error();
    auto wr = out.value().write_exact(0, h);
    if (!wr)return wr.error();
    wr = out.value().write_exact(header_bytes, payload);
    if (!wr)return wr.error();
    auto fl = out.value().flush();
    if (!fl)return fl.error();
    auto pub = publish_file_noreplace(temp, backup_path,"GPT metadata backup");
    if (!pub)return pub.error();
    cleanup.keep = true;
    return {};
}

Result<MutationReport> restore_metadata(BlockDevice& dev,
                                        const std::string& backup_path, const MutationOptions& options) {
    auto safe = require_disk_offline_for_write(dev);
    if (!safe)return safe.error();
    auto ds = inspect_device_safety(dev);
    if (!ds)return ds.error();
    if (ds.value().block_device && !options.allow_block_device)return Error{
        Errc::unsafe, 0,"GPT restore on a block device requires --allow-block-device"
    }
    ;
    auto in = BlockDevice::open_read(backup_path);
    if (!in)return in.error();
    constexpr std::size_t header_bytes = 128;
    std::array<std::byte, header_bytes> h{};
    auto rr = in.value().read_exact(0, h);
    if (!rr)return rr.error();
    constexpr char magic[8] = {
        'F','S','X','G','P','T','B','1'
    }
    ;
    for (std::size_t i = 0; i<8; ++i)if (std::to_integer<char>(h[i]) != magic[i])return Error{
        Errc::corrupt, 0,"not an FSX GPT backup"
    }
    ;
    if (read_le<std::uint32_t>(h, 8) != 1)return Error{Errc::unsupported, 0,"unsupported FSX GPT backup version"};
    const auto stored_hcrc = read_le<std::uint32_t>(h, 68);
    auto hc = h;
    write_le32(hc, 68, 0);
    if ((crc32c(hc)^0xffffffffU) != stored_hcrc)return Error{
        Errc::corrupt, 0,"GPT backup header checksum mismatch"
    }
    ;
    const auto ss = static_cast<std::uint64_t>(read_le<std::uint32_t>(h, 12));
    const auto disk_size = read_le<std::uint64_t>(h, 16);
    const auto entry_bytes = read_le<std::uint64_t>(h, 24);
    const auto primary_entries = read_le<std::uint64_t>(h, 32);
    const auto backup_entries = read_le<std::uint64_t>(h, 40);
    const auto backup_header = read_le<std::uint64_t>(h, 48);
    const auto payload_size = read_le<std::uint64_t>(h, 56);
    const auto stored_pcrc = read_le<std::uint32_t>(h, 64);
    const auto actual_ss = static_cast<std::uint64_t>(dev.geometry().logical_sector?dev.geometry().logical_sector:512U);
    if (ss != actual_ss || disk_size != dev.geometry().size_bytes)return Error{
        Errc::unsafe, 0,"GPT backup geometry does not match target disk"
    }
    ;
    if (entry_bytes == 0 || entry_bytes>64ULL*1024ULL*1024ULL)return Error{
        Errc::corrupt, 0,"GPT backup entry geometry is unreasonable"
    }
    ;
    if (payload_size != 3*ss+2*entry_bytes)return Error{Errc::corrupt, 0,"GPT backup payload geometry mismatch"};
    if (payload_size>std::numeric_limits<std::size_t>::max())return Error{
        Errc::corrupt, 0,"GPT backup payload exceeds host address space"
    }
    ;
    if (header_bytes+payload_size != in.value().geometry().size_bytes)return Error{
        Errc::corrupt, 0,"GPT backup file size does not match its header"
    }
    ;
    const auto total_sectors = disk_size/ss;
    if (total_sectors<2 || backup_header != total_sectors-1)return Error{
        Errc::corrupt, 0,"GPT backup header LBA does not match disk geometry"
    }
    ;
    const auto entry_sectors = (entry_bytes+ss-1)/ss;
    if (primary_entries >= total_sectors || backup_entries >= total_sectors
        || entry_sectors>total_sectors-primary_entries || entry_sectors>total_sectors-backup_entries)return Error{
        Errc::corrupt, 0,"GPT backup entry arrays are outside disk geometry"
    }
    ;
    std::vector<std::byte> payload(static_cast<std::size_t>(payload_size));
    rr = in.value().read_exact(header_bytes, payload);
    if (!rr)return rr.error();
    if ((crc32c(payload)^0xffffffffU) != stored_pcrc)return Error{
        Errc::corrupt, 0,"GPT backup payload checksum mismatch"
    }
    ;
    std::uint64_t off = 0;
    auto piece = [&](std::uint64_t n)->std::span<const std::byte>{
        auto r = std::span<const std::byte>(payload).subspan(static_cast<std::size_t>(off),
                                                             static_cast<std::size_t>(n));
        off+=n;
        return r;
    }
    ;
    const auto mbr = piece(ss);
    const auto ph = piece(ss);
    const auto pe = piece(entry_bytes);
    const auto be = piece(entry_bytes);
    const auto bh = piece(ss);
    if (std::to_integer<unsigned>(mbr[510]) != 0x55U || std::to_integer<unsigned>(mbr[511]) != 0xaaU)return Error{
        Errc::corrupt, 0,"GPT backup protective MBR signature is invalid"
    }
    ;
    bool protective = false;
    for (unsigned i = 0; i<4; ++i)if (std::to_integer<unsigned>(mbr[446U+i*16U+4U]) == 0xeeU)protective = true;
    if (!protective)return Error{
        Errc::corrupt, 0,"GPT backup protective MBR has no 0xEE entry"
    }
    ;
    auto ps = side_from_backup_bytes(ph, pe, 1, total_sectors, static_cast<std::uint32_t>(ss));
    if (!ps)return ps.error();
    auto bs = side_from_backup_bytes(bh, be, backup_header, total_sectors, static_cast<std::uint32_t>(ss));
    if (!bs)return bs.error();
    if (ps.value().entries_lba != primary_entries || bs.value().entries_lba != backup_entries)return Error{
        Errc::corrupt, 0,"GPT backup sidecar entry-array LBAs disagree with embedded headers"
    }
    ;
    if (!sides_equal(ps.value(), bs.value()))return Error{
        Errc::corrupt, 0,"GPT backup contains internally inconsistent primary and backup tables"
    }
    ;
    auto wr = dev.write_exact(backup_entries*ss, be);
    if (!wr)return wr.error();
    wr = dev.write_exact(backup_header*ss, bh);
    if (!wr)return wr.error();
    auto fl = dev.flush();
    if (!fl)return fl.error();
    wr = dev.write_exact(primary_entries*ss, pe);
    if (!wr)return wr.error();
    wr = dev.write_exact(ss, ph);
    if (!wr)return wr.error();
    wr = dev.write_exact(0, mbr);
    if (!wr)return wr.error();
    fl = dev.flush();
    if (!fl)return fl.error();
    MutationReport report;
    report.backup_written = true;
    report.primary_written = true;
    auto verify = read_redundant_table(dev);
    if (!verify)return verify.error();
    if (!verify.value().primary_header_crc_ok || !verify.value().entries_crc_ok || !verify.value().backup_header_crc_ok
        || !verify.value().backup_entries_crc_ok || !verify.value().protective_mbr_ok)return Error{
        Errc::io, 0,"restored GPT failed post-write verification"
    }
    ;
    report.verified = true;
#if defined(__linux__)
    if (options.reread_kernel_table
        && dev.geometry().is_block_device)report.kernel_reread_requested = (::ioctl(dev.native_fd(), BLKRRPART) == 0);
#endif
    return report;
}

std::string guid_to_string(const std::array<std::byte, 16>& g) {
    auto b = [&](std::size_t i) {
        return std::to_integer<unsigned>(g[i]);
    }
    ;
    char s[37];
    std::snprintf(s, sizeof(s),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b(3), b(2), b(1), b(0), b(5), b(4), b(7), b(6), b(8), b(9), b(10), b(11), b(12), b(13), b(14), b(15));
    return s;
}

} // namespace fsx::gpt
