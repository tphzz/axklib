#pragma once

#include "axklib/formats/typhoon/document.hpp"
#include "session_formats.hpp"

namespace axk::app::formats::typhoon {
void configure_companions(NativeDocumentAdmission &, const AdmittedImageSources &,
                          std::shared_ptr<const axk::typhoon::Document>);
[[nodiscard]] Result<void> check_extension(const native::Document &previous, const axk::typhoon::Document &next);
[[nodiscard]] native::FormatProvider scoped_provider(const SessionFormats &);
} // namespace axk::app::formats::typhoon
