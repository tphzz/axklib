#pragma once

#include "rawdisk/open/flow.hpp"

#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace rawtool {
struct Options {
    bool prompt = true;
    bool yes = false;
    std::uint64_t max_device_bytes = 9'000'000'000;
};

template <class... A> void out(std::format_string<A...> fmt, A &&...args) {
    std::fputs(std::format(fmt, std::forward<A>(args)...).c_str(), stdout);
    std::fflush(stdout);
}

int fail(std::string_view what, const std::error_code &ec, std::string_view detail = {});
int fail(const rawdisk::open::SubmitError &e);
std::optional<std::string> read_line(std::string_view prompt);
void print_preview(const rawdisk::open::Preview &p);
std::expected<std::unique_ptr<rawdisk::open::Flow>, int> complete(const std::string &target, rawdisk::Access access,
                                                                  const Options &opt, bool inspect = false);
int cmd_probe_write(const std::string &target, const std::filesystem::path &plan, const std::filesystem::path &journal,
                    const Options &opt);
int cmd_recover_write(const std::filesystem::path &journal, const Options &opt);
} // namespace rawtool
