#pragma once

#include "remote.hpp"
#include "win32_common.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace rawdisk::detail {

inline constexpr std::wstring_view kPipePrefix = LR"(\\.\pipe\rawdisk-)";
inline constexpr std::size_t kPipeRandomHex = 32;

Result<std::wstring> random_pipe_name();
Result<Handle> create_pipe(const std::wstring &name);
Result<void> await_helper(HANDLE pipe, HANDLE process, DWORD timeout_ms);
Result<std::unique_ptr<Stream>> make_pipe_stream(Handle pipe, DWORD timeout_ms);

} // namespace rawdisk::detail
