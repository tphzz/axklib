#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/native/source.hpp"

namespace axk::native {

enum class Completeness : std::uint8_t { complete, incomplete, invalid };
enum class Severity : std::uint8_t { info, warning, error };
enum class Resolution : std::uint8_t { resolved, missing, ambiguous };
enum class DependencyState : std::uint8_t { missing, ambiguous, unsupported, invalid };

struct SourceRequirement {
    std::string name;
    DependencyState state{DependencyState::missing};
    std::optional<std::string> object_id;
};

struct Capability {
    std::string operation;
    bool available{};
    std::string reason;
};

struct DocumentSummary {
    std::string id;
    std::string format_id;
    std::string profile;
    std::string name;
    Completeness completeness{Completeness::complete};
    std::vector<Capability> capabilities;
    std::vector<SourceRequirement> source_requirements{};
};

struct ObjectSummary {
    std::string id;
    std::string type;
    std::string name;
    std::vector<SourceLocation> locations;
    std::vector<Capability> capabilities;
};

struct Relationship {
    std::string id;
    std::string source_id;
    std::optional<std::string> target_id;
    std::vector<std::string> candidates;
    std::string type;
    std::string native_reference;
    Resolution resolution{Resolution::missing};
};

struct Node {
    std::string id;
    std::optional<std::string> parent_id;
    std::optional<std::string> object_id;
    std::string type;
    std::string name;
};

struct Diagnostic {
    std::string code;
    Severity severity{Severity::warning};
    std::string message;
    std::optional<std::string> object_id;
};

template <typename T> struct Page {
    std::vector<T> items;
    std::size_t total{};
};

struct Catalog {
    std::vector<ObjectSummary> objects;
    std::vector<Relationship> relationships;
    std::vector<Node> nodes;
    std::vector<Diagnostic> diagnostics;
};

class Document {
  public:
    virtual ~Document() = default;
    [[nodiscard]] virtual const DocumentSummary &summary() const noexcept = 0;
    [[nodiscard]] virtual const Catalog &catalog() const noexcept = 0;
    [[nodiscard]] virtual const std::shared_ptr<const SourceSet> &sources() const noexcept = 0;

    [[nodiscard]] Result<Page<ObjectSummary>> objects(std::size_t offset, std::size_t limit,
                                                      const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<Page<Relationship>> relationships(std::size_t offset, std::size_t limit,
                                                           const CancellationToken &cancellation = {}) const;
};

} // namespace axk::native
