#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/catalog.hpp"
#include "axklib/export.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sfs.hpp"

namespace axk {

enum class ObjectDeletionRole : std::uint8_t { target, dependency };
enum class ObjectDeletionStatus : std::uint8_t { required, optional, preserved, blocked };
enum class ObjectDeletionReferenceEffect : std::uint8_t { blocking, removed, preserved };

struct ObjectDeletionNotice {
    std::string code;
    std::string message;
    std::vector<std::string> object_keys;
};

struct ObjectDeletionImpact {
    std::string object_key;
    ObjectType object_type{ObjectType::unknown};
    std::string object_name;
    PartitionIndex partition;
    std::string partition_name;
    std::string volume_name;
    ObjectDeletionRole role{ObjectDeletionRole::dependency};
    ObjectDeletionStatus status{ObjectDeletionStatus::preserved};
    bool selected{};
    std::uint64_t stored_size_bytes{};
    std::uint64_t freed_clusters{};
    std::vector<std::string> prerequisite_keys;
    std::string reason;
};

struct ObjectDeletionReference {
    std::string source_key;
    std::string target_key;
    std::string type;
    RelationshipQuality quality{RelationshipQuality::unknown};
    ObjectDeletionReferenceEffect effect{ObjectDeletionReferenceEffect::preserved};
};

struct ObjectDeletionSelection {
    std::string target_key;
    std::vector<std::string> included_dependency_keys;
};

struct ObjectDeletionInspection {
    bool valid{};
    std::string target_key;
    std::vector<std::string> selected_keys;
    std::vector<ObjectDeletionImpact> impacts;
    std::vector<ObjectDeletionReference> references;
    std::vector<ObjectDeletionNotice> blockers;
    std::vector<ObjectDeletionNotice> warnings;
    std::uint64_t estimated_freed_bytes{};
    std::uint64_t estimated_freed_clusters{};
    AlterationManifest manifest;
};

AXK_AUDIO_API Result<ObjectDeletionInspection> inspect_object_deletion(const Container &container,
                                                                       const ObjectCatalog &catalog,
                                                                       const RelationshipGraph &graph,
                                                                       const ObjectDeletionSelection &selection);

AXK_AUDIO_API std::string_view object_deletion_role_name(ObjectDeletionRole role) noexcept;
AXK_AUDIO_API std::string_view object_deletion_status_name(ObjectDeletionStatus status) noexcept;
AXK_AUDIO_API std::string_view object_deletion_reference_effect_name(ObjectDeletionReferenceEffect effect) noexcept;

} // namespace axk
