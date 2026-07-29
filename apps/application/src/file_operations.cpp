#include "axklib/application/file_operations.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/terminology.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"

#include "file_operations_internal.hpp"

using namespace axk::app::file_operations_internal;

axk::app::Result<void> axk::app::bind_file_operations(OperationRegistry &registry, const Sandbox &sandbox) {
    if (!registry.is_implemented("report.info")) {
        auto bound =
            registry.bind("report.info", [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                return execute_info(sandbox, request, context);
            });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.objects")) {
        auto bound =
            registry.bind("report.objects", [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                return execute_objects(sandbox, request, context);
            });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.inventory")) {
        auto bound = registry.bind("report.inventory",
                                   [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                                       return execute_inventory(sandbox, request, context);
                                   });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.orphans")) {
        auto bound =
            registry.bind("report.orphans", [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                return execute_orphans(sandbox, request, context);
            });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.coverage")) {
        auto bound = registry.bind("report.coverage",
                                   [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                                       return execute_coverage(sandbox, request, context);
                                   });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.relationships")) {
        auto bound = registry.bind("report.relationships",
                                   [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                                       return execute_relationships(sandbox, request, context);
                                   });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("corpus.audit")) {
        auto bound =
            registry.bind("corpus.audit", [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                return execute_corpus_audit(sandbox, request, context);
            });
        if (!bound)
            return std::unexpected(bound.error());
    }
    return {};
}
