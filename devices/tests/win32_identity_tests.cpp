#include "test_util.hpp"
#include "win32_identity.hpp"

#include <cstdint>
#include <expected>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <system_error>

using namespace rawdisk;
using namespace rawdisk::detail;

namespace {

struct Node {
    std::string id;
    std::uint64_t arrival;
    std::uint32_t parent;
};

struct Api {
    std::map<std::uint32_t, Node> nodes;
    std::optional<std::uint32_t> failed_arrival{};
    std::optional<std::uint32_t> removed_on_arrival{};
    unsigned id_queries{};

    Result<std::uint32_t> locate(const std::string &id) {
        for (const auto &[instance, node] : nodes)
            if (node.id == id)
                return instance;
        return std::unexpected(std::make_error_code(std::errc::no_such_device));
    }

    Result<std::string> device_id(std::uint32_t node) {
        ++id_queries;
        const auto found = nodes.find(node);
        if (found == nodes.end())
            return std::unexpected(std::make_error_code(std::errc::no_such_device));
        return found->second.id;
    }

    Result<std::uint64_t> last_arrival(std::uint32_t node) {
        if (removed_on_arrival == node)
            nodes.erase(node);
        if (failed_arrival == node)
            return std::unexpected(std::make_error_code(std::errc::permission_denied));
        const auto found = nodes.find(node);
        if (found == nodes.end())
            return std::unexpected(std::make_error_code(std::errc::no_such_device));
        return found->second.arrival;
    }

    Result<std::uint32_t> parent(std::uint32_t node) {
        const auto found = nodes.find(node);
        if (found == nodes.end())
            return std::unexpected(std::make_error_code(std::errc::no_such_device));
        return found->second.parent;
    }
};

Api connection(std::uint32_t offset = 0) {
    return {{{1U + offset, {R"(USBSTOR\DISK&VEN_READER\SERIAL&0)", 200, 2U + offset}},
             {2U + offset, {R"(USB\VID_1234&PID_5678&MI_00\INTERFACE)", 150, 3U + offset}},
             {3U + offset, {R"(USB\VID_1234&PID_5678\SERIAL)", 100, 0}}}};
}

void same_connection_in_different_processes() {
    auto backend = connection();
    auto helper = connection(500);
    const auto selected = win32_node_identity(backend, 1);
    const auto opened = win32_node_identity(helper, 501);
    CHECK(selected && opened);
    if (selected && opened) {
        CHECK(selected->carrier_id == opened->carrier_id);
        CHECK(selected->connection_id == opened->connection_id);
        CHECK(!selected->carrier_id.empty());
        CHECK(!selected->connection_id.empty());
    }
}

void replug_with_reused_handles() {
    auto api = connection();
    const auto selected = win32_node_identity(api, 1);
    ++api.nodes.at(3).arrival;
    const auto refreshed = win32_node_identity(api, 1);
    CHECK(selected && refreshed);
    if (selected && refreshed) {
        CHECK(selected->carrier_id == refreshed->carrier_id);
        CHECK(selected->connection_id != refreshed->connection_id);
    }
}

void targeted_checks_relocate_the_disk_and_reject_a_new_arrival() {
    auto original = connection();
    const auto bound = win32_node_identity(original, 1);
    CHECK(bound.has_value());
    if (!bound)
        return;
    auto current = connection(500);
    CHECK(win32_revalidate_identity(current, *bound).has_value());
    ++current.nodes.at(501).arrival;
    CHECK(err(win32_revalidate_identity(current, *bound)) == Errc::device_connection_changed);
    current = connection(500);
    current.nodes.erase(501);
    CHECK(err(win32_revalidate_identity(current, *bound)) == std::errc::no_such_device);
    current = connection(500);
    current.failed_arrival = 503;
    CHECK(err(win32_revalidate_identity(current, *bound)) == std::errc::permission_denied);
}

void disk_restart_without_parent_replug() {
    auto api = connection();
    const auto selected = win32_node_identity(api, 1);
    ++api.nodes.at(1).arrival;
    const auto refreshed = win32_node_identity(api, 1);
    CHECK(selected && refreshed);
    if (selected && refreshed)
        CHECK(selected->connection_id != refreshed->connection_id);
}

void removal_during_identity_query_and_retry() {
    auto api = connection();
    const auto selected = win32_node_identity(api, 1);
    api.removed_on_arrival = 3;
    CHECK(err(win32_node_identity(api, 1)) == std::errc::no_such_device);
    api = connection(500);
    const auto retry = win32_node_identity(api, 501);
    CHECK(selected && retry);
    if (selected && retry)
        CHECK(selected->connection_id == retry->connection_id);
}

void sibling_disks_have_distinct_connections() {
    auto api = connection();
    const auto first = win32_node_identity(api, 1);
    api.nodes.at(1).id += "-LUN1";
    const auto sibling = win32_node_identity(api, 1);
    CHECK(first && sibling);
    if (first && sibling) {
        CHECK(first->carrier_id == sibling->carrier_id);
        CHECK(first->connection_id != sibling->connection_id);
    }
}

void incomplete_identity_is_not_admitted() {
    for (const auto node : {1U, 3U}) {
        auto api = connection();
        api.nodes.at(node).arrival = 0;
        CHECK(err(win32_node_identity(api, 1)) == Errc::device_identity_unavailable);
        api = connection();
        api.nodes.at(node).id.clear();
        CHECK(err(win32_node_identity(api, 1)) == Errc::device_identity_unavailable);
        api = connection();
        api.failed_arrival = node;
        CHECK(err(win32_node_identity(api, 1)) == std::errc::permission_denied);
    }
    auto removed = connection();
    removed.nodes.erase(3);
    CHECK(err(win32_node_identity(removed, 1)) == std::errc::no_such_device);
}

void traversal_is_bounded_and_selects_the_usb_parent() {
    auto api = connection();
    const auto identity = win32_node_identity(api, 1);
    CHECK(identity && identity->carrier_id == api.nodes.at(3).id);
    api.nodes.at(3).id = "ROOT";
    api.nodes.at(3).parent = 3;
    api.id_queries = 0;
    CHECK(err(win32_node_identity(api, 1)) == Errc::device_identity_unavailable);
    CHECK(api.id_queries <= 65);
}

void identity_mismatch_reasons() {
    const DeviceInfo expected{.path = R"(\\.\PhysicalDrive2)",
                              .model = "Reader",
                              .serial = "Serial",
                              .size_bytes = 4096,
                              .sector_size = 512,
                              .carrier_id = "USB parent",
                              .connection_id = "disk arrival"};
    CHECK(!win32_identity_mismatch(expected, expected));
    auto actual = expected;
    actual.connection_id = "new arrival";
    CHECK(win32_identity_mismatch(actual, expected) == Errc::device_connection_changed);
    actual = expected;
    actual.carrier_id = "another parent";
    CHECK(win32_identity_mismatch(actual, expected) == Errc::device_connection_changed);
    actual = expected;
    actual.connection_id.clear();
    CHECK(win32_identity_mismatch(actual, expected) == Errc::device_identity_unavailable);
    actual = expected;
    actual.model = "Other reader";
    CHECK(win32_identity_mismatch(actual, expected) == Errc::device_descriptor_changed);
    actual = expected;
    actual.serial = "Other serial";
    CHECK(win32_identity_mismatch(actual, expected) == Errc::device_descriptor_changed);
    actual = expected;
    actual.size_bytes *= 2;
    CHECK(win32_identity_mismatch(actual, expected) == Errc::device_geometry_changed);
    actual = expected;
    actual.sector_size *= 2;
    CHECK(win32_identity_mismatch(actual, expected) == Errc::device_geometry_changed);
    actual = expected;
    actual.path = R"(\\.\PhysicalDrive3)";
    CHECK(win32_identity_mismatch(actual, expected) == Errc::device_changed);
}

} // namespace

int main() {
    same_connection_in_different_processes();
    replug_with_reused_handles();
    targeted_checks_relocate_the_disk_and_reject_a_new_arrival();
    disk_restart_without_parent_replug();
    removal_during_identity_query_and_retry();
    sibling_disks_have_distinct_connections();
    incomplete_identity_is_not_admitted();
    traversal_is_bounded_and_selects_the_usb_parent();
    identity_mismatch_reasons();
    return finish_tests();
}
