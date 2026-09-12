#include "fsx/crc.hpp"
#include "fsx/device.hpp"
#include "fsx/endian.hpp"
#include "fsx/exfat.hpp"
#include "fsx/fat.hpp"
#include "fsx/image.hpp"
#include "fsx/probe.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

std::uint32_t exfat_boot_checksum(std::span<const std::byte> data, std::uint32_t sector_bytes) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(sector_bytes) * 11U; ++i) {
        if (i == 106U || i == 107U || i == 112U) continue;
        sum = ((sum << 31U) | (sum >> 1U)) + std::to_integer<std::uint8_t>(data[i]);
    }
    return sum;
}

void make_fat12(const std::string& path) {
    auto d = fsx::BlockDevice::create_file(path, 2880ULL * 512ULL);
    assert(d);
    std::array<std::byte, 512> b{};
    b[0] = std::byte{0xeb};
    b[1] = std::byte{0x3c};
    b[2] = std::byte{0x90};
    const char oem[] = "MSDOS5.0";
    for (int i = 0; i<8; ++i)b[3+i] = std::byte(oem[i]);
    fsx::write_le16(b, 11, 512);
    b[13] = std::byte{1};
    fsx::write_le16(b, 14, 1);
    b[16] = std::byte{2};
    fsx::write_le16(b, 17, 224);
    fsx::write_le16(b, 19, 2880);
    b[21] = std::byte{0xf0};
    fsx::write_le16(b, 22, 9);
    fsx::write_le16(b, 24, 18);
    fsx::write_le16(b, 26, 2);
    b[38] = std::byte{
        0x29
    }
    ;
    fsx::write_le32(b, 39, 0x12345678U);
    const char label[] = "FSXTEST    ";
    for (int i = 0; i<11; ++i)b[43+i] = std::byte(label[i]);
    b[510] = std::byte{0x55};
    b[511] = std::byte{0xaa};
    assert(d.value().write_exact(0, b));
    std::vector<std::byte> fat(9U*512U);
    fat[0] = std::byte{
        0xf0
    }
    ;
    fat[1] = std::byte{
        0xff
    }
    ;
    fat[2] = std::byte{
        0xff
    }
    ;
    assert(d.value().write_exact(512, fat));
    assert(d.value().write_exact((1+9)*512ULL, fat));
    assert(d.value().flush());
}

void make_exfat(const std::string& path) {
    constexpr std::uint32_t ss = 512;
    constexpr std::uint64_t sectors = 64;
    auto d = fsx::BlockDevice::create_file(path, sectors*ss);
    assert(d);
    std::vector<std::byte> region(12U*ss);
    region[0] = std::byte{0xeb};
    region[1] = std::byte{0x76};
    region[2] = std::byte{0x90};
    const char name[] = "EXFAT   ";
    for (int i = 0; i<8; ++i)region[3+i] = std::byte(name[i]);
    fsx::write_le64(region, 64, 0);
    fsx::write_le64(region, 72, sectors);
    fsx::write_le32(region, 80, 24);
    fsx::write_le32(region, 84, 1);
    fsx::write_le32(region, 88, 25);
    fsx::write_le32(region, 92, 39);
    fsx::write_le32(region, 96, 2);
    fsx::write_le32(region, 100, 0xabcdef01U);
    fsx::write_le16(region, 104, 0x0100);
    region[108] = std::byte{
        9
    }
    ;
    region[109] = std::byte{
        0
    }
    ;
    region[110] = std::byte{
        1
    }
    ;
    region[112] = std::byte{
        0
    }
    ;
    region[510] = std::byte{0x55};
    region[511] = std::byte{0xaa};
    const auto sum = exfat_boot_checksum(region, ss);
    for (std::size_t off = 11U*ss; off<12U*ss; off+=4)fsx::write_le32(region, off, sum);
    assert(d.value().write_exact(0, region));
    assert(d.value().write_exact(12ULL*ss, region));
    std::array<std::byte, 512> fat{};
    fsx::write_le32(fat, 2U*4U, 0xffffffffU); // root directory cluster 2
    assert(d.value().write_exact(24ULL*ss, fat));
    std::array<std::byte, 512> root{};
    root[0] = std::byte{
        0x83
    }
    ;
    root[1] = std::byte{
        3
    }
    ;
    root[2] = std::byte{
        'F'
    }
    ;
    root[4] = std::byte{
        'S'
    }
    ;
    root[6] = std::byte{
        'X'
    }
    ;
    assert(d.value().write_exact(25ULL*ss, root));
    assert(d.value().flush());
}

} // namespace

int main() {
    const auto pid = std::to_string(static_cast<long long>(::getpid()));
    const std::string fatp = "/tmp/fsx-step5-fat-"+pid+".img";
    make_fat12(fatp);
    {
        auto d = fsx::BlockDevice::open_read(fatp);
        assert(d);
        auto v = fsx::fat::read_volume(d.value());
        assert(v);
        assert(v.value().kind == fsx::fat::Kind::fat12);
        auto c = fsx::fat::check(d.value());
        assert(c && c.value().clean);
        auto p = fsx::probe_filesystem(d.value());
        assert(p && p.value().kind == fsx::FilesystemKind::fat12);
    }

    const std::string exp = "/tmp/fsx-step5-exfat-"+pid+".img";
    make_exfat(exp);
    {
        auto d = fsx::BlockDevice::open_read(exp);
        assert(d);
        auto v = fsx::exfat::read_volume(d.value());
        assert(v);
        assert(v.value().main_boot_checksum_ok && v.value().backup_boot_checksum_ok);
        assert(v.value().label == "FSX");
        auto c = fsx::exfat::check(d.value());
        assert(c && c.value().clean);
        auto p = fsx::probe_filesystem(d.value());
        assert(p && p.value().kind == fsx::FilesystemKind::exfat);
    }

    const std::string srcp = "/tmp/fsx-step5-src-"+pid+".bin", imgp = srcp+".fsximg", dstp = srcp+".dst";
    {
        auto d = fsx::BlockDevice::create_file(srcp, 8ULL*1024ULL*1024ULL);
        assert(d);
        std::vector<std::byte>x(1024*1024);
        for (std::size_t i = 0; i<x.size(); ++i)x[i] = std::byte((i*17U+3U)&0xffU);
        assert(d.value().write_exact(0, x));
        assert(d.value().write_exact(6ULL*1024ULL*1024ULL, x));
        assert(d.value().flush());
    }
    {
        auto s = fsx::BlockDevice::open_read(srcp);
        assert(s);
        fsx::image::CreateOptions o;
        o.chunk_bytes = 1024U*1024U;
        auto c = fsx::image::create(s.value(), imgp, o);
        assert(c && c.value().zero_chunks >= 4);
        auto v = fsx::image::verify(imgp);
        assert(v && v.value().complete);
        auto t = fsx::BlockDevice::create_file(dstp, 8ULL*1024ULL*1024ULL);
        assert(t);
        auto r = fsx::image::restore(imgp, t.value(), false);
        assert(r);
    }
    // Restoring an archive onto itself must fail before any payload write.
    {
        auto t = fsx::BlockDevice::open_write(imgp);
        assert(t);
        auto r = fsx::image::restore(imgp, t.value(), false);
        assert(!r && r.error().code == fsx::Errc::unsafe);
        auto v = fsx::image::verify(imgp);
        assert(v && v.value().complete);
    }
    // Payload corruption is detected before restore can trust the archive.
    {
        const std::string bad = imgp+".bad";
        std::filesystem::copy_file(imgp, bad);
        auto d = fsx::BlockDevice::open_write(bad);
        assert(d);
        std::array<std::byte, 1>x{};
        assert(d.value().read_exact(4096+48, x));
        x[0]^=std::byte{
            0x5a
        }
        ;
        assert(d.value().write_exact(4096+48, x));
        assert(d.value().flush());
        auto v = fsx::image::verify(bad);
        assert(!v);
        std::filesystem::remove(bad);
    }
    {
        auto a = fsx::BlockDevice::open_read(srcp);
        auto b = fsx::BlockDevice::open_read(dstp);
        assert(a && b);
        std::vector<std::byte>x(1<<20), y(1<<20);
        for (std::uint64_t o = 0; o<8ULL*1024ULL*1024ULL; o+=x.size()) {
            assert(a.value().read_exact(o, x));
            assert(b.value().read_exact(o, y));
            assert(x == y);
        }
    }

    // Device bounds are checked before the kernel is asked to perform I/O.
    {
        auto d = fsx::BlockDevice::open_read(srcp);
        assert(d);
        std::array<std::byte, 16>b{};
        auto r = d.value().read_exact(8ULL*1024ULL*1024ULL-8, b);
        assert(!r);
    }

    std::filesystem::remove(fatp);
    std::filesystem::remove(exp);
    std::filesystem::remove(srcp);
    std::filesystem::remove(imgp);
    std::filesystem::remove(dstp);
    return 0;
}
