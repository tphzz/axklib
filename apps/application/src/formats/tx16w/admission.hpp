#pragma once

#include "axklib/formats/tx16w/document.hpp"
#include "native_document_admission.hpp"

namespace axk::app::formats::tx16w {

NativeDocumentAdmission admit_document(std::shared_ptr<const axk::tx16w::Document> document,
                                       const AdmittedImageSources &sources);

} // namespace axk::app::formats::tx16w
