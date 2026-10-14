#pragma once

#include <filesystem>
#include <vector>

#include "axklib/native/source.hpp"
#include "axklib/package_archive.hpp"

namespace axk::conversion::file_internal {
struct Binding {
    std::filesystem::path path;
    package_internal::Sha256Digest digest;
    std::uint64_t size{};
};
struct Directory {
    std::filesystem::path path;
    std::vector<std::filesystem::path> members;
};
struct Sources {
    std::shared_ptr<const native::SourceSet> sources;
    std::vector<Binding> files;
    std::vector<Directory> directories;
};
[[nodiscard]] Result<Sources> admit(std::span<const std::filesystem::path> paths,
                                    const CancellationToken &cancellation);
[[nodiscard]] Result<void> verify(const Sources &sources, const CancellationToken &cancellation);
[[nodiscard]] Result<Binding> bind(const std::filesystem::path &path, const CancellationToken &cancellation);
} // namespace axk::conversion::file_internal
