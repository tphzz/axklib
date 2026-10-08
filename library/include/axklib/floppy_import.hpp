#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "axklib/relationship.hpp"

namespace axk {

struct FloppyImportFileSource {
    std::string member_name;
    std::string path;
    std::uint64_t size_bytes{};
};

struct FloppyImportObject {
    std::string key;
    std::string name;
    std::string display_name;
    ObjectType type{ObjectType::unknown};
    std::uint64_t size_bytes{};
    std::vector<std::string> required_object_keys;
    std::string exclusion_reason;
    std::vector<FloppyImportFileSource> sources;
};

struct FloppyImportExcludedFile {
    std::string member_name;
    std::string path;
    std::uint64_t size_bytes{};
    std::string reason;
    bool unreadable_object{};
};

struct FloppyImportInspection {
    bool complete{};
    bool can_import{};
    bool recovery_used{};
    bool requires_acknowledgement{};
    std::string label;
    std::vector<FloppyDiskIdentity> members;
    std::optional<std::uint16_t> next_required_index;
    std::vector<FloppyImportObject> objects;
    std::vector<FloppyImportExcludedFile> excluded_files;
    std::vector<MediaValidationIssue> issues;
};

struct FloppyImportDirectory {
    std::string name;
    std::vector<AxkObjectDirectoryEntry> entries;
};

// Explicit source selection may recover complete objects without certifying a disk set.
class AXK_API FloppyImportSource {
  public:
    [[nodiscard]] static Result<FloppyImportSource> open(std::vector<FatImage> members,
                                                         const CancellationToken &cancellation = {});
    [[nodiscard]] static Result<FloppyImportSource> open_directories(std::vector<FloppyImportDirectory> sources,
                                                                     const CancellationToken &cancellation = {});
    [[nodiscard]] const FloppyImportInspection &inspection() const noexcept;
    [[nodiscard]] Result<PortablePackage> prepare(std::span<const std::string> selected_object_keys,
                                                  const CancellationToken &cancellation = {}) const;

  private:
    FloppyImportSource(MediaKind kind, ObjectCatalog catalog, RelationshipGraph relationships,
                       FloppyImportInspection inspection);
    MediaKind kind_;
    ObjectCatalog catalog_;
    RelationshipGraph relationships_;
    FloppyImportInspection inspection_;
};

} // namespace axk
