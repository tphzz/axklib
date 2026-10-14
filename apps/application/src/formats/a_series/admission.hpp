#pragma once

#include "native_document_admission.hpp"

namespace axk::app::formats::a_series {

[[nodiscard]] Result<std::vector<NativeDocumentAdmission>>
admit_documents(const Sandbox &sandbox, const AdmittedImageSources &sources, const CancellationToken &cancellation,
                ProgressSink *progress, diagnostic_sink *diagnostics);

} // namespace axk::app::formats::a_series
