#include "device_format_test_support.hpp"

#include "axklib/catalog.hpp"
#include "axklib/sfs.hpp"

#include <cstddef>
#include <fstream>
#include <iterator>

namespace device_format_test {
using DeviceFormat = Fixture;

TEST_F(DeviceFormat, SelectionAndReviewAreReadOnlyAndUnknownNeverMeansBlank) {
    const auto plan = review();
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_FALSE(plan->at("inspection").at("warnings").empty());
    EXPECT_TRUE(plan->at("inspection").at("blockedReasons").empty());
    EXPECT_EQ(provider->disk->writes, 0U);
    EXPECT_EQ(provider->disk->initialization_authorizations, 0U);
    EXPECT_EQ(provider->disk->ordinary_authorizations, 0U);
}

TEST_F(DeviceFormat, ConsentIsOwnerBoundExactSingleUseAndReservedBeforeExecution) {
    const auto plan = review();
    ASSERT_TRUE(plan);
    auto input = consent(*plan);
    input["confirmationText"] = "ERASE some other disk";
    EXPECT_FALSE(formats->admit(input, context));
    input = consent(*plan);
    auto foreign = context;
    foreign.owner_id = "foreign";
    EXPECT_FALSE(formats->admit(input, foreign));
    auto admitted = formats->admit(input, context);
    ASSERT_TRUE(admitted) << admitted.error().message;
    EXPECT_FALSE(formats->admit(input, context));
    EXPECT_FALSE(devices->close(session.session_id, context.owner_id));
    EXPECT_FALSE(devices->open_source(session.session_id, "disk", context.owner_id));
    const auto retained = devices->operations(context.owner_id);
    ASSERT_TRUE(retained);
    ASSERT_EQ(retained->size(), 1U);
    EXPECT_EQ(retained->front().kind, "format");
    EXPECT_EQ(retained->front().state, "no_write");
    admitted->reset();
    EXPECT_FALSE(formats->execute(input, context));
    EXPECT_EQ(provider->disk->writes, 0U);
    EXPECT_TRUE(devices->close(session.session_id, context.owner_id));
}

TEST_F(DeviceFormat, FormatsStrictEmptySfsAndNeverWritesTheUntouchedSuffix) {
    const auto plan = review();
    ASSERT_TRUE(plan);
    const auto input = consent(*plan);
    auto admitted = formats->admit(input, context);
    ASSERT_TRUE(admitted);
    const auto formatted = formats->execute(input, context);
    ASSERT_TRUE(formatted) << formatted.error().code << ": " << formatted.error().message;
    EXPECT_EQ(provider->disk->ordinary_authorizations, 0U);
    EXPECT_EQ(provider->disk->initialization_authorizations, 1U);
    EXPECT_GT(provider->disk->writes, 0U);
    const auto image = axk::open_image(provider->transport, {});
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->partitions().size(), 1U);
    EXPECT_TRUE(image->backup_superblock_matches());
    const auto catalog = axk::build_object_catalog(*image);
    ASSERT_TRUE(catalog);
    EXPECT_TRUE(catalog->objects.empty());
    std::array<std::byte, 512U> suffix{};
    ASSERT_TRUE(provider->transport->read_exact_at(provider->disk->size - 512U, suffix));
    EXPECT_TRUE(std::ranges::all_of(suffix, [](auto value) { return value == std::byte{0x42}; }));
    admitted->reset();
    const auto opened = devices->open_source(session.session_id, "disk", context.owner_id);
    ASSERT_TRUE(opened) << opened.error().message;
}

TEST_F(DeviceFormat, MountedIncompleteAndUnsupportedCarrierStatesCannotProducePlans) {
    for (const auto kind : {0, 1, 2}) {
        provider->disk->safety.mounted.clear();
        provider->disk->safety.complete = kind != 1;
        provider->disk->safety.non_dismounting_claim_supported = kind != 2;
        if (kind == 0)
            provider->disk->safety.mounted.push_back({"partition", "/mounted", "FAT32", "CARD"});
        const auto inspected = inspect();
        ASSERT_TRUE(inspected);
        EXPECT_FALSE(inspected->at("blockedReasons").empty());
        EXPECT_FALSE(formats->plan({{"sessionId", session.session_id},
                                    {"inspectionToken", inspected->at("inspectionToken")},
                                    {"partitionCount", 1}},
                                   context));
    }
    EXPECT_EQ(provider->disk->initialization_authorizations, 0U);
    EXPECT_EQ(provider->disk->writes, 0U);
}

TEST_F(DeviceFormat, ReviewRefreshesNativeSafetyBeforePresentingDestructiveConsent) {
    const auto inspected = inspect();
    ASSERT_TRUE(inspected);
    provider->disk->safety.mounted.push_back({"partition", "/late", "FAT32", "CARD"});
    const auto planned = formats->plan({{"sessionId", session.session_id},
                                        {"inspectionToken", inspected->at("inspectionToken")},
                                        {"partitionCount", 1}},
                                       context);
    EXPECT_FALSE(planned);
    EXPECT_EQ(provider->disk->initialization_authorizations, 0U);
    EXPECT_EQ(provider->disk->writes, 0U);
}

TEST_F(DeviceFormat, ExpiryAndCancellationAfterAuthorizationNeverWrite) {
    for (const auto expire : {false, true}) {
        connect();
        axk::CancellationSource cancellation;
        context.cancellation = cancellation.token();
        const auto plan = review();
        ASSERT_TRUE(plan);
        const auto input = consent(*plan);
        auto admitted = formats->admit(input, context);
        ASSERT_TRUE(admitted);
        provider->disk->on_authorize = [&] {
            if (expire)
                now += std::chrono::minutes{16};
            else
                cancellation.cancel();
        };
        const auto formatted = formats->execute(input, context);
        ASSERT_FALSE(formatted);
        EXPECT_EQ(provider->disk->writes, 0U);
        admitted->reset();
        provider->disk->on_authorize = {};
        context.mutation_id += "-next";
    }
}

TEST_F(DeviceFormat, ChangedReviewedBytesAndNewMountsRefuseAllWrites) {
    const auto plan = review();
    ASSERT_TRUE(plan);
    const auto input = consent(*plan);
    auto admitted = formats->admit(input, context);
    ASSERT_TRUE(admitted);
    const std::array changed{std::byte{0x11}};
    provider->disk->put(0U, changed);
    EXPECT_FALSE(formats->execute(input, context));
    EXPECT_EQ(provider->disk->writes, 0U);
    admitted->reset();
    connect();
    context.mutation_id += "-next";
    const auto second = review();
    ASSERT_TRUE(second);
    const auto second_input = consent(*second);
    auto second_admitted = formats->admit(second_input, context);
    ASSERT_TRUE(second_admitted);
    provider->disk->on_authorize = [&] {
        provider->disk->safety.mounted.push_back({"partition", "/late", "FAT32", {}});
    };
    EXPECT_FALSE(formats->execute(second_input, context));
    EXPECT_EQ(provider->disk->writes, 0U);
}

TEST_F(DeviceFormat, FailedWritesRestoreOriginalBytesWithoutOrdinaryAuthorization) {
    const auto plan = review();
    ASSERT_TRUE(plan);
    const auto input = consent(*plan);
    auto admitted = formats->admit(input, context);
    ASSERT_TRUE(admitted);
    provider->disk->fail_at = 1U;
    EXPECT_FALSE(formats->execute(input, context));
    std::vector<std::byte> restored(2U * 1024U * 1024U);
    ASSERT_TRUE(provider->transport->read_exact_at(0U, restored));
    EXPECT_TRUE(std::ranges::all_of(restored, [](auto value) { return value == std::byte{0x42}; }));
    const auto operations = devices->operations(context.owner_id);
    ASSERT_TRUE(operations);
    ASSERT_EQ(operations->size(), 1U);
    EXPECT_EQ(operations->front().state, "rolled_back");
    EXPECT_EQ(provider->disk->ordinary_authorizations, 0U);
}

TEST_F(DeviceFormat, FailedRestorationRequiresFreshTypedRecoveryAndUsesJournalExtent) {
    const auto plan = review();
    ASSERT_TRUE(plan);
    const auto input = consent(*plan);
    auto admitted = formats->admit(input, context);
    ASSERT_TRUE(admitted);
    provider->disk->fail_at = 1U;
    provider->disk->fail_restore = true;
    const auto failed = formats->execute(input, context);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, "device_recovery_required");
    admitted->reset();
    provider->disk->fail_at = 0U;
    provider->disk->fail_restore = false;
    connect();
    EXPECT_FALSE(devices->recover(context.mutation_id, session.session_id, "disk", context.owner_id, true));
    EXPECT_FALSE(devices->recover(context.mutation_id, session.session_id, "disk", context.owner_id, true,
                                  "RESTORE wrong", true));
    const auto recovered = devices->recover(context.mutation_id, session.session_id, "disk", context.owner_id, true,
                                            "RESTORE /dev/synthetic", true);
    ASSERT_TRUE(recovered) << recovered.error().code << ": " << recovered.error().message;
    std::vector<std::byte> restored(2U * 1024U * 1024U);
    ASSERT_TRUE(provider->transport->read_exact_at(0U, restored));
    EXPECT_TRUE(std::ranges::all_of(restored, [](auto value) { return value == std::byte{0x42}; }));
    EXPECT_EQ(provider->disk->ordinary_authorizations, 0U);
}

TEST_F(DeviceFormat, QueuedOwnershipFencesAnotherOwnerConnectingThroughAnAlias) {
    const auto plan = review();
    ASSERT_TRUE(plan);
    auto admitted = formats->admit(consent(*plan), context);
    ASSERT_TRUE(admitted);
    auto other = context;
    other.owner_id = "another-owner";
    provider->carrier = "layout-alias";
    ASSERT_TRUE(devices->list(other.owner_id));
    const auto alias = devices->connect("candidate", other.owner_id, "raw");
    ASSERT_TRUE(alias);
    const auto inspected = formats->inspect({{"sessionId", alias->session_id}, {"targetId", "disk"}}, other);
    ASSERT_TRUE(inspected);
    EXPECT_FALSE(inspected->at("blockedReasons").empty());
    EXPECT_EQ(provider->disk->writes, 0U);
}

TEST_F(DeviceFormat, ResolvingAnUnknownRequestCannotOverflowTheGlobalLedger) {
    const auto operations = directory / "operations";
    for (std::size_t index = 0U; index < 4096U; ++index) {
        const auto retained = operations / std::to_string(index);
        ASSERT_TRUE(std::filesystem::create_directory(retained));
        std::ofstream record{retained / "operation.json"};
        record << Json{{"owner", "first-owner"}, {"operationId", std::to_string(index)},
                       {"kind", "unstarted"},    {"state", "no_write"},
                       {"fenced", true},         {"model", ""},
                       {"serial", ""},           {"targetLabel", "Unstarted request"},
                       {"targetSize", 0U},       {"identity", ""}};
        ASSERT_TRUE(record.good());
    }
    const auto resolved = devices->resolve("unknown-request", "another-owner");
    EXPECT_FALSE(resolved);
    if (!resolved)
        EXPECT_EQ(resolved.error().code, "device_operation_capacity");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{operations}, std::filesystem::directory_iterator{}),
              std::ptrdiff_t{4096});
    EXPECT_EQ(provider->disk->writes, 0U);
}
} // namespace device_format_test
