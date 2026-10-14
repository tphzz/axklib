#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "axklib/native/document.hpp"

namespace axk::native {

enum class RecognitionState : std::uint8_t { no_match, supported, incomplete, invalid, limit_exceeded };

struct FormatDescriptor {
    std::string id;
    std::string name;
    std::vector<std::string> profiles;
};

struct Recognition {
    std::string format_id;
    RecognitionState state{RecognitionState::no_match};
    std::vector<Diagnostic> diagnostics;
    std::vector<SourceLocation> claims{};
};

struct FormatProvider {
    FormatDescriptor descriptor;
    std::function<Result<Recognition>(const SourceSet &, const CancellationToken &)> recognize;
    std::function<Result<std::vector<std::shared_ptr<const Document>>>(std::shared_ptr<const SourceSet>,
                                                                       const CancellationToken &)>
        open;
};

struct OpenResult {
    std::vector<Recognition> candidates;
    std::vector<std::shared_ptr<const Document>> documents;
    bool selection_required{};
};

class FormatRegistry final {
  public:
    [[nodiscard]] Result<void> add(FormatProvider provider);
    [[nodiscard]] std::vector<FormatDescriptor> formats() const;
    [[nodiscard]] Result<OpenResult> open(std::shared_ptr<const SourceSet> sources,
                                          const std::vector<std::string> &selected_formats = {},
                                          const CancellationToken &cancellation = {}) const;

  private:
    std::vector<FormatProvider> providers_;
};

} // namespace axk::native
