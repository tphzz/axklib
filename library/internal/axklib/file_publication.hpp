#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>

#include "axklib/error.hpp"
#include "axklib/publication.hpp"

namespace axk::detail {

using TemporaryFileSink = std::function<Result<void>(std::span<const std::byte>)>;
using TemporaryFileProducer = std::function<Result<void>(const TemporaryFileSink &)>;

enum class PublicationMode : std::uint8_t { create_only, replace_existing };

struct PublicationHooks {
    std::function<void()> before_publish;
};

class TemporaryPublication final {
  public:
    static Result<TemporaryPublication> create(const std::filesystem::path &destination);
    static Result<TemporaryPublication> create(const std::filesystem::path &destination,
                                               std::shared_ptr<const PublicationHooks> hooks);
    static Result<TemporaryPublication> create(const std::filesystem::path &destination,
                                               const TemporaryFileProducer &producer);

    TemporaryPublication(TemporaryPublication &&other) noexcept;
    TemporaryPublication &operator=(TemporaryPublication &&other) noexcept;
    TemporaryPublication(const TemporaryPublication &) = delete;
    TemporaryPublication &operator=(const TemporaryPublication &) = delete;
    ~TemporaryPublication();

    [[nodiscard]] const std::filesystem::path &path() const noexcept;
    [[nodiscard]] Result<void> append(std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> write_at(std::uint64_t offset, std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> resize(std::uint64_t size);
    [[nodiscard]] Result<void> flush();
    [[nodiscard]] Result<axk::PublicationOutcome> publish(PublicationMode mode);
    [[nodiscard]] Result<void> discard();

  private:
    struct Impl;

    explicit TemporaryPublication(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace axk::detail
