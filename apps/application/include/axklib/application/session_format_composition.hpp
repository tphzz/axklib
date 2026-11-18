#pragma once

#include "axklib/application/contracts.hpp"

namespace axk::app {

class SessionFormats;
struct SessionFormatContext;
using ConfigureSessionFormats = Result<void>(SessionFormats &, const SessionFormatContext &);
ConfigureSessionFormats configure_builtin_session_formats;

} // namespace axk::app
