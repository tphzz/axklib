#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "axklib/sdk.hpp"

namespace axk {

struct device_candidate {
    std::string id;
    std::string model;
    std::string serial;
    std::uint64_t size_bytes{};
    std::uint32_t sector_size{};
};
enum class scsi_device_type : std::uint8_t { fixed, removable, optical, floppy, magneto_optical, sequential };
struct device_scsi_target {
    std::uint8_t id{};
    scsi_device_type type{};
    std::string vendor;
    std::string product;
    std::string revision;
    std::string serial;
};
struct device_target {
    std::string id;
    std::string label;
    std::uint64_t size_bytes{};
    std::uint32_t sector_size{};
    bool read_only{true};
    std::vector<std::string> notices;
    std::uint64_t offset_bytes{};
    std::optional<device_scsi_target> scsi{};
};
struct device_session {
    std::string id;
    device_candidate candidate;
    std::vector<device_target> targets;
    bool writes_supported{};
    bool writes_authorized{};
    bool invalidated{};
};
struct device_operation {
    std::string id;
    // no_write, prepared, committed, rolled_back, recovery_required.
    std::string state;
    std::string model;
    std::string serial;
    std::string target_label;
    std::uint64_t target_size{};
};

struct scsi2sd_xml_target {
    std::string id;
    std::string label;
    std::uint64_t size_bytes{};
    std::uint32_t sector_size{};
    bool enabled{};
    bool eligible{};
    bool read_only{true};
    std::vector<std::string> notices;
};
// Pure validation of a utility-exported XML document; never accesses hardware.
// XML dialect does not identify the board version or grant permission to write.
[[nodiscard]] AXK_SDK_API result<std::vector<scsi2sd_xml_target>> inspect_scsi2sd_xml(const std::string &xml);

// Explicit device ownership. Construction and operation-status enumeration do
// not enumerate, open, or recover physical media. Calls on one host are serialized.
class AXK_SDK_API device_host final {
  public:
    device_host();
    ~device_host();
    device_host(device_host &&) noexcept;
    device_host &operator=(device_host &&) noexcept;
    device_host(const device_host &) = delete;
    device_host &operator=(const device_host &) = delete;

    [[nodiscard]] static bool available() noexcept;
    [[nodiscard]] static result<device_host> create(const std::string &state_directory,
                                                    const std::string &helper_path = {});
    [[nodiscard]] result<std::vector<device_candidate>> list();
    // Layout is raw, scsi2sd-v6, or scsi2sd-xml. XML mismatches are read-only.
    [[nodiscard]] result<device_session> connect(const std::string &candidate_id, const std::string &layout,
                                                 const std::string &xml = {});
    [[nodiscard]] result<device_session> status(const std::string &session_id);
    [[nodiscard]] result<image> open_image(const std::string &session_id, const std::string &target_id,
                                           operation_context &context);
    [[nodiscard]] result<void> authorize_writes(const std::string &session_id, const std::string &target_id,
                                                bool exclusive_use_acknowledged);
    // Applies the existing alteration-manifest contract to an opened SFS target.
    // Inputs must be inline; partition edits and repair are intentionally excluded.
    // A caller-generated unique operation ID must be retained before invoking.
    [[nodiscard]] result<device_operation> apply(const std::string &session_id, const std::string &target_id,
                                                 const std::string &operation_id, const std::string &manifest_json,
                                                 operation_context &context);
    [[nodiscard]] result<std::vector<device_operation>> operations();
    [[nodiscard]] result<void> recover(const std::string &operation_id, const std::string &session_id,
                                       const std::string &target_id, bool exclusive_use_acknowledged);
    // Destroy images/snapshots before closing their carrier. Recovery requires
    // an explicit reopen and never resumes an old reader or authorization.
    [[nodiscard]] result<void> close(const std::string &session_id);

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace axk
