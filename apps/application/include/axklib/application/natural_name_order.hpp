#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/application/contracts.hpp"

namespace axk::app {

struct NaturalNameKey {
    std::vector<std::uint8_t> collation;
    std::string original;

    auto operator<=>(const NaturalNameKey &) const = default;
};

// One request-local collator; ICU types stay inside the application implementation.
class NaturalNameOrder {
  public:
    static Result<NaturalNameOrder> create();
    NaturalNameOrder(NaturalNameOrder &&) noexcept;
    NaturalNameOrder &operator=(NaturalNameOrder &&) noexcept;
    ~NaturalNameOrder();

    Result<NaturalNameKey> key(std::string_view name) const;

  private:
    struct State;
    explicit NaturalNameOrder(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

} // namespace axk::app
