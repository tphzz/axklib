#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "axklib/formats/tx16w/codec.hpp"
#include "axklib/native/document.hpp"
#include "axklib/native/registry.hpp"

namespace axk::tx16w {

struct OpaqueFile {
    char kind{};
};

using ObjectValue = std::variant<WaveHeader, NativeSetup, Voice, Timbre, Performance, OpaqueFile>;

struct NativeObject {
    std::string id;
    native::SourceLocation location;
    FileIdentity file;
    ObjectValue value;
};

struct NativeFile {
    native::SourceLocation location;
    FileIdentity identity;
    bool decoded{};
    bool unsupported{};
};

class Document final : public native::Document {
  public:
    [[nodiscard]] static Result<std::shared_ptr<const Document>>
    open(std::shared_ptr<const native::SourceSet> sources, const CancellationToken &cancellation = {},
         std::span<const std::string> member_ids = {}, std::span<const std::string> ambiguous_requirements = {});
    [[nodiscard]] const native::DocumentSummary &summary() const noexcept override;
    [[nodiscard]] const native::Catalog &catalog() const noexcept override;
    [[nodiscard]] const std::shared_ptr<const native::SourceSet> &sources() const noexcept override;
    [[nodiscard]] std::span<const NativeObject> native_objects() const noexcept;
    [[nodiscard]] std::span<const NativeFile> native_files() const noexcept;
    [[nodiscard]] Result<const NativeObject *> object(std::string_view id) const;
    [[nodiscard]] Result<Wave> read_wave(std::string_view id, const CancellationToken &cancellation = {}) const;

  private:
    explicit Document(std::shared_ptr<const native::SourceSet> sources);
    native::DocumentSummary summary_;
    native::Catalog catalog_;
    std::shared_ptr<const native::SourceSet> sources_;
    std::vector<NativeObject> objects_;
    std::vector<NativeFile> files_;
};

[[nodiscard]] native::FormatProvider provider();

} // namespace axk::tx16w
