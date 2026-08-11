#include "archive_download_budget.hpp"

#include <mutex>

struct axk::server::ArchiveDownloadBudget::State {
    explicit State(std::size_t limit) : maximum_active(limit) {}

    std::size_t maximum_active{};
    std::size_t active{};
    mutable std::mutex mutex;
};

axk::server::ArchiveDownloadBudget::Lease::~Lease() {
    if (!state_)
        return;
    const std::scoped_lock lock{state_->mutex};
    --state_->active;
}

axk::server::ArchiveDownloadBudget::ArchiveDownloadBudget(std::size_t maximum_active)
    : state_(std::make_shared<State>(maximum_active)) {}

std::optional<axk::server::ArchiveDownloadBudget::Lease> axk::server::ArchiveDownloadBudget::try_acquire() {
    const std::scoped_lock lock{state_->mutex};
    if (state_->active >= state_->maximum_active)
        return std::nullopt;
    ++state_->active;
    return Lease{state_};
}

std::size_t axk::server::ArchiveDownloadBudget::active() const {
    const std::scoped_lock lock{state_->mutex};
    return state_->active;
}
