#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "axklib/native/registry.hpp"
#include "native_document_admission.hpp"

namespace axk::app {

struct SessionFormatContext {
    const AdmittedImageSources &sources;
    const CancellationToken &cancellation;
    ProgressSink *progress;
    diagnostic_sink *diagnostics;
};

// Built per discovery. A binding may retain the admissions it prepared for this open only.
class SessionFormats {
  public:
    using Bind = std::function<Result<NativeDocumentAdmission>(const std::shared_ptr<const native::Document> &)>;

    void retain_member_group(const std::string &format, std::vector<std::string> member_ids);
    [[nodiscard]] std::vector<std::vector<std::string>> member_groups(const std::string &format) const;
    [[nodiscard]] Result<void> add(native::FormatProvider provider, Bind bind);
    [[nodiscard]] const native::FormatRegistry &registry() const noexcept { return registry_; }
    [[nodiscard]] Result<NativeDocumentAdmission> bind(const std::shared_ptr<const native::Document> &document) const;

  private:
    native::FormatRegistry registry_;
    std::map<std::string, Bind> bindings_;
    std::map<std::string, std::vector<std::vector<std::string>>> member_groups_;
};

} // namespace axk::app
