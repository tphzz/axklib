#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace axk {

enum class diagnostic_level : std::uint8_t { error = 1, warning = 2, info = 3, debug = 4, trace = 5 };
enum class diagnostic_source : std::uint8_t { filesystem, objects, devices, audio };
struct diagnostic_metric {
    std::string name;
    std::uint64_t value{};
};
struct diagnostic_event {
    diagnostic_level level{diagnostic_level::debug};
    diagnostic_source source{diagnostic_source::objects};
    std::string event;
    std::string phase;
    std::string outcome;
    std::uint64_t duration_us{};
    std::vector<diagnostic_metric> metrics;
};

// The caller retains the sink until all operations return. Callbacks may run
// on operation threads and must not re-enter the same operation context.
class diagnostic_sink {
  public:
    virtual ~diagnostic_sink() = default;
    virtual bool enabled(diagnostic_level level) const noexcept = 0;
    virtual void report(const diagnostic_event &event) = 0;
};
} // namespace axk
