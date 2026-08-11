#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "axklib/application/contracts.hpp"

namespace axk::server {

class StateNamespaceLease {
  public:
    StateNamespaceLease();
    ~StateNamespaceLease();
    StateNamespaceLease(StateNamespaceLease &&) noexcept;
    StateNamespaceLease &operator=(StateNamespaceLease &&) noexcept;
    StateNamespaceLease(const StateNamespaceLease &) = delete;
    StateNamespaceLease &operator=(const StateNamespaceLease &) = delete;

    [[nodiscard]] static app::Result<StateNamespaceLease> acquire(std::vector<std::filesystem::path> lock_files);

  private:
    struct Implementation;
    explicit StateNamespaceLease(std::unique_ptr<Implementation> implementation);

    std::unique_ptr<Implementation> implementation_;
};

} // namespace axk::server
