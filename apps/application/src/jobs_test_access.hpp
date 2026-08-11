#pragma once

#include <functional>

namespace axk::app {

class JobManager;

class JobManagerTestAccess {
  public:
    static void set_before_running_claim(JobManager &manager, std::function<void()> hook);
};

} // namespace axk::app
