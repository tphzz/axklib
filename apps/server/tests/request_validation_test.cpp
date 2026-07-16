#include <string>

#include <gtest/gtest.h>

#include "axklib/server/request_validation.hpp"

namespace {

axk::server::Config constrained_config() {
    axk::server::Config config;
    config.maximum_json_bytes = 64U;
    config.maximum_json_depth = 3U;
    config.maximum_json_nodes = 5U;
    config.maximum_json_container_items = 2U;
    config.maximum_json_string_bytes = 4U;
    return config;
}

void expect_error(std::string_view body, const axk::server::Config &config, std::string_view code) {
    const auto parsed = axk::server::parse_json_request(body, config);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().code, code);
}

TEST(ServerRequestValidation, AcceptsOneBoundedJsonObject) {
    const auto parsed = axk::server::parse_json_request(R"({"a":[1,2]})", constrained_config());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->at("a").size(), 2U);
}

TEST(ServerRequestValidation, RejectsOversizedAndMalformedBodies) {
    auto config = constrained_config();
    config.maximum_json_bytes = 4U;
    expect_error(R"({"a":1})", config, "request_too_large");
    expect_error("{", constrained_config(), "invalid_json");
    expect_error("[]", constrained_config(), "invalid_json");
}

TEST(ServerRequestValidation, RejectsEveryConfiguredStructureLimit) {
    const auto config = constrained_config();
    expect_error(R"({"a":{"b":{"c":1}}})", config, "json_structure_too_large");
    expect_error(R"({"a":1,"b":2,"c":3})", config, "json_structure_too_large");
    expect_error(R"({"a":[1,2,3]})", config, "json_structure_too_large");
    expect_error(R"({"abcde":1})", config, "json_structure_too_large");
    expect_error(R"({"a":"abcde"})", config, "json_structure_too_large");

    auto node_limited = config;
    node_limited.maximum_json_nodes = 3U;
    expect_error(R"({"a":[1,2]})", node_limited, "json_structure_too_large");
}

} // namespace
