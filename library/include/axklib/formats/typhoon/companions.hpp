#pragma once

#include "axklib/formats/typhoon/document.hpp"
#include <set>
#include <span>

namespace axk::typhoon {
struct Companions {
    std::vector<std::size_t> candidates;
    std::set<std::string> ambiguous;
};
[[nodiscard]] Result<Companions> find_companions(const Document &seed,
                                                 std::span<const std::shared_ptr<const Document>> neighbors,
                                                 const CancellationToken &token = {});
} // namespace axk::typhoon
