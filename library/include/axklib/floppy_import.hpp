#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/media.hpp"
#include "axklib/package.hpp"

namespace axk {

struct FloppyImportObject {
    std::string key;
    std::string name;
    std::string display_name;
    ObjectType type{ObjectType::unknown};
    std::uint64_t size_bytes{};
    std::vector<std::string> required_object_keys;
    std::string exclusion_reason;
};

struct FloppyImportExcludedFile {
    std::string member_name;
    std::string path;
    std::uint64_t size_bytes{};
};

struct FloppyImportInspection {
    bool complete{};
    std::string label;
    std::vector<FloppyDiskIdentity> members;
    std::optional<std::uint16_t> next_required_index;
    std::vector<FloppyImportObject> objects;
    std::vector<FloppyImportExcludedFile> excluded_files;
    std::vector<MediaValidationIssue> issues;
};

// One ordinary disk or one catalog-identified set, never unrelated disks combined.
class AXK_API FloppyImportSource {
  public:
    [[nodiscard]] static Result<FloppyImportSource> open(std::vector<FatImage> members,
                                                         const CancellationToken &cancellation = {});
    [[nodiscard]] const FloppyImportInspection &inspection() const noexcept;
    [[nodiscard]] Result<PortablePackage> prepare(std::span<const std::string> selected_object_keys,
                                                  const CancellationToken &cancellation = {}) const;

  private:
    FloppyImportSource(MediaKind kind, ObjectCatalog catalog, FloppyImportInspection inspection);
    MediaKind kind_;
    ObjectCatalog catalog_;
    FloppyImportInspection inspection_;
};

} // namespace axk
