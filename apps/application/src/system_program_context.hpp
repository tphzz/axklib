#pragma once

#include "axklib/application/image_sessions.hpp"
#include "axklib/system_file.hpp"

namespace axk::app::image_sessions_internal {

ImageSystemProgramContext system_program_context(const axk::DecodedSystemFile &decoded);

} // namespace axk::app::image_sessions_internal
