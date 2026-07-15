#include "axklib/version.hpp"

#include "axklib/sdk/version.hpp"

namespace axk {

std::string_view version() noexcept { return version_string; }

} // namespace axk
