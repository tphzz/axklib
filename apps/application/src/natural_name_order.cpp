#include "axklib/application/natural_name_order.hpp"

#include <limits>
#include <utility>

#include <unicode/ucol.h>
#include <unicode/ustring.h>

namespace axk::app {

struct NaturalNameOrder::State {
    UCollator *collator{};
    ~State() {
        if (collator != nullptr)
            ucol_close(collator);
    }
};

NaturalNameOrder::NaturalNameOrder(std::unique_ptr<State> state) : state_(std::move(state)) {}
NaturalNameOrder::NaturalNameOrder(NaturalNameOrder &&) noexcept = default;
NaturalNameOrder &NaturalNameOrder::operator=(NaturalNameOrder &&) noexcept = default;
NaturalNameOrder::~NaturalNameOrder() = default;

Result<NaturalNameOrder> NaturalNameOrder::create() {
    auto state = std::make_unique<State>();
    UErrorCode status = U_ZERO_ERROR;
    state->collator = ucol_open("en", &status);
    if (U_FAILURE(status) || state->collator == nullptr)
        return std::unexpected(Error{"directory_sort_failed", "Unicode name ordering could not be initialized"});
    ucol_setAttribute(state->collator, UCOL_STRENGTH, UCOL_PRIMARY, &status);
    ucol_setAttribute(state->collator, UCOL_CASE_LEVEL, UCOL_OFF, &status);
    ucol_setAttribute(state->collator, UCOL_NUMERIC_COLLATION, UCOL_ON, &status);
    ucol_setAttribute(state->collator, UCOL_NORMALIZATION_MODE, UCOL_ON, &status);
    ucol_setAttribute(state->collator, UCOL_ALTERNATE_HANDLING, UCOL_NON_IGNORABLE, &status);
    if (U_FAILURE(status))
        return std::unexpected(Error{"directory_sort_failed", "Unicode name ordering could not be configured"});
    return NaturalNameOrder{std::move(state)};
}

Result<NaturalNameKey> NaturalNameOrder::key(std::string_view name) const {
    if (name.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        return std::unexpected(Error{"directory_sort_failed", "Directory name exceeds Unicode ordering limits"});
    // UTF-16 never needs more code units than the source's UTF-8 bytes.
    std::vector<UChar> text(name.size() + 1U);
    std::int32_t length{};
    UErrorCode status = U_ZERO_ERROR;
    u_strFromUTF8(text.data(), static_cast<std::int32_t>(text.size()), &length, name.data(),
                  static_cast<std::int32_t>(name.size()), &status);
    if (U_FAILURE(status))
        return std::unexpected(Error{"directory_sort_failed", "Directory name is not valid UTF-8"});
    const auto required = ucol_getSortKey(state_->collator, text.data(), length, nullptr, 0);
    if (required <= 0)
        return std::unexpected(Error{"directory_sort_failed", "Unicode name ordering key could not be generated"});
    NaturalNameKey result{.collation = std::vector<std::uint8_t>(static_cast<std::size_t>(required)),
                          .original = std::string{name}};
    if (ucol_getSortKey(state_->collator, text.data(), length, result.collation.data(), required) != required)
        return std::unexpected(Error{"directory_sort_failed", "Unicode name ordering key size changed"});
    return result;
}

} // namespace axk::app
