#pragma once

#include "axklib/formats/tx16w/document.hpp"
#include "native_document_admission.hpp"

namespace axk::app::formats::tx16w {

Result<std::vector<ImageSourceRef>> nearby_candidates(const Sandbox &sandbox, const ImageSourceRef &source,
                                                      const CancellationToken &cancellation);
NativeDocumentAdmission admit_document(std::shared_ptr<const axk::tx16w::Document> document,
                                       const AdmittedImageSources &sources);

} // namespace axk::app::formats::tx16w
