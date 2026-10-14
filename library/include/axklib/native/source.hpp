#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/filesystems/view.hpp"
#include "axklib/io.hpp"

namespace axk::native {

struct SourceMember {
    std::string id;
    std::string label;
    std::shared_ptr<const RandomAccessReader> reader;
    std::vector<filesystems::Root> roots;
    // Mutable sources must retain their admission lease and identity check in this callback.
    std::function<Result<void>()> verify_unchanged{};
};

struct SourceLocation {
    std::string member_id;
    std::string root_id;
    std::string entry_id;
    std::uint64_t offset{};
    std::uint64_t length{};

    friend bool operator==(const SourceLocation &, const SourceLocation &) = default;
};

class SourceSet final {
  public:
    static constexpr std::size_t maximum_members = 32U;
    static constexpr std::size_t maximum_entries = 1'000'000U;
    static constexpr std::uint64_t maximum_read_bytes = 16U * 1024U * 1024U;

    [[nodiscard]] static Result<std::shared_ptr<const SourceSet>> create(std::vector<SourceMember> members);
    [[nodiscard]] std::span<const SourceMember> members() const noexcept;
    [[nodiscard]] Result<void> verify(const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<std::vector<std::byte>> read(const SourceLocation &location,
                                                      const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<SourceLocation> locate(std::string_view member_id, std::string_view root_id,
                                                std::string_view entry_id) const;

  private:
    explicit SourceSet(std::vector<SourceMember> members);
    [[nodiscard]] Result<std::shared_ptr<const RandomAccessReader>> reader(const SourceLocation &location) const;
    std::vector<SourceMember> members_;
};

} // namespace axk::native
