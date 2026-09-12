#pragma once
#include "fsx/result.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace fsx {

struct DeviceGeometry {
    std::uint64_t size_bytes{};
    std::uint32_t logical_sector{512};
    std::uint32_t physical_sector{512};
    bool is_block_device{false};
    bool read_only{false};
};

class BlockDevice {
public:
    BlockDevice() = default;
    BlockDevice(const BlockDevice&) = delete;
    BlockDevice& operator = (const BlockDevice&) = delete;
    BlockDevice(BlockDevice && other) noexcept;
    BlockDevice& operator = (BlockDevice && other) noexcept;
    ~BlockDevice();

    static Result<BlockDevice> open_read(std::string path);
    static Result<BlockDevice> open_write(std::string path);
    static Result<BlockDevice> create_file(std::string path, std::uint64_t size_bytes);

    Result<void> read_exact(std::uint64_t off, std::span<std::byte> out) const;
    Result<void> write_exact(std::uint64_t off, std::span<const std::byte> in);
    Result<void> flush();

    const DeviceGeometry& geometry() const noexcept { return geometry_;
    }
    const std::string& path() const noexcept { return path_;
    }
    int native_fd() const noexcept { return fd_;
    }

private:
    friend Result<BlockDevice> open_device_impl(std::string, bool);
    int fd_{-1};
    std::string path_;
    DeviceGeometry geometry_{};
    bool writable_{false};
};

} // namespace fsx
