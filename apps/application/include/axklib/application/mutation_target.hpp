#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "axklib/application/contracts.hpp"
#include "axklib/io.hpp"

namespace axk::app {

enum class MutationSourceKind : std::uint8_t { file, device };

struct MutationIdentity {
    MutationSourceKind kind{MutationSourceKind::file};
    std::string scope;
    std::string key;

    MutationIdentity() = default;
    explicit MutationIdentity(const FileRef &file) : scope(file.root_id), key(file.relative_path) {}
    MutationIdentity(MutationSourceKind source_kind, std::string source_scope, std::string source_key)
        : kind(source_kind), scope(std::move(source_scope)), key(std::move(source_key)) {}

    friend bool operator==(const MutationIdentity &, const MutationIdentity &) = default;
};

class MutationTarget : public RandomAccessReader {
  public:
    [[nodiscard]] virtual Result<void> write_exact_at(std::uint64_t offset, std::span<const std::byte> source) = 0;
    [[nodiscard]] virtual Result<void> flush() = 0;
    [[nodiscard]] virtual Result<void> verify_bound() const = 0;
    [[nodiscard]] virtual Result<void> verify_unchanged() const = 0;
    [[nodiscard]] virtual std::string revision() const = 0;
    [[nodiscard]] virtual std::string stable_identity() const = 0;
    [[nodiscard]] virtual MutationIdentity journal_identity() const = 0;
    [[nodiscard]] virtual std::uint32_t write_alignment() const noexcept { return 1U; }
};

} // namespace axk::app
