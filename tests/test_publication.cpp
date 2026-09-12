#include "fsx/device.hpp"
#include "fsx/file_util.hpp"
#include "fsx/image.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
std::string temp_name(const char* tag) {
    return std::string("/tmp/fsx-publication-") + tag + "-" +
        std::to_string(static_cast<long long>(::getpid()));
}

std::vector<unsigned char> read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_all(const std::string& path, std::initializer_list<unsigned char> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (auto b : bytes) out.put(static_cast<char>(b));
    out.flush();
    assert(out.good());
}
}

int main() {
    const auto src = temp_name("src");
    const auto tmp = temp_name("tmp");
    const auto dst = temp_name("dst");
    const auto image = temp_name("image");

    // Atomic no-replace publication must never clobber an existing path.
    write_all(tmp, {1, 2, 3, 4});
    write_all(dst, {9, 8, 7, 6});
    auto r = fsx::publish_file_noreplace(tmp, dst, "test object");
    assert(!r && r.error().code == fsx::Errc::unsafe);
    assert((read_all(dst) == std::vector<unsigned char>{9, 8, 7, 6}));
    assert((read_all(tmp) == std::vector<unsigned char>{1, 2, 3, 4}));

    // When the destination is absent, publication succeeds and the temporary
    // name disappears while the bytes remain intact under the final name.
    std::filesystem::remove(dst);
    r = fsx::publish_file_noreplace(tmp, dst, "test object");
    assert(r);
    assert(!std::filesystem::exists(tmp));
    assert((read_all(dst) == std::vector<unsigned char>{1, 2, 3, 4}));

    // Image creation must likewise preserve an existing output verbatim.
    auto d = fsx::BlockDevice::create_file(src, 2ULL * 1024ULL * 1024ULL);
    assert(d);
    std::array<std::byte, 4096> block{};
    block.fill(std::byte{0x5a});
    assert(d.value().write_exact(0, block));
    assert(d.value().flush());
    auto ro = fsx::BlockDevice::open_read(src);
    assert(ro);
    write_all(image, {0xde, 0xad, 0xbe, 0xef});
    auto made = fsx::image::create(ro.value(), image);
    assert(!made && made.error().code == fsx::Errc::unsafe);
    assert((read_all(image) == std::vector<unsigned char>{0xde, 0xad, 0xbe, 0xef}));

    // A stale predictable-looking temporary name must not prevent creation;
    // mkstemp chooses a unique sibling instead.
    std::filesystem::remove(image);
    const auto stale = image + ".part.XXXXXX";
    write_all(stale, {0xaa});
    made = fsx::image::create(ro.value(), image);
    assert(made && made.value().complete);
    auto verified = fsx::image::verify(image);
    assert(verified && verified.value().complete);

    for (const auto& p : {src, dst, image, stale}) std::filesystem::remove(p);
    return 0;
}
