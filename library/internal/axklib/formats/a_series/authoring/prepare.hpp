#pragma once

#include <filesystem>
#include <memory>

#include "axklib/alteration_transaction.hpp"
#include "axklib/formats/a_series/authoring/bundle.hpp"

namespace axk::a_series::authoring {

[[nodiscard]] Result<Inventory> inventory(std::shared_ptr<const RandomAccessReader> reader,
                                          const Destination &destination, const CancellationToken &cancellation = {});

[[nodiscard]] Result<detail::PreparedAlteration> prepare(std::shared_ptr<const RandomAccessReader> reader,
                                                         std::filesystem::path path, const Destination &destination,
                                                         const Bundle &bundle, const VolumeCapacityPolicy &policy = {},
                                                         const CancellationToken &cancellation = {},
                                                         ProgressSink *progress = nullptr);

} // namespace axk::a_series::authoring
