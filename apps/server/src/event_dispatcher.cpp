#include "axklib/server/event_dispatcher.hpp"

#include <algorithm>
#include <utility>

axk::server::EventDispatcher::EventDispatcher(std::size_t maximum_pending_events, Sink sink)
    : maximum_pending_events_(std::max<std::size_t>(maximum_pending_events, 1U)), sink_(std::move(sink)),
      worker_([this] { run(); }) {}

axk::server::EventDispatcher::~EventDispatcher() { shutdown(); }

bool axk::server::EventDispatcher::publish(app::JobEvent event) noexcept {
    try {
        {
            const std::scoped_lock lock{mutex_};
            if (stopping_) {
                ++dropped_events_;
                return false;
            }
            if (pending_.size() >= maximum_pending_events_) {
                if (app::is_terminal(event.state)) {
                    const auto progress = std::ranges::find(pending_, std::string{"progress"}, &app::JobEvent::type);
                    if (progress != pending_.end()) {
                        pending_.erase(progress);
                        ++dropped_events_;
                    } else {
                        ++dropped_events_;
                        return false;
                    }
                } else {
                    ++dropped_events_;
                    return false;
                }
            }
            pending_.push_back(std::move(event));
        }
        condition_.notify_one();
        return true;
    } catch (...) {
        const std::scoped_lock lock{mutex_};
        ++dropped_events_;
        return false;
    }
}

axk::server::EventDispatcherSnapshot axk::server::EventDispatcher::snapshot() const noexcept {
    const std::scoped_lock lock{mutex_};
    return {
        .delivered_events = delivered_events_, .dropped_events = dropped_events_, .pending_events = pending_.size()};
}

void axk::server::EventDispatcher::shutdown() noexcept {
    {
        const std::scoped_lock lock{mutex_};
        if (stopping_ && !worker_.joinable())
            return;
        stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void axk::server::EventDispatcher::run() noexcept {
    for (;;) {
        app::JobEvent event;
        {
            std::unique_lock lock{mutex_};
            condition_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) {
                if (stopping_)
                    return;
                continue;
            }
            event = std::move(pending_.front());
            pending_.pop_front();
        }
        try {
            if (sink_)
                sink_(event);
        } catch (...) {
        }
        {
            const std::scoped_lock lock{mutex_};
            ++delivered_events_;
        }
    }
}
