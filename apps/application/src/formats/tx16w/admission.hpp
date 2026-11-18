#pragma once

#include "axklib/application/session_format_composition.hpp"
#include "native_document_admission.hpp"

namespace axk::app::formats::tx16w {

Result<std::vector<ImageSourceRef>> nearby_candidates(const Sandbox &sandbox, const ImageSourceRef &source,
                                                      const CancellationToken &cancellation);
ConfigureSessionFormats register_format;

} // namespace axk::app::formats::tx16w
