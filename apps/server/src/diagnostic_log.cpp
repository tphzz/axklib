#include "diagnostic_log.hpp"

#include "environment.hpp"
#include <algorithm>
#include <iostream>
#include <optional>
#include <utility>

namespace axk::server::detail {
namespace {
std::optional<diagnostic_level> declared_level(const nlohmann::json &event) {
    if (!event.is_object() || !event.contains("level") || !event.at("level").is_string())
        return std::nullopt;
    const auto &value = event.at("level").get_ref<const std::string &>();
    if (value == "error")
        return diagnostic_level::error;
    if (value == "warning")
        return diagnostic_level::warning;
    if (value == "info")
        return diagnostic_level::info;
    if (value == "debug")
        return diagnostic_level::debug;
    if (value == "trace")
        return diagnostic_level::trace;
    return std::nullopt;
}
nlohmann::json invalid_record(std::string_view original, std::string_view reason) {
    auto end = std::min(original.size(), std::size_t{4096});
    while (end < original.size() && end != 0U && (static_cast<unsigned char>(original[end]) & 0xc0U) == 0x80U)
        --end;
    return {{"event", "invalid_log_record"},
            {"source", "other"},
            {"level", "error"},
            {"message", "Logging contract violation; original severity is not inferred"},
            {"reason", reason},
            {"originalText", original.substr(0, end)},
            {"truncated", end < original.size()}};
}
std::uint8_t severity(std::string_view value) noexcept {
    if (value == "trace")
        return 5;
    if (value == "debug")
        return 4;
    if (value == "warning" || value == "warn")
        return 2;
    if (value == "error")
        return 1;
    if (value == "off")
        return 0;
    return 3;
}
std::int64_t ticks(std::chrono::steady_clock::time_point value) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}
} // namespace
void write_startup_error(std::string_view code, std::string_view message) noexcept {
    try {
        std::cerr << nlohmann::json{{"event", "server_start_failed"},
                                    {"source", "application"},
                                    {"level", "error"},
                                    {"code", code},
                                    {"message", message}}
                         .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)
                  << '\n';
    } catch (...) {
        try {
            std::cerr << "{\"event\":\"server_start_failed\",\"source\":\"application\",\"level\":\"error\","
                         "\"message\":\"unable to serialize startup failure\"}\n";
        } catch (...) {
        }
    }
}
DiagnosticLog::DiagnosticLog(bool sidecar, Clock clock, Writer writer)
    : sidecar_(sidecar), console_level_(severity(environment_variable("AXKLIB_SERVER_LOG_LEVEL").value_or("info"))),
      clock_(std::move(clock)),
      writer_(writer ? std::move(writer) : Writer{[](const std::string &line) { std::clog << line << '\n'; }}),
      worker_([this] { drain(); }) {}
DiagnosticLog::~DiagnosticLog() {
    {
        const std::scoped_lock lock{mutex_};
        stopping_ = true;
    }
    ready_.notify_one();
    worker_.join();
}
diagnostic_level DiagnosticLog::capture() const noexcept {
    try {
        if (ticks(clock_()) >= expires_.load(std::memory_order_acquire))
            return diagnostic_level::info;
        return static_cast<diagnostic_level>(capture_level_.load(std::memory_order_relaxed));
    } catch (...) {
        return diagnostic_level::info;
    }
}
void DiagnosticLog::set_capture(diagnostic_level level) noexcept {
    try {
        expires_.store(ticks(clock_()) + 30000, std::memory_order_release);
        capture_level_.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
    } catch (...) {
    }
}
bool DiagnosticLog::enabled(diagnostic_level level) const noexcept {
    const auto limit = sidecar_ ? std::max(console_level_, static_cast<std::uint8_t>(capture())) : console_level_;
    return static_cast<std::uint8_t>(level) <= limit;
}
void DiagnosticLog::submit(nlohmann::json event) noexcept {
    try {
        auto level = declared_level(event);
        if (!level) {
            event = invalid_record(event.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace),
                                   "missing or invalid declared level");
            level = diagnostic_level::error;
        }
        if (!enabled(*level))
            return;
        event["timestamp"] =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        auto line = event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        const std::scoped_lock lock{mutex_};
        if (line.size() > 65536U || queue_.size() >= 1024U || queued_bytes_ + line.size() > 4U * 1024U * 1024U) {
            ++dropped_;
            ready_.notify_one();
            return;
        }
        queued_bytes_ += line.size();
        queue_.push_back(std::move(line));
        ready_.notify_one();
    } catch (...) {
    }
}
void DiagnosticLog::submit_line(const std::string &line) noexcept {
    try {
        auto event = nlohmann::json::parse(line, nullptr, false);
        if (event.is_discarded())
            submit(invalid_record(line, "unrecognized log format"));
        else
            submit(std::move(event));
    } catch (...) {
    }
}
void DiagnosticLog::drain() noexcept {
    for (;;) {
        std::string line;
        std::uint64_t dropped{};
        {
            std::unique_lock lock{mutex_};
            ready_.wait(lock, [this] { return stopping_ || !queue_.empty() || dropped_ != 0U; });
            if (queue_.empty() && dropped_ == 0U && stopping_)
                return;
            dropped = std::exchange(dropped_, 0U);
            if (!queue_.empty()) {
                line = std::move(queue_.front());
                queue_.pop_front();
                queued_bytes_ -= line.size();
            }
        }
        try {
            if (dropped != 0U)
                writer_(nlohmann::json{{"event", "diagnostic_events_dropped"},
                                       {"source", "jobs"},
                                       {"level", "warning"},
                                       {"count", dropped}}
                            .dump());
            if (!line.empty())
                writer_(line);
        } catch (...) {
        }
    }
}
} // namespace axk::server::detail
