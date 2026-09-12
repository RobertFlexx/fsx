#include "fsx/crc.hpp"
#include "fsx/endian.hpp"
#include "fsx/size.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <iostream>

int main() {
    auto a = fsx::parse_size("1GiB");
    assert(a && a.value() == 1073741824ULL);
    auto b = fsx::parse_size("1GB");
    assert(b && b.value() == 1000000000ULL);
    std::array<std::byte, 8> x{};
    fsx::write_le64(x, 0, 0x1122334455667788ULL);
    assert(fsx::read_le<std::uint64_t>(x, 0) == 0x1122334455667788ULL);
    const char* s = "123456789";
    auto data = std::span<const std::byte>(reinterpret_cast<const std::byte*>(s), 9);
    assert((fsx::crc32_ieee(data)^0xffffffffU) == 0xcbf43926U);
    assert((fsx::crc32c(data)^0xffffffffU) == 0xe3069283U);
    std::cout<<"ok\n";
}
