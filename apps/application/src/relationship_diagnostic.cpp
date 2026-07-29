#include "relationship_diagnostic.hpp"

#include <string>

#include <nlohmann/json.hpp>

#include "axklib/relationship.hpp"

nlohmann::json axk::app::relationship_diagnostic(const ExcludedExtractionRelationship &relationship,
                                                 std::string_view message, std::optional<std::string_view> source,
                                                 std::optional<std::string_view> selector) {
    nlohmann::json result{
        {"code", "unconfirmed_relationship_excluded"},
        {"message", message},
        {"fatal", false},
        {"relationshipType", relationship.type},
        {"relationshipQuality", relationship_quality_name(relationship.quality)},
        {"reason", relationship.reason},
        {"sourceObjectKey", relationship.source_key},
        {"candidateObjectKeys", relationship.candidate_keys},
        {"basis", relationship.basis},
        {"assignmentState", assignment_state_name(relationship.assignment_state)},
    };
    if (relationship.target_key)
        result["targetObjectKey"] = *relationship.target_key;
    if (source)
        result["source"] = *source;
    if (selector)
        result["selector"] = *selector;
    return result;
}
