#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "axklib/application/jobs.hpp"

namespace axk::server {

struct EventDispatcherSnapshot {
    std::uint64_t delivered_events{};
    std::uint64_t dropped_events{};
    std::size_t pending_events{};
};

class EventDispatcher {
  public:
    using Sink = std::function<void(const app::JobEvent &)>;

    EventDispatcher(std::size_t maximum_pending_events, Sink sink);
    ~EventDispatcher();

    EventDispatcher(const EventDispatcher &) = delete;
    EventDispatcher &operator=(const EventDispatcher &) = delete;
    EventDispatcher(EventDispatcher &&) = delete;
    EventDispatcher &operator=(EventDispatcher &&) = delete;

    [[nodiscard]] bool publish(app::JobEvent event) noexcept;
    [[nodiscard]] EventDispatcherSnapshot snapshot() const noexcept;
    void shutdown() noexcept;

  private:
    void run() noexcept;

    const std::size_t maximum_pending_events_;
    Sink sink_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<app::JobEvent> pending_;
    std::uint64_t delivered_events_{};
    std::uint64_t dropped_events_{};
    bool stopping_{};
    std::thread worker_;
};

} // namespace axk::server
