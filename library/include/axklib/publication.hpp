#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "axklib/export.hpp"

namespace axk {

enum class PublicationDurability : std::uint8_t { confirmed, unconfirmed };

struct PublicationWarning {
    std::string code;
    std::string message;

    friend bool operator==(const PublicationWarning &, const PublicationWarning &) = default;
};

struct PublicationOutcome {
    PublicationDurability durability{PublicationDurability::confirmed};
    std::vector<PublicationWarning> warnings;

    friend bool operator==(const PublicationOutcome &, const PublicationOutcome &) = default;
};

} // namespace axk
