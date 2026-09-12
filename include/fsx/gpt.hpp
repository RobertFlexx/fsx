#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/result.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace fsx::gpt {

struct Partition {
    std::uint32_t index{};
    std::uint64_t first_lba{};
    std::uint64_t last_lba{};
    std::uint64_t attributes{};
    std::array<std::byte, 16> type_guid{};
    std::array<std::byte, 16> unique_guid{};
    std::string name;
};

struct Table {
    bool primary_header_crc_ok{false};
    bool entries_crc_ok{false};
    bool backup_header_crc_ok{false};
    bool backup_entries_crc_ok{false};
    bool protective_mbr_ok{false};
    std::uint64_t current_lba{};
    std::uint64_t backup_lba{};
    std::uint64_t first_usable_lba{};
    std::uint64_t last_usable_lba{};
    std::uint64_t primary_entries_lba{};
    std::uint64_t backup_entries_lba{};
    std::uint32_t header_size{};
    std::uint32_t entry_size{};
    std::uint32_t entry_count{};
    std::array<std::byte, 16> disk_guid{};
    std::vector<Partition> partitions;
};

struct MutationOptions {
    bool allow_block_device{false};
    bool reread_kernel_table{true};
};

struct MutationReport {
    bool backup_written{false};
    bool primary_written{false};
    bool verified{false};
    bool kernel_reread_requested{false};
};

Result<bool> probe(BlockDevice& dev);
Result<Table> read_table(BlockDevice& dev);
Result<Table> read_redundant_table(BlockDevice& dev);
Result<MutationReport> resize_partition(BlockDevice& dev,
                                        std::uint32_t index,
                                        std::uint64_t new_first_lba,
                                        std::uint64_t new_last_lba,
                                        const MutationOptions& options = {},
                                        Progress* progress = nullptr);
Result<MutationReport> repair_redundancy(BlockDevice& dev,
                                         const MutationOptions& options = {});
Result<void> backup_metadata(BlockDevice& dev, const std::string& backup_path);
Result<MutationReport> restore_metadata(BlockDevice& dev,
                                        const std::string& backup_path,
                                        const MutationOptions& options = {});
std::string guid_to_string(const std::array<std::byte, 16>& guid);

} // namespace fsx::gpt
