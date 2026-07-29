#pragma once

#include "axklib/file_publication.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace axk::audio_export_detail {

std::string safe_component(std::string value, std::string_view fallback);
void append_publication_warnings(std::vector<std::string> &destination, const PublicationOutcome &publication);

} // namespace axk::audio_export_detail
