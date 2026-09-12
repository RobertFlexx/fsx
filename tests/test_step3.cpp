#include "fsx/allocation.hpp"
#include "fsx/device.hpp"
#include "fsx/extents.hpp"
#include "fsx/journal.hpp"
#include "fsx/relocate.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unistd.h>

int main() {
    // Unit-level owner-plan and verified-staging test image is built directly here.
    const std::string img = "/tmp/fsx-step3-"+std::to_string(static_cast<long long>(::getpid()))+".img";
    const std::string jnl = img+".jnl";
    constexpr std::uint64_t bs = 4096;
    constexpr std::uint64_t blocks = 4096;
    {
        auto d = fsx::BlockDevice::create_file(img, blocks*bs);
        assert(d);
        std::vector<std::byte> sb(1024);
        auto w16 = [&](std::size_t o, std::uint16_t v) {
            sb[o] = std::byte(v&0xffU);
            sb[o+1] = std::byte((v>>8U)&0xffU);
        }
        ;
        auto w32 = [&](std::size_t o, std::uint32_t v) {
            for (int i = 0; i<4; ++i)sb[o+static_cast<std::size_t>(i)] = std::byte((v>>(8*i))&0xffU);
        }
        ;
        w32(0x00, 1024);
        w32(0x04, blocks);
        w32(0x0c, 4012);
        w32(0x10, 1023);
        w32(0x14, 0);
        w32(0x18, 2);
        w32(0x20, 32768);
        w32(0x28, 1024);
        w16(0x38, 0xef53);
        w16(0x3a, 1);
        w32(0x4c, 1);
        w32(0x54, 11);
        w16(0x58, 256);
        w32(0x60, 0x40);
        w16(0xfe, 32);
        const unsigned char uuid[16] = {
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
        }
        ;
        for (int i = 0; i<16; ++i)sb[0x68+static_cast<std::size_t>(i)] = std::byte(uuid[i]);
        assert(d.value().write_exact(1024, sb));
        std::vector<std::byte> gd(32);
        auto g32 = [&](std::size_t o, std::uint32_t v) {
            for (int i = 0; i<4; ++i)gd[o+static_cast<std::size_t>(i)] = std::byte((v>>(8*i))&0xffU);
        }
        ;
        g32(0, 2);
        g32(4, 3);
        g32(8, 4);
        auto g16 = [&](std::size_t o, std::uint16_t v) {
            gd[o] = std::byte(v&0xffU);
            gd[o+1] = std::byte((v>>8U)&0xffU);
        }
        ;
        g16(12, 4012);
        g16(14, 1023);
        g16(16, 1);
        assert(d.value().write_exact(bs, gd));
        std::vector<std::byte> bm(bs);
        for (std::uint64_t i = 0; i<68; ++i)bm[static_cast<std::size_t>(i/8)]|=std::byte(1U<<(i%8));
        for (std::uint64_t i = 3800; i<3816; ++i)bm[static_cast<std::size_t>(i/8)]|=std::byte(1U<<(i%8));
        assert(d.value().write_exact(2*bs, bm));
        std::vector<std::byte> ib(bs);
        ib[0] = std::byte(0x02);
        assert(d.value().write_exact(3*bs, ib));
        std::vector<std::byte> inode(256);
        auto i16 = [&](std::size_t o, std::uint16_t v) {
            inode[o] = std::byte(v&0xffU);
            inode[o+1] = std::byte((v>>8U)&0xffU);
        }
        ;
        auto i32 = [&](std::size_t o, std::uint32_t v) {
            for (int i = 0; i<4; ++i)inode[o+static_cast<std::size_t>(i)] = std::byte((v>>(8*i))&0xffU);
        }
        ;
        i16(0, 0x81a4);
        i16(26, 1);
        i32(32, 0x00080000);
        i32(100, 0x1234);
        i16(40, 0xf30a);
        i16(42, 1);
        i16(44, 4);
        i16(46, 0);
        i32(52, 0);
        i16(56, 16);
        i16(58, 0);
        i32(60, 3800);
        assert(d.value().write_exact(4*bs+256, inode));
        std::vector<std::byte> data(16*bs, std::byte{
                                    0x5a
                                    }
                                   );
        assert(d.value().write_exact(3800*bs, data));
        assert(d.value().flush());
    }
    {
        auto r = fsx::BlockDevice::open_read(img);
        assert(r);
        auto owners = fsx::ext4::scan_extent_owners(r.value(), 3700, nullptr);
        assert(owners);
        assert(owners.value().extents.size() == 1);
        assert(owners.value().extents[0].physical_block == 3800);
        assert(owners.value().extents[0].length == 16);
        auto plan = fsx::ext4::build_owner_relocation_plan(r.value(), 3700*bs, nullptr);
        assert(plan);
        assert(plan.value().destinations_available);
        assert(plan.value().moves.size() == 1);
        assert(plan.value().moves[0].source_block == 3800);
        assert(plan.value().moves[0].destination_block<3700);
    }
    {
        auto w = fsx::BlockDevice::open_write(img);
        assert(w);
        auto plan = fsx::ext4::build_owner_relocation_plan(w.value(), 3700*bs, nullptr);
        assert(plan);
        fsx::ext4::StageOptions o;
        o.journal_path = jnl;
        o.verify_after_write = true;
        auto st = fsx::ext4::stage_relocation(w.value(), plan.value(), o, nullptr);
        assert(st);
        assert(st.value().complete);
        assert(st.value().moves_completed == 1);
        std::vector<std::byte> a(16*bs), b(16*bs);
        assert(w.value().read_exact(3800*bs, a));
        assert(w.value().read_exact(plan.value().moves[0].destination_block*bs, b));
        assert(a == b);
        auto j = fsx::TransactionJournal::open(jnl, false);
        assert(j);
        assert(j.value().status().phase == fsx::OperationPhase::staged);
        // A second run must validate the staged destination and resume idempotently.
        auto again = fsx::ext4::stage_relocation(w.value(), plan.value(), o, nullptr);
        assert(again);
        assert(again.value().complete);
        assert(again.value().moves_completed == 1);
    }
    // One torn/corrupted redundant header must still leave the journal readable.
    {
        auto j = fsx::BlockDevice::open_write(jnl);
        assert(j);
        std::array<std::byte, 16> junk{};
        for (auto&b:junk)b = std::byte{
            0xa5
        }
        ;
        assert(j.value().write_exact(0, junk));
        assert(j.value().flush());
        auto reopened = fsx::TransactionJournal::open(jnl, false);
        assert(reopened);
        assert(reopened.value().status().header_b_valid);
        assert(reopened.value().status().phase == fsx::OperationPhase::staged);
    }
    std::filesystem::remove(img);
    std::filesystem::remove(jnl);
    return 0;
}
