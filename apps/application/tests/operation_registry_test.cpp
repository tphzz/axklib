#include <set>
#include <string>

#include <gtest/gtest.h>

#include "axklib/application/operation_registry.hpp"

namespace {

TEST(OperationRegistry, DeclaresEveryMaintainedCliParityOperationExactlyOnce) {
    const auto registry = axk::app::make_operation_registry();
    const auto entries = registry.entries();
    EXPECT_EQ(entries.size(), 27U);

    std::set<std::string> ids;
    std::set<std::string> cli_commands;
    std::size_t parity_count{};
    for (const auto &entry : entries) {
        EXPECT_TRUE(ids.insert(entry.descriptor.id).second);
        if (entry.descriptor.cli_parity) {
            ++parity_count;
            EXPECT_FALSE(entry.descriptor.cli_command.empty());
            EXPECT_TRUE(cli_commands.insert(entry.descriptor.cli_command).second);
        } else {
            EXPECT_TRUE(entry.descriptor.cli_command.empty());
        }
        EXPECT_TRUE(entry.descriptor.route.starts_with("/api/v1/"));
        EXPECT_FALSE(entry.descriptor.request_schema.empty());
        EXPECT_FALSE(entry.descriptor.result_schema.empty());
    }
    EXPECT_EQ(parity_count, 22U);
}

TEST(OperationRegistry, InvokesTypedSystemVersionWithoutTransportTypes) {
    const auto registry = axk::app::make_operation_registry();
    const auto result = registry.invoke("system.version", nlohmann::json::object(), {});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("apiVersion"), "v1");
    EXPECT_EQ(result->at("eventSchemaVersion"), "1");
    EXPECT_FALSE(result->at("semanticVersion").get<std::string>().empty());
    EXPECT_FALSE(result->at("sourceIdentity").get<std::string>().empty());
}

TEST(OperationRegistry, RejectsUnknownMalformedAndUnimplementedOperations) {
    const auto registry = axk::app::make_operation_registry();

    const auto unknown = registry.invoke("missing", nlohmann::json::object(), {});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, "unknown_operation");

    const auto malformed = registry.invoke("system.version", nlohmann::json{{"unexpected", true}}, {});
    ASSERT_FALSE(malformed);
    EXPECT_EQ(malformed.error().code, "invalid_request");

    const auto incomplete = registry.invoke("report.info", nlohmann::json::object(), {});
    ASSERT_FALSE(incomplete);
    EXPECT_EQ(incomplete.error().code, "operation_not_implemented");
}

TEST(OperationRegistry, RejectsDuplicateIdentifiersAndCliCommandPaths) {
    axk::app::OperationRegistry registry;
    const axk::app::OperationDescriptor first{
        "one",        "axklib one", axk::app::HttpMethod::post, "/api/v1/one", axk::app::ExecutionMode::request, {},
        "OneRequest", "OneResponse"};
    ASSERT_TRUE(registry.declare(first));

    auto duplicate_id = first;
    duplicate_id.cli_command = "axklib two";
    duplicate_id.route = "/api/v1/two";
    ASSERT_FALSE(registry.declare(duplicate_id));
    EXPECT_EQ(duplicate_id.id, "one");

    auto duplicate_cli = first;
    duplicate_cli.id = "two";
    duplicate_cli.route = "/api/v1/two";
    const auto rejected = registry.declare(duplicate_cli);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, "duplicate_operation");
}

} // namespace
