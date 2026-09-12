#pragma once
#include "fsx/device.hpp"
#include "fsx/progress.hpp"
#include "fsx/relocate.hpp"
#include "fsx/result.hpp"
#include <cstdint>
#include <string>

namespace fsx::ext4 {

struct CommitOptions {
    std::string journal_path;
    bool allow_block_device{false};
    bool verify_metadata_after{true};
    bool leave_filesystem_dirty{false};
};

struct CommitReport {
    std::uint64_t moves_total{};
    std::uint64_t destinations_reserved{};
    std::uint64_t owners_switched{};
    std::uint64_t sources_released{};
    std::uint64_t moves_committed{};
    std::uint64_t bytes_relocated{};
    bool paused{false};
    bool complete{false};
    bool filesystem_clean{false};
};

// Convert verified staging copies into authoritative ext4 mappings.  This is
// intentionally a compaction primitive, not a filesystem-size change.  It
// updates allocation bitmaps and owner extent metadata transactionally while
// keeping at least one allocated copy of each extent throughout the switch.
Result<CommitReport> commit_staged_relocation(BlockDevice& dev,
                                              const OwnerRelocationPlan& plan,
                                              const CommitOptions& options,
                                              Progress* progress = nullptr);

} // namespace fsx::ext4
