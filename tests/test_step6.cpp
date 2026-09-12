#include "fsx/clone.hpp"
#include "fsx/crc.hpp"
#include "fsx/device.hpp"
#include "fsx/endian.hpp"
#include "fsx/fault.hpp"
#include "fsx/image.hpp"
#include "fsx/journal.hpp"
#include "fsx/journal_validate.hpp"
#include "fsx/ntfs.hpp"
#include "fsx/probe.hpp"
#include "fsx/scrub.hpp"
#include "fsx/xfs.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
std::string temp_name(const char* tag) {
    return std::string("/tmp/fsx-step6-") + tag + "-" + std::to_string(static_cast<long long>(::getpid()));
}
void put_be16(std::span<std::byte> b, std::size_t o, std::uint16_t v) {
    b[o] = std::byte((v>>8)&0xff);
    b[o+1] = std::byte(v&0xff);
}
void put_be32(std::span<std::byte> b, std::size_t o, std::uint32_t v) {
    for (int i = 0; i<4; ++i)b[o+i] = std::byte((v>>(24-8*i))&0xff);
}
void put_be64(std::span<std::byte> b, std::size_t o, std::uint64_t v) {
    for (int i = 0; i<8; ++i)b[o+i] = std::byte((v>>(56-8*i))&0xff);
}

void make_xfs(const std::string& path) {
    constexpr std::uint64_t bytes = 16ULL*1024ULL*1024ULL;
    auto d = fsx::BlockDevice::create_file(path, bytes);
    assert(d);
    std::array<std::byte, 512>b{};
    b[0] = std::byte{
        'X'
    }
    ;
    b[1] = std::byte{
        'F'
    }
    ;
    b[2] = std::byte{
        'S'
    }
    ;
    b[3] = std::byte{
        'B'
    }
    ;
    put_be32(b, 4, 4096);
    put_be64(b, 8, 4096);
    for (std::size_t i = 0; i<16; ++i)b[32+i] = std::byte(i+1);
    put_be32(b, 84, 1024);
    put_be32(b, 88, 4);
    put_be32(b, 96, 64);
    put_be16(b, 100, 5);
    put_be16(b, 102, 512);
    put_be16(b, 104, 512);
    put_be16(b, 106, 8);
    const char label[] = "FSX-XFS";
    for (std::size_t i = 0; i<sizeof(label)-1; ++i)b[108+i] = std::byte(label[i]);
    fsx::write_le32(b, 224, 0);
    fsx::write_le32(b, 224, ~fsx::crc32c(b));
    assert(d.value().write_exact(0, b));
    for (std::uint64_t ag = 1; ag<4; ++ag) assert(d.value().write_exact(ag*1024ULL*4096ULL, b));
    assert(d.value().flush());
}
void make_ntfs(const std::string& path) {
    constexpr std::uint64_t bytes = 16ULL*1024ULL*1024ULL;
    auto d = fsx::BlockDevice::create_file(path, bytes);
    assert(d);
    std::array<std::byte, 512>b{};
    b[0] = std::byte{
        0xeb
    }
    ;
    b[1] = std::byte{
        0x52
    }
    ;
    b[2] = std::byte{
        0x90
    }
    ;
    const char sig[] = "NTFS    ";
    for (int i = 0; i<8; ++i)b[3+i] = std::byte(sig[i]);
    fsx::write_le16(b, 11, 512);
    b[13] = std::byte{
        8
    }
    ;
    fsx::write_le64(b, 40, bytes/512);
    fsx::write_le64(b, 48, 4);
    fsx::write_le64(b, 56, 8);
    b[64] = std::byte{
        0xf6
    }
    ;
    fsx::write_le64(b, 72, 0x123456789abcdef0ULL);
    b[510] = std::byte{
        0x55
    }
    ;
    b[511] = std::byte{
        0xaa
    }
    ;
    assert(d.value().write_exact(0, b));
    assert(d.value().write_exact(bytes-512, b));
    std::array<std::byte, 1024> rec{};
    rec[0] = std::byte{
        'F'
    }
    ;
    rec[1] = std::byte{
        'I'
    }
    ;
    rec[2] = std::byte{
        'L'
    }
    ;
    rec[3] = std::byte{
        'E'
    }
    ;
    fsx::write_le16(rec, 4, 48);
    fsx::write_le16(rec, 6, 3);
    fsx::write_le16(rec, 20, 56);
    fsx::write_le32(rec, 24, 64);
    fsx::write_le32(rec, 28, 1024);
    fsx::write_le16(rec, 48, 0xa55a);
    fsx::write_le16(rec, 50, 0x1111);
    fsx::write_le16(rec, 52, 0x2222);
    fsx::write_le16(rec, 510, 0xa55a);
    fsx::write_le16(rec, 1022, 0xa55a);
    assert(d.value().write_exact(4ULL*4096ULL, rec));
    assert(d.value().write_exact(8ULL*4096ULL, rec));
    assert(d.value().flush());
}
}

int main() {
    const auto io = temp_name("io");
    {
        auto d = fsx::BlockDevice::create_file(io, 4096);
        assert(d);
        std::array<std::byte, 16>x{};
        x.fill(std::byte{
               0x5a
               }
              );
        fsx::fault::install({
                            fsx::fault::Operation::write, fsx::fault::Timing::before, 1, true
                            }
                           );
        auto r = d.value().write_exact(0, x);
        assert(!r);
        fsx::fault::reset();
        std::array<std::byte, 16>z{};
        assert(d.value().read_exact(0, z));
        for (auto b:z)assert(b == std::byte{0});
        fsx::fault::install({
                            fsx::fault::Operation::write, fsx::fault::Timing::after, 1, true
                            }
                           );
        r = d.value().write_exact(0, x);
        assert(!r);
        fsx::fault::reset();
        assert(d.value().read_exact(0, z));
        assert(z == x);
    }

    const auto jpath = temp_name("journal");
    {
        fsx::JournalIdentity id;
        id.operation_uuid = fsx::TransactionJournal::random_uuid();
        id.filesystem_uuid = fsx::TransactionJournal::random_uuid();
        id.device_size = 1024*1024;
        id.target_size = 512*1024;
        fsx::fault::install({
                            fsx::fault::Operation::write, fsx::fault::Timing::before, 1, true
                            }
                           );
        auto badcreate = fsx::TransactionJournal::create(jpath, id, 1<<20);
        assert(!badcreate);
        fsx::fault::reset();
        assert(!std::filesystem::exists(jpath));
        auto j = fsx::TransactionJournal::create(jpath, id, 1<<20);
        assert(j);
        fsx::JournalRecord dirty;
        dirty.type = fsx::JournalRecordType::filesystem_marked_dirty;
        assert(j.value().append(dirty));
        auto rec = [&](fsx::JournalRecordType t) {
            fsx::JournalRecord r;
            r.type = t;
            r.transaction_id = 1;
            r.source_block = 100;
            r.destination_block = 20;
            r.block_count = 4;
            r.inode = 12;
            return r;
        }
        ;
        assert(j.value().append(rec(fsx::JournalRecordType::relocation_intent)));
        assert(j.value().append(rec(fsx::JournalRecordType::copy_verified)));
        assert(j.value().append(rec(fsx::JournalRecordType::destination_reserved)));
        assert(j.value().append(rec(fsx::JournalRecordType::owner_switched)));
        assert(j.value().append(rec(fsx::JournalRecordType::source_released)));
        assert(j.value().append(rec(fsx::JournalRecordType::metadata_committed)));
        fsx::JournalRecord clean;
        clean.type = fsx::JournalRecordType::filesystem_marked_clean;
        assert(j.value().append(clean));
        assert(j.value().checkpoint(fsx::OperationPhase::complete));
        auto vr = fsx::validate_journal_semantics(j.value());
        assert(vr && vr.value().clean && vr.value().metadata_committed == 1);
    }
    const auto badj = temp_name("badjournal");
    {
        fsx::JournalIdentity id;
        id.operation_uuid = fsx::TransactionJournal::random_uuid();
        id.filesystem_uuid = fsx::TransactionJournal::random_uuid();
        id.device_size = 4096;
        id.target_size = 2048;
        auto j = fsx::TransactionJournal::create(badj, id, 1<<20);
        assert(j);
        fsx::JournalRecord a;
        a.type = fsx::JournalRecordType::relocation_intent;
        a.transaction_id = 2;
        a.source_block = 30;
        a.destination_block = 4;
        a.block_count = 2;
        a.inode = 7;
        assert(j.value().append(a));
        a.type = fsx::JournalRecordType::copy_verified;
        assert(j.value().append(a));
        a.type = fsx::JournalRecordType::source_released;
        assert(j.value().append(a));
        auto v = fsx::validate_journal_semantics(j.value());
        assert(v && !v.value().clean && !v.value().errors.empty());
    }

    const auto src = temp_name("src"), dst = temp_name("dst");
    {
        auto d = fsx::BlockDevice::create_file(src, 4ULL*1024ULL*1024ULL);
        assert(d);
        std::vector<std::byte>b(1<<20);
        for (std::size_t i = 0; i<b.size(); ++i)b[i] = std::byte((i*29U+11U)&0xffU);
        assert(d.value().write_exact(0, b));
        assert(d.value().write_exact(3ULL<<20, b));
        assert(d.value().flush());
    }
    {
        auto d = fsx::BlockDevice::open_read(src);
        assert(d);
        fsx::ScrubOptions o;
        o.passes = 2;
        o.chunk_bytes = 256*1024;
        auto s = fsx::scrub_read(d.value(), o);
        assert(s && s.value().stable && s.value().passes_completed == 2
               && s.value().total_bytes_read == 8ULL*1024ULL*1024ULL);
    }
    {
        auto s = fsx::BlockDevice::open_read(src);
        auto t = fsx::BlockDevice::create_file(dst, 4ULL*1024ULL*1024ULL);
        assert(s && t);
        fsx::CloneOptions o;
        o.chunk_bytes = 256*1024;
        auto c = fsx::clone_device(s.value(), t.value(), o);
        assert(c && c.value().verified && c.value().bytes_verified == 4ULL*1024ULL*1024ULL);
    }
    {
        auto s = fsx::BlockDevice::open_write(src);
        assert(s);
        auto c = fsx::clone_device(s.value(), s.value());
        assert(!c);
    }
    {
        const auto alias = src+".hardlink";
        std::filesystem::create_hard_link(src, alias);
        auto s = fsx::BlockDevice::open_read(src);
        auto t = fsx::BlockDevice::open_write(alias);
        assert(s && t);
        auto c = fsx::clone_device(s.value(), t.value());
        assert(!c && c.error().code == fsx::Errc::unsafe);
        std::filesystem::remove(alias);
    }

    const auto img = src+".fsximg";
    {
        auto s = fsx::BlockDevice::open_read(src);
        assert(s);
        fsx::fault::install({
                            fsx::fault::Operation::read, fsx::fault::Timing::before, 1, true
                            }
                           );
        auto r = fsx::image::create(s.value(), img);
        assert(!r);
        fsx::fault::reset();
        assert(!std::filesystem::exists(img));
    }

    const auto xp = temp_name("xfs");
    make_xfs(xp);
    {
        auto d = fsx::BlockDevice::open_read(xp);
        assert(d);
        auto p = fsx::probe_filesystem(d.value());
        assert(p && p.value().kind == fsx::FilesystemKind::xfs && p.value().confident);
        auto c = fsx::xfs::check(d.value());
        assert(c && c.value().clean);
    }
    {
        auto d = fsx::BlockDevice::open_write(xp);
        assert(d);
        std::array<std::byte, 1>b{
            std::byte{
                0x7f
            }
        }
        ;
        assert(d.value().write_exact(300, b));
        assert(d.value().flush());
        auto c = fsx::xfs::check(d.value());
        assert(c && !c.value().clean);
    }
    const auto np = temp_name("ntfs");
    make_ntfs(np);
    {
        auto d = fsx::BlockDevice::open_read(np);
        assert(d);
        auto p = fsx::probe_filesystem(d.value());
        assert(p && p.value().kind == fsx::FilesystemKind::ntfs && p.value().confident);
        auto c = fsx::ntfs::check(d.value());
        assert(c && c.value().clean);
    }
    {
        auto d = fsx::BlockDevice::open_write(np);
        assert(d);
        std::array<std::byte, 2>b{
            std::byte{
                0
            }
            , std::byte{
                0
            }
        }
        ;
        assert(d.value().write_exact(4ULL*4096ULL+510ULL, b));
        assert(d.value().flush());
        auto c = fsx::ntfs::check(d.value());
        assert(c && !c.value().clean);
    }

    for (const auto&p:{io, jpath, badj, src, dst, img, xp, np})std::filesystem::remove(p);
    for (const auto& e:std::filesystem::directory_iterator("/tmp")) {
        const auto n = e.path().filename().string();
        if (n.rfind(std::filesystem::path(jpath).filename().string()+".part.",
                    0) == 0)std::filesystem::remove(e.path());
    }
    return 0;
}
