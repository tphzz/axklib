#pragma once

#include "axklib/application/content_id.hpp"

namespace axk::cli::detail {

using axk::app::ContentId;
using axk::app::PooledPathAllocator;
using axk::app::sha1_content_id;
using axk::app::sha1_wav_content_id;
using axk::app::WavContentIdProvider;

} // namespace axk::cli::detail
