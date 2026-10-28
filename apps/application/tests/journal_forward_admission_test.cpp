#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/secure_random.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>

namespace {
class CountingTarget final : public axk::app::MutationTarget {
  public:
    std::array<std::byte, 512U> bytes{};
    std::size_t writes{};
    std::function<void()> on_verify;
    std::function<void()> on_write;
    std::uint64_t size() const noexcept override { return bytes.size(); }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(), destination.begin());
        return {};
    }
    axk::app::Result<void> write_exact_at(std::uint64_t offset, std::span<const std::byte> source) override {
        ++writes;
        std::ranges::copy(source, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        if (on_write)
            on_write();
        return {};
    }
    axk::app::Result<void> flush() override { return {}; }
    axk::app::Result<void> verify_bound() const override { return {}; }
    axk::app::Result<void> verify_unchanged() const override {
        if (on_verify)
            on_verify();
        return {};
    }
    std::string revision() const override { return "revision"; }
    std::string stable_identity() const override { return "identity"; }
    axk::app::MutationIdentity journal_identity() const override {
        return {axk::app::MutationSourceKind::device, "scope", "disk"};
    }
};

TEST(JournalForwardAdmission, CancellationDuringLastVerificationDoesNotWriteRollbackBytes) {
    const auto directory =
        std::filesystem::temp_directory_path() / ("axk-forward-cancel-" + *axk::app::secure_random_hex(8U));
    auto target = std::make_shared<CountingTarget>();
    axk::CancellationSource cancellation;
    const std::array patches{axk::app::AlterationJournalPatch{0U, std::vector<std::byte>(512U),
                                                              std::vector<std::byte>(512U, std::byte{0x11})}};
    axk::app::AlterationJournalStore store{directory};
    const auto result = store.apply(target, target->size(), patches, cancellation.token(), {}, {},
                                    [&](const auto &) -> axk::app::Result<void> {
                                        target->on_verify = [&] { cancellation.cancel(); };
                                        return {};
                                    });
    EXPECT_FALSE(result);
    EXPECT_EQ(target->writes, 0U);
    EXPECT_TRUE(store.storage_ready());
    std::filesystem::remove_all(directory);
}

TEST(JournalForwardAdmission, FinalGateRunsOnceAndNeverRevokesRollbackAfterAWriteAttempt) {
    const auto directory =
        std::filesystem::temp_directory_path() / ("axk-forward-gate-" + *axk::app::secure_random_hex(8U));
    auto target = std::make_shared<CountingTarget>();
    axk::CancellationSource cancellation;
    target->on_write = [&] { cancellation.cancel(); };
    const std::array patches{axk::app::AlterationJournalPatch{0U, std::vector<std::byte>(512U),
                                                              std::vector<std::byte>(512U, std::byte{0x11})}};
    axk::app::AlterationJournalStore store{directory};
    std::size_t gates{};
    const auto result =
        store.apply(target, target->size(), patches, cancellation.token(), {}, {}, {}, [&]() -> axk::app::Result<void> {
            ++gates;
            if (gates > 1U)
                return std::unexpected(axk::app::Error{"expired", "Consent expired"});
            return {};
        });
    EXPECT_FALSE(result);
    EXPECT_EQ(gates, 1U);
    EXPECT_EQ(target->writes, 2U);
    EXPECT_TRUE(std::ranges::all_of(target->bytes, [](auto byte) { return byte == std::byte{}; }));
    std::filesystem::remove_all(directory);
}

TEST(JournalForwardAdmission, RefusedFinalGateNeverWritesOrRestores) {
    const auto directory =
        std::filesystem::temp_directory_path() / ("axk-forward-refusal-" + *axk::app::secure_random_hex(8U));
    auto target = std::make_shared<CountingTarget>();
    const std::array patches{axk::app::AlterationJournalPatch{0U, std::vector<std::byte>(512U),
                                                              std::vector<std::byte>(512U, std::byte{0x11})}};
    axk::app::AlterationJournalStore store{directory};
    const auto result = store.apply(target, target->size(), patches, {}, {}, {}, {}, []() -> axk::app::Result<void> {
        return std::unexpected(axk::app::Error{"expired", "Consent expired"});
    });
    EXPECT_FALSE(result);
    EXPECT_EQ(target->writes, 0U);
    EXPECT_TRUE(store.storage_ready());
    std::filesystem::remove_all(directory);
}
} // namespace
