#include "fsx/iso9660.hpp"
#include "fsx/endian.hpp"
#include <algorithm>
#include <array>

namespace fsx::iso9660 {
namespace {
constexpr std::uint64_t SECTOR = 2048;
bool id_ok(std::span<const std::byte>b) {
    if (b.size()<7)return false;
    constexpr char id[] = "CD001";
    for (std::size_t i = 0; i < 5; ++i) {
        if (std::to_integer<unsigned char>(b[1 + i]) != static_cast<unsigned char>(id[i])) return false;
    }
    return true;
}
std::string trim_ascii(std::span<const std::byte>b) {
    std::string s;
    for (auto x:b) {
        auto c = std::to_integer<unsigned char>(x);
        if (c == 0)break;
        s.push_back((c >= 0x20 && c <= 0x7e)?static_cast<char>(c):'?');
    }
    while (!s.empty() && s.back() == ' ')s.pop_back();
    return s;
}
std::uint32_t be32(std::span<const std::byte>b, std::size_t o) {
    return read_be<std::uint32_t>(b, o);
}
std::uint16_t be16(std::span<const std::byte>b, std::size_t o) {
    return read_be<std::uint16_t>(b, o);
}
void err(CheckReport&r, std::string s) {
    r.clean = false;
    r.errors.push_back(std::move(s));
}
}
Result<bool> probe(BlockDevice&dev) {
    if (dev.geometry().size_bytes<17*SECTOR)return false;
    std::array<std::byte, SECTOR>b{};
    auto r = dev.read_exact(16*SECTOR, b);
    if (!r)return r.error();
    return id_ok(b);
}
Result<VolumeDescriptor> read_primary(BlockDevice&dev) {
    if (dev.geometry().size_bytes<17*SECTOR)return Error{
        Errc::unsupported, 0,"device is too small for ISO9660"
    }
    ;
    VolumeDescriptor out;
    bool have_primary = false;
    constexpr std::uint32_t max_desc = 128;
    for (std::uint32_t i = 0; i<max_desc; ++i) {
        const auto off = (16ULL+i)*SECTOR;
        if (off+SECTOR>dev.geometry().size_bytes)break;
        std::array<std::byte, SECTOR>b{};
        auto rr = dev.read_exact(off, b);
        if (!rr)return rr.error();
        if (!id_ok(b))return Error{
            Errc::corrupt, 0,"ISO9660 volume descriptor has invalid standard identifier"
        }
        ;
        if (std::to_integer<std::uint8_t>(b[6]) != 1)return Error{
            Errc::corrupt, 0,"ISO9660 volume descriptor version is not 1"
        }
        ;
        ++out.descriptor_count;
        const auto type = std::to_integer<std::uint8_t>(b[0]);
        if (type == 1 && !have_primary) {
            have_primary = true;
            out.volume_id = trim_ascii(std::span<const std::byte>(b).subspan(40, 32));
            const auto v_le = read_le<std::uint32_t>(b, 80), v_be = be32(b, 84);
            if (v_le != v_be)return Error{
                Errc::corrupt, 0,"ISO9660 volume-space-size endian copies disagree"
            }
            ;
            out.volume_blocks = v_le;
            const auto bs_le = read_le<std::uint16_t>(b, 128), bs_be = be16(b, 130);
            if (bs_le != bs_be)return Error{
                Errc::corrupt, 0,"ISO9660 logical-block-size endian copies disagree"
            }
            ;
            out.logical_block_size = bs_le;
            const auto root = std::span<const std::byte>(b).subspan(156);
            const auto root_len = std::to_integer<std::uint8_t>(root[0]);
            if (root_len<34)return Error{
                Errc::corrupt, 0,"ISO9660 root directory record is too short"
            }
            ;
            const auto e_le = read_le<std::uint32_t>(root, 2), e_be = be32(root, 6);
            const auto s_le = read_le<std::uint32_t>(root, 10), s_be = be32(root, 14);
            if (e_le != e_be || s_le != s_be)return Error{
                Errc::corrupt, 0,"ISO9660 root directory endian copies disagree"
            }
            ;
            out.root_extent = e_le;
            out.root_size = s_le;
        }
        if (type == 255) {
            out.terminator_seen = true;
            break;
        }
    }
    if (!have_primary)
        return Error{Errc::corrupt, 0,"ISO9660 primary volume descriptor was not found"};
    return out;
}
Result<CheckReport> check(BlockDevice&dev) {
    auto vr = read_primary(dev);
    if (!vr)return vr.error();
    const auto&v = vr.value();
    CheckReport r;
    if (!v.terminator_seen)err(r,"ISO9660 volume descriptor terminator was not found");
    if (v.logical_block_size<512 || v.logical_block_size>4096
        || (v.logical_block_size&(v.logical_block_size-1U)) != 0)err(r,"ISO9660 logical block size is invalid");
    if (v.volume_blocks == 0) {
        err(r, "ISO9660 volume block count is zero");
    }
    else if (v.logical_block_size
             && static_cast<std::uint64_t>(v.volume_blocks)
                 > dev.geometry().size_bytes / v.logical_block_size) {
        err(r, "ISO9660 volume geometry exceeds containing device");
    }
    if (v.root_extent >= v.volume_blocks)err(r,"ISO9660 root directory extent lies outside volume");
    if (v.root_size == 0)r.warnings.push_back("ISO9660 root directory has zero length");
    if (v.descriptor_count >= 128
        && !v.terminator_seen)r.warnings.push_back("ISO9660 descriptor scan hit the safety limit");
    r.clean = r.errors.empty();
    return r;
}
} // namespace fsx::iso9660
