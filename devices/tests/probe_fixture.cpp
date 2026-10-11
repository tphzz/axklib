#include "rawdisk/open/locator.hpp"
#include "rawdisk/probe.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

// Leave a known modified sector for CLI recovery tests, only in an isolated regular file.
class InterruptedImage final : public rawdisk::BlockDevice {
  public:
    explicit InterruptedImage(std::unique_ptr<rawdisk::BlockDevice> image) : image_(std::move(image)) {}
    std::uint32_t sector_size() const noexcept override { return image_->sector_size(); }
    std::uint64_t sector_count() const noexcept override { return image_->sector_count(); }
    bool writable() const noexcept override { return true; }

  private:
    rawdisk::Result<void> read_raw(std::uint64_t offset, std::span<std::byte> bytes) override {
        return image_->read(offset / sector_size(), bytes);
    }
    rawdisk::Result<void> write_raw(std::uint64_t offset, std::span<const std::byte> bytes) override {
        if (++writes_ != 1)
            return std::unexpected(std::make_error_code(std::errc::io_error));
        return image_->write(offset / sector_size(), bytes);
    }
    rawdisk::Result<void> flush_raw() override { return image_->flush(); }
    std::unique_ptr<rawdisk::BlockDevice> image_;
    int writes_ = 0;
};

// Fixture-plan producer only; open_image rejects physical device files.
int main(int argc, char **argv) {
    if (argc != 3 && argc != 5)
        return 2;
    auto image = rawdisk::open_image(argv[1], argc == 3 ? rawdisk::Access::ReadOnly : rawdisk::Access::ReadWrite);
    if (!image || (*image)->sector_count() < 128)
        return 1;
    rawdisk::probe::Plan plan{.sector_size = 512,
                              .sector_count = (*image)->sector_count(),
                              .lba = 64,
                              .original = std::vector<std::byte>(512),
                              .guards = {{0, std::vector<std::byte>(512)}, {127, std::vector<std::byte>(512)}}};
    if (!(*image)->read(plan.lba, plan.original))
        return 1;
    for (auto &guard : plan.guards)
        if (!(*image)->read(guard.lba, guard.bytes))
            return 1;
    if (argc == 5) {
        const auto locator = rawdisk::open::parse_locator(argv[3]);
        if (!locator)
            return 1;
        InterruptedImage interrupted(std::move(*image));
        const auto report =
            rawdisk::probe::run(interrupted, plan, {.locator = rawdisk::open::format_locator(*locator)}, argv[4]);
        return report.pattern_verified && !report.restored && report.restoration_error ? 0 : 1;
    }
    return rawdisk::probe::save_plan(argv[2], plan) ? 0 : 1;
}
