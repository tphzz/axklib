#include "axklib/application/device_sessions.hpp"
#include "axklib/application/secure_random.hpp"
#include "axklib/writer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <memory>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
class PrefixTransport final : public axk::app::DeviceTransport {
  public:
    axk::HdsInitialization initialization;
    std::uint64_t physical_size{};
    std::size_t writes{};
    bool corrupt_backup{};
    bool corrupt_sector_size{};
    std::uint64_t size() const noexcept override { return physical_size; }
    std::uint32_t sector_size() const noexcept override { return 512U; }
    std::string carrier_identity() const override { return "prefix-carrier"; }
    std::string media_identity() const override { return "prefix-media"; }
    axk::app::Result<void> authorize_writes() override { return {}; }
    axk::app::Result<void> verify_bound() const override { return {}; }
    axk::app::Result<void> flush() override { return {}; }
    axk::app::Result<void> write_exact_at(std::uint64_t, std::span<const std::byte>) override {
        ++writes;
        return {};
    }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        std::ranges::fill(bytes, std::byte{0x42});
        for (const auto &write : initialization.writes) {
            const auto start = std::max(offset, write.offset);
            const auto end = std::min(offset + bytes.size(), write.offset + write.bytes.size());
            if (start < end)
                std::memcpy(bytes.data() + (start - offset), write.bytes.data() + (start - write.offset), end - start);
        }
        if (corrupt_backup && offset <= 512U && offset + bytes.size() > 512U)
            bytes[static_cast<std::size_t>(512U - offset)] = std::byte{};
        if (corrupt_sector_size) {
            for (const auto at : {0x9dU, 512U + 0x9dU})
                if (offset <= at && at < offset + bytes.size())
                    bytes[static_cast<std::size_t>(at - offset)] = std::byte{0x10};
        }
        return {};
    }
};
class PrefixProvider final : public axk::app::DeviceProvider {
  public:
    std::shared_ptr<PrefixTransport> media = std::make_shared<PrefixTransport>();
    axk::app::Result<std::vector<axk::app::DeviceCandidate>> list() override {
        return std::vector<axk::app::DeviceCandidate>{{"candidate", "Prefix disk", "serial", media->size(), 512U}};
    }
    axk::app::Result<axk::app::ConnectedDevice> connect(std::string_view, std::string_view, std::string_view) override {
        return axk::app::ConnectedDevice{media,
                                         {"candidate", "Prefix disk", "serial", media->size(), 512U},
                                         {{"disk", "Whole disk", 0U, media->size(), 512U, false, {}}}};
    }
};
} // namespace

TEST(DevicePrefix, ReopeningUsesTheDeclaredSfsExtentAtBothSidesOfTheFormerNineGbBoundary) {
    const auto initialization = axk::plan_hds_initialization(axk::maximum_hds_size, 8U);
    ASSERT_TRUE(initialization);
    const auto random = axk::app::secure_random_hex(8U);
    ASSERT_TRUE(random);
    const auto directory = std::filesystem::temp_directory_path() / ("axk-prefix-" + *random);
    for (const auto physical_size :
         std::array<std::uint64_t, 3>{axk::maximum_hds_size + 512U, 9'000'000'000ULL, 64ULL * 1024U * 1024U * 1024U}) {
        auto provider = std::make_shared<PrefixProvider>();
        provider->media->physical_size = physical_size;
        provider->media->initialization = *initialization;
        for (unsigned restart = 0; restart < 2U; ++restart) {
            axk::app::DeviceSessionManager manager{directory, true, true, provider};
            ASSERT_TRUE(manager.list("owner"));
            const auto connection = manager.connect("candidate", "owner");
            ASSERT_TRUE(connection) << connection.error().message;
            const auto source = manager.open_source(connection->session_id, "disk", "owner");
            ASSERT_TRUE(source) << source.error().message;
            EXPECT_EQ(source->reader->size(), axk::maximum_hds_size);
            EXPECT_EQ(connection->targets.front().size_bytes, physical_size);
            ASSERT_TRUE(source->seal({}, nullptr));
            ASSERT_TRUE(manager.authorize_writes(connection->session_id, "disk", "owner", true));
            auto mutation = manager.begin_mutation(connection->session_id, "disk", "owner",
                                                   std::to_string(physical_size) + '-' + std::to_string(restart));
            ASSERT_TRUE(mutation) << mutation.error().message;
            EXPECT_EQ((*mutation)->size(), axk::maximum_hds_size);
            const std::vector<std::byte> beyond(512U);
            EXPECT_FALSE((*mutation)->write_exact_at(axk::maximum_hds_size, beyond));
            EXPECT_EQ(provider->media->writes, 0U);
        }
    }
    std::filesystem::remove_all(directory);
}

TEST(DevicePrefix, ValidLegacyFullCapacitySfsRetainsItsWholeNineGbView) {
    auto provider = std::make_shared<PrefixProvider>();
    auto initialization = axk::plan_hds_initialization(axk::maximum_hds_size, 8U);
    ASSERT_TRUE(initialization);
    constexpr std::uint64_t capacity = 9'000'000'000ULL;
    auto &headers = initialization->writes.front();
    ASSERT_EQ(headers.offset, 0U);
    ASSERT_GE(headers.bytes.size(), 1024U);
    for (const auto base : {0U, 512U})
        for (unsigned index = 0U; index < 4U; ++index)
            headers.bytes[base + 0xa0U + index] = static_cast<std::byte>((capacity / 512U) >> ((3U - index) * 8U));
    provider->media->initialization = *initialization;
    provider->media->physical_size = capacity;
    const auto directory =
        std::filesystem::temp_directory_path() / ("axk-prefix-legacy-" + *axk::app::secure_random_hex(8U));
    {
        axk::app::DeviceSessionManager manager{directory, true, true, provider};
        ASSERT_TRUE(manager.list("owner"));
        const auto connection = manager.connect("candidate", "owner");
        ASSERT_TRUE(connection);
        const auto source = manager.open_source(connection->session_id, "disk", "owner");
        ASSERT_TRUE(source);
        EXPECT_EQ(source->reader->size(), capacity);
        const auto image = axk::open_image(source->reader, {});
        ASSERT_TRUE(image) << image.error().message;
        EXPECT_TRUE(image->backup_superblock_matches());
        EXPECT_TRUE(image->diagnostics().empty());
        ASSERT_TRUE(source->seal({}, nullptr));
        ASSERT_TRUE(manager.authorize_writes(connection->session_id, "disk", "owner", true));
        const auto mutation = manager.begin_mutation(connection->session_id, "disk", "owner", "legacy");
        ASSERT_TRUE(mutation);
        EXPECT_EQ((*mutation)->size(), capacity);
        EXPECT_EQ(provider->media->writes, 0U);
    }
    std::filesystem::remove_all(directory);
}

TEST(DevicePrefix, InconsistentSmallerSfsNeverFallsBackToWritableWholeTargetAccess) {
    auto provider = std::make_shared<PrefixProvider>();
    const auto initialization = axk::plan_hds_initialization(axk::maximum_hds_size, 8U);
    ASSERT_TRUE(initialization);
    provider->media->initialization = *initialization;
    provider->media->physical_size = 9'000'000'000ULL;
    provider->media->corrupt_backup = true;
    const auto random = axk::app::secure_random_hex(8U);
    ASSERT_TRUE(random);
    const auto directory = std::filesystem::temp_directory_path() / ("axk-prefix-corrupt-" + *random);
    {
        axk::app::DeviceSessionManager manager{directory, true, true, provider};
        ASSERT_TRUE(manager.list("owner"));
        const auto connection = manager.connect("candidate", "owner");
        ASSERT_TRUE(connection);
        const auto source = manager.open_source(connection->session_id, "disk", "owner");
        ASSERT_TRUE(source);
        ASSERT_TRUE(source->seal({}, nullptr));
        EXPECT_FALSE(manager.authorize_writes(connection->session_id, "disk", "owner", true));
        EXPECT_EQ(provider->media->writes, 0U);
    }
    std::filesystem::remove_all(directory);
}

TEST(DevicePrefix, DamagedSectorSizeInBothSmallerDeclarationsCannotWidenTheWritableView) {
    auto provider = std::make_shared<PrefixProvider>();
    const auto initialization = axk::plan_hds_initialization(axk::maximum_hds_size, 8U);
    ASSERT_TRUE(initialization);
    provider->media->initialization = *initialization;
    provider->media->physical_size = 9'000'000'000ULL;
    provider->media->corrupt_sector_size = true;
    const auto random = axk::app::secure_random_hex(8U);
    ASSERT_TRUE(random);
    const auto directory = std::filesystem::temp_directory_path() / ("axk-prefix-sector-" + *random);
    {
        axk::app::DeviceSessionManager manager{directory, true, true, provider};
        ASSERT_TRUE(manager.list("owner"));
        const auto connection = manager.connect("candidate", "owner");
        ASSERT_TRUE(connection);
        const auto source = manager.open_source(connection->session_id, "disk", "owner");
        ASSERT_TRUE(source);
        ASSERT_TRUE(source->seal({}, nullptr));
        EXPECT_FALSE(manager.authorize_writes(connection->session_id, "disk", "owner", true));
        EXPECT_EQ(provider->media->writes, 0U);
    }
    std::filesystem::remove_all(directory);
}

TEST(DevicePrefix, AdmissionGuardsAreDurableBeforeTheFinalForwardGate) {
    auto provider = std::make_shared<PrefixProvider>();
    const auto initialization = axk::plan_hds_initialization(axk::maximum_hds_size, 8U);
    ASSERT_TRUE(initialization);
    provider->media->initialization = *initialization;
    provider->media->physical_size = axk::maximum_hds_size + 512U;
    const auto directory =
        std::filesystem::temp_directory_path() / ("axk-prefix-admission-" + *axk::app::secure_random_hex(8U));
    {
        axk::app::DeviceSessionManager manager{directory, true, true, provider};
        ASSERT_TRUE(manager.list("owner"));
        const auto connection = manager.connect("candidate", "owner");
        ASSERT_TRUE(connection);
        const auto source = manager.open_source(connection->session_id, "disk", "owner");
        ASSERT_TRUE(source);
        ASSERT_TRUE(source->seal({}, nullptr));
        ASSERT_TRUE(manager.authorize_writes(connection->session_id, "disk", "owner", true));
        auto mutation = manager.begin_mutation(connection->session_id, "disk", "owner", "guard-publication");
        ASSERT_TRUE(mutation);
        std::vector<std::byte> original(512U);
        ASSERT_TRUE((*mutation)->read_exact_at(0U, original));
        const std::array patches{
            axk::app::AlterationJournalPatch{0U, original, std::vector<std::byte>(512U, std::byte{0x11})}};
        constexpr std::uint64_t extra = 123456ULL * 4096U;
        std::size_t gates{};
        const auto applied = manager.apply(
            *mutation, patches, {}, {}, {},
            [&](const auto &anticipated) -> axk::app::Result<void> {
                std::array<std::byte, 512U> bytes{};
                EXPECT_TRUE(anticipated->read_exact_at(extra, bytes));
                return {};
            },
            [&]() -> axk::app::Result<void> {
                ++gates;
                const auto operation = std::filesystem::directory_iterator{directory / "operations"} -> path();
                std::ifstream saved{operation / "guards.json"};
                const auto guards = nlohmann::json::parse(saved);
                EXPECT_TRUE(std::ranges::any_of(guards.at("sectors"), [](const auto &entry) {
                    return entry.at("offset").template get<std::uint64_t>() == extra;
                }));
                return std::unexpected(axk::app::Error{"operation_cancelled", "Cancelled during final admission"});
            });
        EXPECT_FALSE(applied);
        EXPECT_EQ(gates, 1U);
        EXPECT_EQ(provider->media->writes, 0U);
    }
    std::filesystem::remove_all(directory);
}
