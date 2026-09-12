#pragma once
#include "fsx/device.hpp"
#include "fsx/result.hpp"
#include <string>
#include <vector>

namespace fsx {

struct DeviceSafety {
    bool mounted{false};
    bool active_swap{false};
    bool block_device{false};
    bool regular_file{false};
    bool has_holders{false};
    bool descendant_mounted{false};
    bool descendant_swap{false};
    std::string mountpoint;
    std::vector<std::string> holders;
    std::vector<std::string> active_descendants;
};

Result<DeviceSafety> inspect_device_safety(const BlockDevice& dev);
Result<void> require_offline_for_write(const BlockDevice& dev);
Result<void> require_disk_offline_for_write(const BlockDevice& dev);
// Refuse clone/copy operations when two handles can refer to overlapping
// storage.  Exact regular-file aliases are detected portably.  On Linux,
// block-device ancestry and shared device-mapper/MD slave relationships are
// also resolved through sysfs so parent/partition and common-backing-device
// copies fail closed before the first write.
Result<void> require_nonoverlapping_devices(const BlockDevice& source,
                                            const BlockDevice& target);

} // namespace fsx
