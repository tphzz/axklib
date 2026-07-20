#pragma once

#include <cstddef>
#include <memory>
#include <optional>

namespace axk::server {

class ArchiveDownloadBudget {
  private:
    struct State;

  public:
    class Lease {
      public:
        Lease(Lease &&) noexcept = default;
        Lease &operator=(Lease &&) = delete;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        ~Lease();

      private:
        explicit Lease(std::shared_ptr<State> state) : state_(std::move(state)) {}

        std::shared_ptr<State> state_;
        friend class ArchiveDownloadBudget;
    };

    explicit ArchiveDownloadBudget(std::size_t maximum_active);

    [[nodiscard]] std::optional<Lease> try_acquire();
    [[nodiscard]] std::size_t active() const;

  private:
    std::shared_ptr<State> state_;
};

} // namespace axk::server
