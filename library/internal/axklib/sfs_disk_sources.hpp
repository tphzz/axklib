#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "axklib/formats/a_series/native_document.hpp"
#include "axklib/native/registry.hpp"

namespace axk::inspection {

// File inspection and file conversion build the same logical SFS record bindings.
class SfsDiskSources {
  public:
    [[nodiscard]] static Result<bool> recognizes(const RandomAccessReader &reader);
    [[nodiscard]] Result<void> admit(native::SourceMember &member, const std::filesystem::path &path,
                                     std::string fingerprint, const CancellationToken &cancellation);
    [[nodiscard]] native::FormatProvider provider() &&;

  private:
    struct Disk {
        std::string member_id;
        std::string document_id;
        std::shared_ptr<const formats::a_series::Document> decoded;
        formats::a_series::NativeDocument::Locations locations;
    };
    std::vector<Disk> disks_;
};
} // namespace axk::inspection
