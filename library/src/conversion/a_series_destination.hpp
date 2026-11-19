#pragma once

#include "axklib/conversion/plan.hpp"
#include "axklib/formats/a_series/authoring/prepare.hpp"
#include "axklib/native_bundle_media.hpp"

namespace axk::conversion {
inline RouteDescriptor existing_sfs_route() {
    return {"A-series · Hard disk (SFS)",
            "sfs-hard-disk",
            {"PLAYABLE_INSTRUMENTS", "WAVE_DATA"},
            {"disk-set-selection", "a-series-volume-placement", "a-series-load-target"},
            0};
}
[[nodiscard]] Result<a_series::authoring::Destination> volume_destination(const nlohmann::json &selection);
struct FreshVolume {
    a_series::authoring::FreshMedia media;
    std::string profile_id;
    std::string extension;
};
using VolumePrepare = std::function<Result<std::shared_ptr<const PreparedPlan>>(
    const Request &, const CancellationToken &, ProgressSink *, const FreshVolume *)>;
[[nodiscard]] std::vector<Profile> fresh_volume_profiles(const Profile &base, const VolumePrepare &prepare);
[[nodiscard]] std::shared_ptr<const PreparedPlan> blocked_volume_plan(const Request &, Report);
[[nodiscard]] Result<std::shared_ptr<const PreparedPlan>>
finish_volume_plan(const Request &, const a_series::authoring::Destination &, a_series::authoring::Bundle, Report,
                   const CancellationToken &, ProgressSink *, const FreshVolume *);
} // namespace axk::conversion
