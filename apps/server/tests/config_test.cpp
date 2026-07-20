#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "axklib/server/config.hpp"
#include "environment.hpp"

namespace {

class TemporaryConfigFile {
  public:
    explicit TemporaryConfigFile(std::string_view document)
        : path_(std::filesystem::temp_directory_path() / "axklib-server-config-test.json") {
        std::ofstream output{path_, std::ios::binary | std::ios::trunc};
        output << document;
    }

    ~TemporaryConfigFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path &path() const { return path_; }

  private:
    std::filesystem::path path_;
};

class ScopedEnvironment {
  public:
    ScopedEnvironment(std::string name, std::string value) : name_(std::move(name)) {
        previous_ = axk::server::detail::environment_variable(name_);
        set(value);
    }

    ~ScopedEnvironment() {
        if (previous_)
            set(*previous_);
        else
            clear();
    }

  private:
    void set(const std::string &value) const {
#ifdef _WIN32
        static_cast<void>(_putenv_s(name_.c_str(), value.c_str()));
#else
        static_cast<void>(setenv(name_.c_str(), value.c_str(), 1));
#endif
    }

    void clear() const {
#ifdef _WIN32
        static_cast<void>(_putenv_s(name_.c_str(), ""));
#else
        static_cast<void>(unsetenv(name_.c_str()));
#endif
    }

    std::string name_;
    std::optional<std::string> previous_;
};

TEST(ServerConfig, ParsesSafeLoopbackConfiguration) {
    const auto root = std::filesystem::temp_directory_path();
    std::array arguments{std::string{"axklib-server"},
                         std::string{"--port"},
                         std::string{"0"},
                         std::string{"--token"},
                         std::string{"0123456789abcdef"},
                         std::string{"--workers"},
                         std::string{"3"},
                         std::string{"--job-workers"},
                         std::string{"4"},
                         std::string{"--write-job-workers"},
                         std::string{"2"},
                         std::string{"--max-queued-jobs"},
                         std::string{"17"},
                         std::string{"--max-retained-jobs"},
                         std::string{"31"},
                         std::string{"--job-retention-seconds"},
                         std::string{"60"},
                         std::string{"--job-replay-events"},
                         std::string{"23"},
                         std::string{"--max-event-tickets"},
                         std::string{"29"},
                         std::string{"--connection-file"},
                         (root / "axklib-server.json").string(),
                         std::string{"--parent-pid"},
                         std::string{"4242"},
                         std::string{"--workspace-store"},
                         (root / "workspaces.json").string()};
    std::array<char *, arguments.size()> pointers{};
    for (std::size_t index = 0; index < arguments.size(); ++index)
        pointers[index] = arguments[index].data();

    const auto parsed = axk::server::parse_command_line(static_cast<int>(pointers.size()), pointers.data());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->config.bind_address, "127.0.0.1");
    EXPECT_EQ(parsed->config.port, 0U);
    EXPECT_EQ(parsed->config.worker_threads, 3U);
    EXPECT_EQ(parsed->config.job_worker_threads, 4U);
    EXPECT_EQ(parsed->config.write_job_worker_threads, 2U);
    EXPECT_EQ(parsed->config.maximum_queued_jobs, 17U);
    EXPECT_EQ(parsed->config.maximum_retained_jobs, 31U);
    EXPECT_EQ(parsed->config.job_retention_seconds, 60U);
    EXPECT_EQ(parsed->config.replay_events_per_job, 23U);
    EXPECT_EQ(parsed->config.maximum_event_tickets, 29U);
    EXPECT_EQ(parsed->config.connection_file, root / "axklib-server.json");
    EXPECT_EQ(parsed->config.parent_process_id, 4242U);
    EXPECT_EQ(parsed->config.workspace_store, root / "workspaces.json");
}

TEST(ServerConfig, GeneratesSidecarTokenWithoutPuttingASecretInProcessArguments) {
    const auto root = std::filesystem::temp_directory_path();
    ScopedEnvironment inherited_bind{"AXKLIB_SERVER_BIND", "0.0.0.0"};
    ScopedEnvironment inherited_token{"AXKLIB_SERVER_TOKEN", "inherited-token-must-not-be-used"};
    std::array arguments{std::string{"axklib-server"},
                         std::string{"--port"},
                         std::string{"0"},
                         std::string{"--connection-file"},
                         (root / "axklib-sidecar.json").string(),
                         std::string{"--workspace-store"},
                         (root / "workspaces.json").string()};
    std::array<char *, arguments.size()> pointers{};
    for (std::size_t index = 0; index < arguments.size(); ++index)
        pointers[index] = arguments[index].data();

    const auto parsed = axk::server::parse_command_line(static_cast<int>(pointers.size()), pointers.data());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->config.bind_address, "127.0.0.1");
    EXPECT_EQ(parsed->config.bearer_token.size(), 64U);
    EXPECT_NE(parsed->config.bearer_token, "inherited-token-must-not-be-used");
    EXPECT_TRUE(std::ranges::all_of(parsed->config.bearer_token,
                                    [](unsigned char character) { return std::isxdigit(character) != 0; }));
}

TEST(ServerConfig, AppliesExplicitConfigInSidecarModeWithoutInheritedEnvironment) {
    const auto root = std::filesystem::current_path();
    ScopedEnvironment inherited_bind{"AXKLIB_SERVER_BIND", "0.0.0.0"};
    ScopedEnvironment inherited_token{"AXKLIB_SERVER_TOKEN", "inherited-token-must-not-be-used"};
    TemporaryConfigFile config{"{\"workspaceStore\":\"" + (root / "workspaces.json").generic_string() + "\"}"};
    std::array arguments{std::string{"axklib-server"},
                         std::string{"--config"},
                         config.path().string(),
                         std::string{"--port"},
                         std::string{"0"},
                         std::string{"--connection-file"},
                         (root / "axklib-sidecar-config-test.json").string()};
    std::array<char *, arguments.size()> pointers{};
    for (std::size_t index = 0; index < arguments.size(); ++index)
        pointers[index] = arguments[index].data();

    const auto parsed = axk::server::parse_command_line(static_cast<int>(pointers.size()), pointers.data());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->config.bind_address, "127.0.0.1");
    EXPECT_EQ(parsed->config.bearer_token.size(), 64U);
    EXPECT_NE(parsed->config.bearer_token, "inherited-token-must-not-be-used");
    EXPECT_FALSE(parsed->config.workspace_store.empty());
}

TEST(ServerConfig, AppliesDefaultsThenConfigFileThenCommandLineOverrides) {
    const auto root = std::filesystem::temp_directory_path();
    ScopedEnvironment port_override{"AXKLIB_SERVER_PORT", "8124"};
    TemporaryConfigFile config{"{\"bearerToken\":\"0123456789abcdef\",\"port\":8123,\"workerThreads\":3,"
                               "\"workspaceStore\":\"" +
                               (root / "configured-workspaces.json").generic_string() + "\"}"};
    std::array arguments{std::string{"axklib-server"},
                         std::string{"--config"},
                         config.path().string(),
                         std::string{"--port"},
                         std::string{"0"},
                         std::string{"--workers"},
                         std::string{"4"},
                         std::string{"--workspace-store"},
                         (root / "workspaces.json").string()};
    std::array<char *, arguments.size()> pointers{};
    for (std::size_t index = 0; index < arguments.size(); ++index)
        pointers[index] = arguments[index].data();

    const auto parsed = axk::server::parse_command_line(static_cast<int>(pointers.size()), pointers.data());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->config.port, 0U);
    EXPECT_EQ(parsed->config.worker_threads, 4U);
    EXPECT_EQ(parsed->config.workspace_store, root / "workspaces.json");

    std::array environment_arguments{std::string{"axklib-server"}, std::string{"--config"}, config.path().string()};
    std::array<char *, environment_arguments.size()> environment_pointers{};
    for (std::size_t index = 0; index < environment_arguments.size(); ++index)
        environment_pointers[index] = environment_arguments[index].data();
    const auto environment =
        axk::server::parse_command_line(static_cast<int>(environment_pointers.size()), environment_pointers.data());
    ASSERT_TRUE(environment) << environment.error().message;
    EXPECT_EQ(environment->config.port, 8124U);
}

TEST(ServerConfig, RejectsUnknownConfigKeys) {
    TemporaryConfigFile config{R"({"bearerToken":"0123456789abcdef","futureSetting":true})"};
    std::array arguments{std::string{"axklib-server"}, std::string{"--config"}, config.path().string()};
    std::array<char *, arguments.size()> pointers{};
    for (std::size_t index = 0; index < arguments.size(); ++index)
        pointers[index] = arguments[index].data();

    const auto parsed = axk::server::parse_command_line(static_cast<int>(pointers.size()), pointers.data());
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().message.find("futureSetting"), std::string::npos);
}

TEST(ServerConfig, RejectsInvalidJobResourceLimits) {
    auto config = axk::server::Config{};
    config.bearer_token = "0123456789abcdef";
    config.job_worker_threads = 0U;
    auto workers = axk::server::validate_config(config);
    ASSERT_FALSE(workers);
    EXPECT_NE(workers.error().message.find("job worker"), std::string::npos);

    config.job_worker_threads = 1U;
    config.maximum_queued_jobs = 0U;
    auto queue = axk::server::validate_config(config);
    ASSERT_FALSE(queue);
    EXPECT_NE(queue.error().message.find("queued jobs"), std::string::npos);
}

TEST(ServerConfig, ParsesAndBoundsConcurrentArchiveDownloads) {
    TemporaryConfigFile config{R"({"bearerToken":"0123456789abcdef","maximumConcurrentArchiveDownloads":7})"};
    std::array arguments{std::string{"axklib-server"}, std::string{"--config"}, config.path().string()};
    std::array<char *, arguments.size()> pointers{};
    for (std::size_t index = 0; index < arguments.size(); ++index)
        pointers[index] = arguments[index].data();
    const auto parsed = axk::server::parse_command_line(static_cast<int>(pointers.size()), pointers.data());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->config.maximum_concurrent_archive_downloads, 7U);

    auto invalid = parsed->config;
    invalid.maximum_concurrent_archive_downloads = 0U;
    EXPECT_FALSE(axk::server::validate_config(invalid));
    invalid.maximum_concurrent_archive_downloads = 65U;
    EXPECT_FALSE(axk::server::validate_config(invalid));
}

TEST(ServerConfig, RestrictsParentMonitoringToSidecarMode) {
    auto config = axk::server::Config{};
    config.bearer_token = "0123456789abcdef";
    config.parent_process_id = 42U;
    auto standalone = axk::server::validate_config(config);
    ASSERT_FALSE(standalone);
    EXPECT_NE(standalone.error().message.find("connection-file"), std::string::npos);

    config.connection_file = std::filesystem::temp_directory_path() / "axklib-sidecar.json";
    EXPECT_TRUE(axk::server::validate_config(config));
}

TEST(ServerConfig, RejectsInvalidJsonComplexityLimits) {
    auto config = axk::server::Config{};
    config.bearer_token = "0123456789abcdef";

    config.maximum_json_depth = 0U;
    auto depth = axk::server::validate_config(config);
    ASSERT_FALSE(depth);
    EXPECT_NE(depth.error().message.find("JSON and stream limits"), std::string::npos);

    config.maximum_json_depth = 32U;
    config.maximum_json_nodes = 0U;
    EXPECT_FALSE(axk::server::validate_config(config));
}

TEST(ServerConfig, RejectsInvalidWebSocketDeliveryBudgets) {
    auto config = axk::server::Config{};
    config.bearer_token = "0123456789abcdef";

    config.maximum_websocket_delivery_events = 0U;
    auto events = axk::server::validate_config(config);
    ASSERT_FALSE(events);
    EXPECT_NE(events.error().message.find("delivery budgets"), std::string::npos);

    config.maximum_websocket_delivery_events = 1U;
    config.maximum_websocket_delivery_bytes = 0U;
    EXPECT_FALSE(axk::server::validate_config(config));
}

TEST(ServerConfig, RejectsWeakAuthenticationAndUnsafeLanDefaults) {
    auto config = axk::server::Config{};
    config.bearer_token = "short";
    auto weak = axk::server::validate_config(config);
    ASSERT_FALSE(weak);
    EXPECT_EQ(weak.error().code, "invalid_server_argument");

    config.bearer_token = "0123456789abcdef";
    config.bind_address = "0.0.0.0";
    auto lan = axk::server::validate_config(config);
    ASSERT_FALSE(lan);
    EXPECT_NE(lan.error().message.find("plaintext"), std::string::npos);

    config.bearer_token.clear();
    config.token_hashes.push_back({"operator", "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"});
    lan = axk::server::validate_config(config);
    ASSERT_FALSE(lan);
    EXPECT_NE(lan.error().message.find("allowed origin"), std::string::npos);
    config.allowed_origins.push_back("https://sampler.example.test");
    EXPECT_TRUE(axk::server::validate_config(config));

    config.connection_file = std::filesystem::temp_directory_path() / "axklib-server.json";
    auto connection_file = axk::server::validate_config(config);
    ASSERT_FALSE(connection_file);
    EXPECT_NE(connection_file.error().message.find("loopback"), std::string::npos);
}

TEST(ServerConfig, ParsesNamedLanTokenHashesAndRejectsWildcards) {
    const auto root = std::filesystem::temp_directory_path();
    std::array arguments{
        std::string{"axklib-server"},
        std::string{"--bind"},
        std::string{"0.0.0.0"},
        std::string{"--token-sha256"},
        std::string{"operator=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"},
        std::string{"--allow-origin"},
        std::string{"https://sampler.example.test"},
        std::string{"--workspace-store"},
        (root / "workspaces.json").string(),
    };
    std::array<char *, arguments.size()> pointers{};
    for (std::size_t index = 0; index < arguments.size(); ++index)
        pointers[index] = arguments[index].data();

    const auto parsed = axk::server::parse_command_line(static_cast<int>(pointers.size()), pointers.data());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed->config.token_hashes.size(), 1U);
    EXPECT_EQ(parsed->config.token_hashes.front().principal_id, "operator");
    EXPECT_TRUE(parsed->config.bearer_token.empty());

    auto wildcard = parsed->config;
    wildcard.allowed_origins = {"*"};
    const auto rejected = axk::server::validate_config(wildcard);
    ASSERT_FALSE(rejected);
    EXPECT_NE(rejected.error().message.find("non-wildcard"), std::string::npos);
}

} // namespace
