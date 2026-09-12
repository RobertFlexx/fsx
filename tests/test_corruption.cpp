#include "fsx/device.hpp"
#include "fsx/crc.hpp"
#include "fsx/endian.hpp"
#include "fsx/ext4.hpp"
#include "fsx/image.hpp"
#include "fsx/journal.hpp"
#include "fsx/probe.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
std::uint64_t next(std::uint64_t& s) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}
void fill(std::vector<std::byte>& b, std::uint64_t& seed) {
    for (auto& x : b) x = std::byte(static_cast<unsigned char>(next(seed) & 0xffU));
}
}

int main() {
    const auto base = std::string("/tmp/fsx-corrupt-") + std::to_string(static_cast<long long>(::getpid()));
    const auto raw = base + ".raw";
    const auto img = base + ".img";
    const auto jnl = base + ".jnl";
    auto d = fsx::BlockDevice::create_file(raw, 512ULL * 1024ULL);
    assert(d);
    auto i = fsx::BlockDevice::create_file(img, 64ULL * 1024ULL);
    assert(i);
    auto j = fsx::BlockDevice::create_file(jnl, 1ULL * 1024ULL * 1024ULL);
    assert(j);

    std::uint64_t seed = 0x8c3c010cb4754c9dULL;
    std::vector<std::byte> head(64U * 1024U);
    std::vector<std::byte> image_head(64U * 1024U);
    std::vector<std::byte> journal_head(8192U);
    for (unsigned round = 0; round < 128; ++round) {
        fill(head, seed);
        // Periodically seed recognizable signatures so parser validation is
        // exercised beyond the initial magic check.
        if ((round % 4U) == 0) {
            head[0] = std::byte{
                'X'
            }
            ;
            head[1] = std::byte{
                'F'
            }
            ;
            head[2] = std::byte{
                'S'
            }
            ;
            head[3] = std::byte{
                'B'
            }
            ;
        }
        if ((round % 4U) == 1) {
            const char s[] = "NTFS    ";
            for (int k = 0; k<8; ++k)head[3+k] = std::byte(s[k]);
            head[510] = std::byte{
                0x55
            }
            ;
            head[511] = std::byte{
                0xaa
            }
            ;
        }
        if ((round % 4U) == 2) {
            const char s[] = "EXFAT   ";
            for (int k = 0; k<8; ++k)head[3+k] = std::byte(s[k]);
        }
        auto w = d.value().write_exact(0, head);
        assert(w);
        (void)fsx::probe_filesystem(d.value());

        fill(image_head, seed);
        assert(i.value().write_exact(0, image_head));
        (void)fsx::image::inspect(img);

        fill(journal_head, seed);
        assert(j.value().write_exact(0, journal_head));
        (void)fsx::TransactionJournal::open(jnl, false);
    }


    // Valid FSX image magic/checksum with hostile geometry must be rejected
    // deterministically without arithmetic overflow or large allocation.
    {
        std::array<std::byte, 4096> h{};
        const char magic[8] = {'F','S','X','I','M','G','1','\0'};
        for (std::size_t k = 0; k<8; ++k) h[k] = std::byte(static_cast<unsigned char>(magic[k]));
        fsx::write_le32(h, 8, 1);
        fsx::write_le32(h, 12, 4096);
        fsx::write_le64(h, 16, std::numeric_limits<std::uint64_t>::max());
        fsx::write_le32(h, 24, 512);
        fsx::write_le32(h, 28, 64U*1024U);
        const auto chunks = std::numeric_limits<std::uint64_t>::max()/(64U*1024U)+1U;
        fsx::write_le64(h, 32, chunks);
        fsx::write_le32(h, 40, 1);
        fsx::write_le32(h, 44, 0);
        fsx::write_le32(h, 44, fsx::crc32c(h)^0xffffffffU);
        assert(i.value().write_exact(0, h));
        // inspect may accept the mathematically valid huge geometry, but verify
        // must fail cleanly on the truncated archive rather than overflowing.
        auto inspected = fsx::image::inspect(img);
        assert(inspected);
        auto verified = fsx::image::verify(img);
        assert(!verified);

        fsx::write_le32(h, 24, 3); // invalid non-power-of-two logical sector
        fsx::write_le32(h, 44, 0);
        fsx::write_le32(h, 44, fsx::crc32c(h)^0xffffffffU);
        assert(i.value().write_exact(0, h));
        inspected = fsx::image::inspect(img);
        assert(!inspected);
    }

    // Hostile ext superblocks must be rejected without undefined shifts,
    // wrapping byte geometry, or oversized allocations.
    {
        std::array<std::byte, 1024> sb{};
        fsx::write_le32(sb, 0x00, 8); // inodes_count
        fsx::write_le32(sb, 0x04, 128); // blocks_count_lo
        fsx::write_le32(sb, 0x14, 1); // first_data_block
        fsx::write_le32(sb, 0x18, std::numeric_limits<std::uint32_t>::max());
        fsx::write_le32(sb, 0x20, 8); // blocks_per_group
        fsx::write_le32(sb, 0x28, 8); // inodes_per_group
        fsx::write_le16(sb, 0x38, 0xEF53U);
        fsx::write_le16(sb, 0x58, 128); // inode_size
        assert(d.value().write_exact(1024, sb));
        auto ext = fsx::ext4::read_superblock(d.value());
        assert(!ext);

        // Valid block-size encoding but impossible 64-bit filesystem geometry
        // must fail against the containing device rather than wrapping bytes.
        sb.fill(std::byte{0});
        fsx::write_le32(sb, 0x00, 8);
        fsx::write_le32(sb, 0x04, 0xffffffffU);
        fsx::write_le32(sb, 0x14, 0);
        fsx::write_le32(sb, 0x18, 0); // 1 KiB blocks
        fsx::write_le32(sb, 0x20, 8);
        fsx::write_le32(sb, 0x28, 8);
        fsx::write_le16(sb, 0x38, 0xEF53U);
        fsx::write_le16(sb, 0x58, 128);
        fsx::write_le32(sb, 0x60, 0x80U);
        fsx::write_le16(sb, 0xfe, 64);
        fsx::write_le32(sb, 0x150, 0xffffffffU);
        assert(d.value().write_exact(1024, sb));
        ext = fsx::ext4::read_superblock(d.value());
        assert(!ext);
    }

    std::filesystem::remove(raw);
    std::filesystem::remove(img);
    std::filesystem::remove(jnl);
    return 0;
}
