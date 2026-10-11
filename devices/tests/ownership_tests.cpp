#include "rawdisk/ownership.hpp"
#include "test_util.hpp"

int main() {
    const auto directory = test_dir() / "leases";
    auto first = rawdisk::claim_carrier(directory, "usb-parent-1");
    CHECK(first);
    CHECK(err(rawdisk::claim_carrier(directory, "usb-parent-1")) == rawdisk::Errc::device_busy);
    auto other = rawdisk::claim_carrier(directory, "usb-parent-2");
    CHECK(other);
    if (first)
        first->reset();
    auto reopened = rawdisk::claim_carrier(directory, "usb-parent-1");
    CHECK(reopened);
    CHECK(err(rawdisk::claim_carrier(directory, "")) == std::errc::invalid_argument);
    if (other)
        other->reset();
    if (reopened)
        reopened->reset();
    return finish_tests();
}
