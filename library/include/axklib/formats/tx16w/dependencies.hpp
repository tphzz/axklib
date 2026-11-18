#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/formats/tx16w/document.hpp"

namespace axk::tx16w::detail {

std::string reference_filename(std::string name, char kind, std::uint8_t slot);
std::string file_key(const FileIdentity &identity);
std::set<std::string> required_file_keys(const Document &document);
std::vector<native::SourceRequirement> source_requirements(std::span<const NativeObject> objects,
                                                           std::span<const NativeFile> files);
bool supplies_requirement(const Document &target, const Document &candidate);
bool supplies_requirement(const native::SourceRequirement &requirement, const native::SourceMember &candidate);
struct CompanionGroup {
    std::vector<std::string> member_ids;
    std::vector<std::string> ambiguous_requirements;
};
std::vector<std::size_t> nearby_order(std::string_view seed, std::span<const std::string> labels);
// Selected members are authoritative. Only newly adopted member IDs are returned.
Result<CompanionGroup> resolve_companions(const Document &seed, std::span<const native::SourceMember> selected,
                                          std::span<const native::SourceMember> candidates,
                                          std::span<const std::shared_ptr<const Document>> documents,
                                          const CancellationToken &cancellation = {});
std::vector<CompanionGroup> group_companions(std::span<const native::SourceMember> members,
                                             std::span<const std::shared_ptr<const Document>> documents);
Result<std::vector<std::shared_ptr<const native::Document>>>
open_documents(std::shared_ptr<const native::SourceSet> sources, const CancellationToken &cancellation);
Result<std::vector<std::shared_ptr<const native::Document>>>
open_grouped_documents(std::shared_ptr<const native::SourceSet> sources, const CancellationToken &cancellation,
                       std::span<const std::vector<std::string>> groups);

} // namespace axk::tx16w::detail
