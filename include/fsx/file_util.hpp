#pragma once
#include "fsx/result.hpp"
#include <string>
#include <string_view>

namespace fsx {

// Flush the directory entry containing path. This is required after durable
// create/rename/link/unlink operations when recovery semantics depend on the
// name surviving a power loss.
Result<void> fsync_parent_directory(const std::string& path, std::string_view object_name);

// Atomically publish a completed sibling temporary file without replacing an
// existing destination. On platforms with an atomic no-replace rename this is
// preferred; otherwise a POSIX hard-link publication fallback is used.
// temp_path and final_path must reside on the same filesystem.
Result<void> publish_file_noreplace(const std::string& temp_path,
                                    const std::string& final_path,
                                    std::string_view object_name);

} // namespace fsx
