#include "audio_import_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace axk::audio_import_detail {
namespace {

// PCG-XSH-RR 64/32 transition and output permutation. The algorithm source and
// axklib-owned seed/stream policy are recorded in docs/native-dependencies.md.
constexpr std::uint64_t pcg32_multiplier = 6'364'136'223'846'793'005ULL;
constexpr std::uint64_t dither_seed = 0x0041'584bULL;
constexpr std::uint64_t dither_first_stream = 0x4158'4b01ULL;
constexpr std::uint64_t dither_second_stream = 0x4158'4b02ULL;
constexpr double uint32_range = 4'294'967'296.0;

class Pcg32 final {
 public:
  constexpr Pcg32(std::uint64_t seed, std::uint64_t stream)
      : increment_{(stream << 1U) | 1U} {
    static_cast<void>(next());
    state_ += seed;
    static_cast<void>(next());
  }

  constexpr std::uint32_t next() {
    const auto previous = state_;
    state_ = previous * pcg32_multiplier + increment_;
    const auto shifted = static_cast<std::uint32_t>(((previous >> 18U) ^ previous) >> 27U);
    const auto rotation = static_cast<std::uint32_t>(previous >> 59U);
    return static_cast<std::uint32_t>((shifted >> rotation) |
                                      (shifted << ((0U - rotation) & 31U)));
  }

  constexpr double unit_double() { return static_cast<double>(next()) / uint32_range; }

 private:
  std::uint64_t state_{};
  std::uint64_t increment_{};
};

static_assert([] {
  Pcg32 random{dither_seed, dither_first_stream};
  return random.next() == 0x60ee'0ca0U && random.next() == 0x11f6'0335U;
}());

}  // namespace

Result<QuantizedPcm16> quantize_pcm16(std::span<const double> samples, bool dither) {
  if (!std::ranges::all_of(samples, [](double value) { return std::isfinite(value); })) {
    return std::unexpected{make_error(ErrorCode::audio_unsupported_format,
                                      ErrorCategory::audio,
                                      "source audio contains NaN or infinite samples")};
  }

  Pcg32 first{dither_seed, dither_first_stream};
  Pcg32 second{dither_seed, dither_second_stream};
  QuantizedPcm16 result;
  result.samples.reserve(samples.size());
  for (const auto sample : samples) {
    auto scaled = sample * 32'768.0;
    if (scaled < -32'768.0 || scaled > 32'767.0)
      ++result.clipped_samples;
    if (dither)
      scaled += first.unit_double() - second.unit_double();
    scaled = std::floor(scaled + 0.5);
    scaled = std::clamp(scaled, -32'768.0, 32'767.0);
    result.samples.push_back(static_cast<std::int16_t>(scaled));
  }
  return result;
}

}  // namespace axk::audio_import_detail
