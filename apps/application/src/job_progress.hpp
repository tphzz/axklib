#pragma once

#include <chrono>
#include <optional>

#include "axklib/application/jobs.hpp"

namespace axk::app::job_runtime_detail {

class ProgressPublication {
  public:
    bool admit(const JobProgress &update, JobManager::Clock::time_point now) {
        if (published_) {
            const bool same_stage = published_->phase == update.phase && published_->message == update.message &&
                                    published_->total == update.total;
            if (same_stage && published_->completed == update.completed)
                return false;
            const bool complete = update.total && update.completed >= *update.total;
            if (same_stage && !complete && now - published_at_ < std::chrono::milliseconds{50})
                return false;
        }
        published_ = update;
        published_at_ = now;
        return true;
    }

  private:
    std::optional<JobProgress> published_;
    JobManager::Clock::time_point published_at_{};
};

} // namespace axk::app::job_runtime_detail
