#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace fsx::iso9660 {

struct VolumeDescriptor {
    std::string volume_id;
    std::uint32_t volume_blocks{};
    std::uint16_t logical_block_size{};
    std::uint32_t root_extent{};
    std::uint32_t root_size{};
    std::uint32_t descriptor_count{};
    bool terminator_seen{false};
};

struct CheckReport {
    bool clean{true};
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

Result<bool> probe(BlockDevice& dev);
Result<VolumeDescriptor> read_primary(BlockDevice& dev);
Result<CheckReport> check(BlockDevice& dev);

} // namespace fsx::iso9660
