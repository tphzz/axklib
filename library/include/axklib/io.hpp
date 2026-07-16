#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"

namespace axk {

enum class ProgressPhase : std::uint8_t {
    opening,
    reading,
    resolving,
    validating,
    exporting,
    writing,
    allocating,
    publishing,
};

struct Progress {
    ProgressPhase phase{ProgressPhase::reading};
    std::uint64_t completed{};
    std::optional<std::uint64_t> total;
    std::string label;
    std::optional<std::string> output_path;
};

class AXK_API ProgressSink {
  public:
    virtual ~ProgressSink() = default;
    virtual void report(const Progress &progress) noexcept = 0;
};

class AXK_API CancellationToken {
  public:
    CancellationToken();

    [[nodiscard]] bool is_cancelled() const noexcept;
    [[nodiscard]] Result<void> check() const;

  private:
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state) noexcept;
    std::shared_ptr<std::atomic_bool> state_;

    friend class CancellationSource;
};

class AXK_API CancellationSource {
  public:
    CancellationSource();

    [[nodiscard]] CancellationToken token() const noexcept;
    void cancel() const noexcept;
    void reset() const noexcept;

  private:
    std::shared_ptr<std::atomic_bool> state_;
};

class AXK_API RandomAccessReader {
  public:
    virtual ~RandomAccessReader() = default;
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;
    [[nodiscard]] virtual Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const = 0;
};

class AXK_API MemoryReader final : public RandomAccessReader {
  public:
    explicit MemoryReader(std::vector<std::byte> bytes);

    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override;

  private:
    std::vector<std::byte> bytes_;
};

class AXK_API FileReader final : public RandomAccessReader {
  public:
    [[nodiscard]] static Result<std::shared_ptr<FileReader>> open(const std::filesystem::path &path);

    [[nodiscard]] std::uint64_t size() const noexcept override;
    [[nodiscard]] Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override;
    [[nodiscard]] Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination,
                                             const CancellationToken &cancellation) const;

  private:
    FileReader(std::filesystem::path path, std::uint64_t size, std::ifstream input) noexcept;

    std::filesystem::path path_;
    std::uint64_t size_{};
    mutable std::mutex mutex_;
    mutable std::ifstream input_;
};

} // namespace axk
