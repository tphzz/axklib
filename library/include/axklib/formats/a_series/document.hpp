#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"

namespace axk::formats::a_series {

// Native A-series state. Storage and transport consumers must not reconstruct
// the graph or interpret payloads independently of this format owner.
struct DocumentState {
    MediaContainer media;
    MediaInventory inventory;
    RelationshipGraph graph;
    ContentTree tree;
    ValidationReport validation;
    std::unordered_map<std::string, const MediaObjectDescriptor *> descriptors;
};

class Document final {
  public:
    Document(const Document &) = delete;
    Document &operator=(const Document &) = delete;
    [[nodiscard]] static Result<std::shared_ptr<const Document>> open(MediaContainer media,
                                                                      const CancellationToken &cancellation = {},
                                                                      ProgressSink *progress = nullptr,
                                                                      diagnostic_sink *diagnostics = nullptr);
    [[nodiscard]] const DocumentState &state() const noexcept;

  private:
    explicit Document(DocumentState state);
    DocumentState state_;
};

} // namespace axk::formats::a_series
