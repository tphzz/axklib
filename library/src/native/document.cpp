#include "axklib/native/document.hpp"

#include <algorithm>
#include <utility>

namespace axk::native {
namespace {

template <typename T>
Result<Page<T>> page(const std::vector<T> &items, std::size_t offset, std::size_t limit,
                     const CancellationToken &cancellation) {
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(active.error());
    if (limit == 0U || limit > 1024U)
        return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::object,
                                          "Native catalog page size must be between one and 1024"));
    Page<T> result{{}, items.size()};
    const auto begin = std::min(offset, items.size());
    const auto count = std::min(limit, items.size() - begin);
    result.items.reserve(count);
    for (std::size_t index = 0U; index < count; ++index)
        result.items.push_back(items[begin + index]);
    return result;
}

} // namespace

Result<Page<ObjectSummary>> Document::objects(std::size_t offset, std::size_t limit,
                                              const CancellationToken &cancellation) const {
    return page(catalog().objects, offset, limit, cancellation);
}

Result<Page<Relationship>> Document::relationships(std::size_t offset, std::size_t limit,
                                                   const CancellationToken &cancellation) const {
    return page(catalog().relationships, offset, limit, cancellation);
}

} // namespace axk::native
