#include "fsx/btrfs.hpp"
#include "fsx/crc.hpp"
#include "fsx/device.hpp"
#include "fsx/endian.hpp"
#include "fsx/gpt.hpp"
#include "fsx/hash.hpp"
#include "fsx/iso9660.hpp"
#include "fsx/io_scheduler.hpp"
#include "fsx/probe.hpp"
#include "fsx/ufs.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <future>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
std::string hex(std::span<const std::byte> b) {
    std::ostringstream o;
    o << std::hex << std::setfill('0');
    for (auto x : b) o << std::setw(2) << static_cast<unsigned>(std::to_integer<std::uint8_t>(x));
    return o.str();
}

std::array<std::byte, 16> guid(std::uint8_t seed) {
    std::array<std::byte, 16> g{};
    for (std::size_t i = 0; i<g.size(); ++i) g[i] = std::byte(static_cast<std::uint8_t>(seed+i));
    return g;
}

std::vector<std::byte> gpt_header(std::uint64_t cur, std::uint64_t alt, std::uint64_t first, std::uint64_t last,
                                  std::uint64_t entries_lba,
                                  std::uint32_t ecrc, const std::array<std::byte, 16>& dg) {
    std::vector<std::byte> h(512);
    const char sig[] = "EFI PART";
    for (std::size_t i = 0; i<8; ++i)h[i] = std::byte(static_cast<unsigned char>(sig[i]));
    fsx::write_le32(h, 8, 0x00010000U);
    fsx::write_le32(h, 12, 92);
    fsx::write_le64(h, 24, cur);
    fsx::write_le64(h, 32, alt);
    fsx::write_le64(h, 40, first);
    fsx::write_le64(h, 48, last);
    std::copy(dg.begin(), dg.end(), h.begin()+56);
    fsx::write_le64(h, 72, entries_lba);
    fsx::write_le32(h, 80, 128);
    fsx::write_le32(h, 84, 128);
    fsx::write_le32(h, 88, ecrc);
    fsx::write_le32(h, 16, 0);
    fsx::write_le32(h, 16, fsx::crc32_ieee(std::span<const std::byte>(h).first(92))^0xffffffffU);
    return h;
}

void make_split_brain_gpt(const std::string& path) {
    constexpr std::uint64_t bytes = 64ULL*1024ULL*1024ULL, sectors = bytes/512ULL,
              last = sectors-1, entry_sectors = 32, first_usable = 34, last_usable = last-entry_sectors-1;
    auto d = fsx::BlockDevice::create_file(path, bytes);
    assert(d);
    std::array<std::byte, 512> m{};
    m[446+4] = std::byte{
        0xee
    }
    ;
    fsx::write_le32(m, 446+8, 1);
    fsx::write_le32(m, 446+12, static_cast<std::uint32_t>(sectors-1));
    m[510] = std::byte{
        0x55
    }
    ;
    m[511] = std::byte{
        0xaa
    }
    ;
    assert(d.value().write_exact(0, m));
    std::vector<std::byte> pe(128U*128U), be(128U*128U);
    const auto type = guid(0x10), uniq = guid(0x30), dg = guid(0x70);
    std::copy(type.begin(), type.end(), pe.begin());
    std::copy(uniq.begin(), uniq.end(), pe.begin()+16);
    fsx::write_le64(pe, 32, 2048);
    fsx::write_le64(pe, 40, 8191);
    be = pe;
    fsx::write_le64(be, 40, 8192);
    // independently valid but different
    const auto pcrc = fsx::crc32_ieee(pe)^0xffffffffU, bcrc = fsx::crc32_ieee(be)^0xffffffffU;
    const auto belba = last-entry_sectors;
    auto ph = gpt_header(1, last, first_usable, last_usable, 2, pcrc,
                         dg), bh = gpt_header(last, 1, first_usable, last_usable, belba, bcrc, dg);
    assert(d.value().write_exact(512, ph));
    assert(d.value().write_exact(2*512ULL, pe));
    assert(d.value().write_exact(belba*512ULL, be));
    assert(d.value().write_exact(last*512ULL, bh));
    assert(d.value().flush());
}

void put_both16(std::span<std::byte>b, std::size_t off, std::uint16_t v) {
    fsx::write_le16(b, off, v);
    b[off+2] = std::byte((v>>8U)&0xffU);
    b[off+3] = std::byte(v&0xffU);
}
void put_both32(std::span<std::byte>b, std::size_t off, std::uint32_t v) {
    fsx::write_le32(b, off, v);
    b[off+4] = std::byte((v>>24U)&0xffU);
    b[off+5] = std::byte((v>>16U)&0xffU);
    b[off+6] = std::byte((v>>8U)&0xffU);
    b[off+7] = std::byte(v&0xffU);
}

void put_ufs32(std::span<std::byte>b, std::size_t o, std::uint32_t v, bool big) {
    if (big) {
        b[o] = std::byte((v>>24U)&0xffU);
        b[o+1] = std::byte((v>>16U)&0xffU);
        b[o+2] = std::byte((v>>8U)&0xffU);
        b[o+3] = std::byte(v&0xffU);
    }
    else fsx::write_le32(b, o, v);
}
void make_ufs(const std::string& path, bool ufs2, bool big) {
    constexpr std::uint64_t bytes = 32ULL*1024ULL*1024ULL;
    auto d = fsx::BlockDevice::create_file(path, bytes);
    assert(d);
    const std::uint64_t off = ufs2?65536ULL:8192ULL;
    std::array<std::byte, 8192>b{};
    put_ufs32(b, 8, 16, big);
    put_ufs32(b, 12, 24, big);
    put_ufs32(b, 16, 32, big);
    put_ufs32(b, 20, 64, big);
    put_ufs32(b, 36, ufs2?0U:static_cast<std::uint32_t>(bytes/1024ULL), big);
    put_ufs32(b, 44, 8, big);
    put_ufs32(b, 48, 8192, big);
    put_ufs32(b, 52, 1024, big);
    put_ufs32(b, 56, 8, big);
    put_ufs32(b, 104, 2048, big);
    put_ufs32(b, 120, 32, big);
    put_ufs32(b, 184, 2048, big);
    put_ufs32(b, 188, 4096, big);
    b[209] = std::byte{1};
    put_ufs32(b, 1372, ufs2?0x19540119U:0x00011954U, big);
    assert(d.value().write_exact(off, b));
    assert(d.value().flush());
}

void make_iso(const std::string& path) {
    constexpr std::uint64_t sectors = 64;
    auto d = fsx::BlockDevice::create_file(path, sectors*2048ULL);
    assert(d);
    std::array<std::byte, 2048> p{};
    p[0] = std::byte{
        1
    }
    ;
    const char id[] = "CD001";
    for (int i = 0; i<5; ++i)p[1+i] = std::byte(id[i]);
    p[6] = std::byte{
        1
    }
    ;
    const char vid[] = "FSX ISO";
    for (std::size_t i = 0; i<sizeof(vid)-1; ++i)p[40+i] = std::byte(vid[i]);
    put_both32(p, 80, static_cast<std::uint32_t>(sectors));
    put_both16(p, 128, 2048);
    p[156] = std::byte{
        34
    }
    ;
    p[157] = std::byte{
        0
    }
    ;
    put_both32(p, 158, 20);
    put_both32(p, 166, 2048);
    p[181] = std::byte{
        2
    }
    ;
    p[188] = std::byte{
        1
    }
    ;
    p[189] = std::byte{
        0
    }
    ;
    std::array<std::byte, 2048> t{};
    t[0] = std::byte{
        255
    }
    ;
    for (int i = 0; i<5; ++i)t[1+i] = std::byte(id[i]);
    t[6] = std::byte{
        1
    }
    ;
    assert(d.value().write_exact(16ULL*2048ULL, p));
    assert(d.value().write_exact(17ULL*2048ULL, t));
    assert(d.value().flush());
}

std::array<std::byte, 32> btrfs_sum(std::span<const std::byte> sb, std::uint16_t type) {
    std::array<std::byte, 32> out{};
    auto body = sb.subspan(32);
    if (type == 0) {
        auto v = fsx::crc32c(body)^0xffffffffU;
        for (unsigned i = 0; i<4; ++i)out[i] = std::byte((v>>(8U*i))&0xffU);
    }
    else if (type == 1) {
        auto v = fsx::xxhash64(body);
        for (unsigned i = 0; i<8; ++i)out[i] = std::byte((v>>(8U*i))&0xffU);
    }
    else if (type == 2)out = fsx::sha256(body);
    else out = fsx::blake2b_256(body);
    return out;
}
void write_btrfs_sb(fsx::BlockDevice& d, std::uint64_t off,
                    std::uint16_t type, std::uint64_t total, std::uint64_t generation) {
    std::array<std::byte, 4096>b{};
    for (unsigned i = 0; i<16; ++i)b[32+i] = std::byte(i+1);
    fsx::write_le64(b, 48, off);
    fsx::write_le64(b, 64, 0x4D5F53665248425FULL);
    fsx::write_le64(b, 72, generation);
    fsx::write_le64(b, 80, 1ULL<<20);
    fsx::write_le64(b, 88, 2ULL<<20);
    fsx::write_le64(b, 112, total);
    fsx::write_le64(b, 120, 4ULL<<20);
    fsx::write_le64(b, 136, 1);
    fsx::write_le32(b, 144, 4096);
    fsx::write_le32(b, 148, 16384);
    fsx::write_le32(b, 156, 4096);
    fsx::write_le32(b, 160, 0);
    fsx::write_le16(b, 196, type);
    b[198] = std::byte{
        1
    }
    ;
    b[199] = std::byte{
        1
    }
    ;
    fsx::write_le64(b, 201, 1);
    fsx::write_le64(b, 209, total);
    fsx::write_le64(b, 217, 4ULL<<20);
    const char label[] = "FSX-BTRFS";
    for (std::size_t i = 0; i<sizeof(label)-1; ++i)b[299+i] = std::byte(label[i]);
    auto sum = btrfs_sum(b, type);
    std::copy(sum.begin(), sum.end(), b.begin());
    assert(d.write_exact(off, b));
}
void make_btrfs(const std::string&path, std::uint16_t type) {
    constexpr std::uint64_t total = 70ULL*1024ULL*1024ULL;
    auto d = fsx::BlockDevice::create_file(path, total);
    assert(d);
    write_btrfs_sb(d.value(), 64ULL*1024ULL, type, total, 7);
    write_btrfs_sb(d.value(), 64ULL*1024ULL*1024ULL, type, total, 7);
    assert(d.value().flush());
}
}

int main() {
    // Independent known vectors for the new standalone hash implementations.
    const std::string digits = "123456789";
    auto bytes = std::as_bytes(std::span(digits.data(), digits.size()));
    assert(fsx::xxhash64({}) == 0xef46db3751d8e999ULL);
    assert(hex(fsx::sha256(bytes)) == "15e2b0d3c33891ebb0f1ef609ec419420c20e320ce94c65fbc8c3312448eb225");
    assert(hex(fsx::blake2b_256({})) == "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");

    const auto pid = std::to_string(static_cast<long long>(::getpid()));
    for (std::uint16_t type = 0; type<4; ++type) {
        const auto p = "/tmp/fsx-btrfs-"+pid+"-"+std::to_string(type);
        make_btrfs(p, type);
        auto d = fsx::BlockDevice::open_read(p);
        assert(d);
        auto pr = fsx::probe_filesystem(d.value());
        assert(pr && pr.value().kind == fsx::FilesystemKind::btrfs && pr.value().confident);
        auto c = fsx::btrfs::check(d.value());
        assert(c && c.value().clean && c.value().mirrors_valid == 2);
        std::filesystem::remove(p);
    }

    const auto iso = "/tmp/fsx-iso-"+pid;
    make_iso(iso);
    {
        auto d = fsx::BlockDevice::open_read(iso);
        assert(d);
        auto p = fsx::probe_filesystem(d.value());
        assert(p && p.value().kind == fsx::FilesystemKind::iso9660 && p.value().confident);
        auto c = fsx::iso9660::check(d.value());
        assert(c && c.value().clean);
    }
    std::filesystem::remove(iso);

    const auto u2 = "/tmp/fsx-ufs2-"+pid;
    make_ufs(u2, true, false);
    {
        auto d = fsx::BlockDevice::open_read(u2);
        assert(d);
        auto p = fsx::probe_filesystem(d.value());
        assert(p && p.value().kind == fsx::FilesystemKind::ufs2 && p.value().confident);
        auto c = fsx::ufs::check(d.value());
        assert(c && c.value().clean);
    }
    std::filesystem::remove(u2);
    const auto u1 = "/tmp/fsx-ufs1be-"+pid;
    make_ufs(u1, false, true);
    {
        auto d = fsx::BlockDevice::open_read(u1);
        assert(d);
        auto p = fsx::probe_filesystem(d.value());
        assert(p && p.value().kind == fsx::FilesystemKind::ufs1 && p.value().confident);
        auto c = fsx::ufs::check(d.value());
        assert(c && c.value().clean);
    }
    std::filesystem::remove(u1);

    const auto gpt = "/tmp/fsx-split-gpt-"+pid;
    make_split_brain_gpt(gpt);
    {
        auto d = fsx::BlockDevice::open_read(gpt);
        assert(d);
        auto t = fsx::gpt::read_redundant_table(d.value());
        assert(!t && t.error().code == fsx::Errc::corrupt);
    }
    std::filesystem::remove(gpt);

    // A submission racing with/after scheduler shutdown must complete with a
    // deterministic error instead of returning a future that can never become
    // ready.  This is important for cancellation/error paths in long storage
    // operations where shutdown can wake producers blocked on queue capacity.
    const auto sched_file = "/tmp/fsx-scheduler-stop-"+pid;
    auto sd = fsx::BlockDevice::create_file(sched_file, 4096);
    assert(sd);
    fsx::AsyncIoScheduler scheduler(fsx::IoTuning{1, 1, 4096});
    scheduler.stop();
    auto rf = scheduler.submit_read(sd.value(), 0, 512);
    assert(rf.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto rr = rf.get();
    assert(!rr && rr.error().code == fsx::Errc::unsafe);
    std::vector<std::byte> wb(512, std::byte{0x5a});
    auto wf = scheduler.submit_write(sd.value(), 0, std::move(wb));
    assert(wf.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    auto wr = wf.get();
    assert(!wr && wr.error().code == fsx::Errc::unsafe);
    std::filesystem::remove(sched_file);
    return 0;
}
