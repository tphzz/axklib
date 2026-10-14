#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "axklib/formats/tx16w/document.hpp"

namespace axk::tx16w::detail {

std::string reference_filename(std::string name, char kind, std::uint8_t slot);
std::vector<native::SourceRequirement> source_requirements(std::span<const NativeObject> objects,
                                                           std::span<const NativeFile> files);
bool supplies_requirement(const Document &target, const Document &candidate);
bool supplies_requirement(const native::SourceRequirement &requirement, const native::SourceMember &candidate);
Result<std::vector<std::shared_ptr<const native::Document>>>
open_documents(std::shared_ptr<const native::SourceSet> sources, const CancellationToken &cancellation);

} // namespace axk::tx16w::detail
