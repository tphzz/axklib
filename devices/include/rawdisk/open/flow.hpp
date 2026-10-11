// Open flows: a descriptor-driven way to get from "nothing selected" to an opened BlockDevice in
// one or more steps (select a device, pick a file, choose a virtual disk, ...).
//
// A Mode (image, raw device, SCSI2SD V6, ...) is a pipeline of Blocks. Each Block describes the
// step it needs as plain data (Step/Field/Option/Notice), so a UI can render any mode without
// knowing it, and code can answer the same steps directly (run(), locators). All checks live in
// the typed API underneath (open_device, open_image, scsi2sd::v6, open_volume); flows only
// orchestrate it.
//
// Discovery is always read-only. When every step is answered the flow is ready(): preview()
// describes what open() will produce. open() with Access::ReadWrite replays all recorded answers
// on fresh handles and requires the same identity and geometry as during discovery.
#pragma once

#include "rawdisk/block_device.hpp"
#include "rawdisk/cancellation.hpp"
#include "rawdisk/volume.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rawdisk::open {

// ---- step descriptors (what a UI renders) ------------------------------------------------------

enum class FieldKind {
    choice,  // pick one Option by id
    file,    // a filesystem path (UTF-8)
    confirm, // "yes" / "no"
};
std::string_view to_string(FieldKind k) noexcept;

struct Option {
    std::string id{}; // stable; the answer value (never a list index)
    std::string label{};
    std::string detail{};
    bool enabled = true;
    std::string disabled_reason{}; // why it can't be chosen (for this access mode)
    std::vector<Notice> notices{};
};

struct FileFilter {
    std::string name{};    // "SCSI2SD XML"
    std::string pattern{}; // "*.xml"
};

struct Field {
    std::string key{}; // answer key within the step ("device", "path", "volume")
    std::string label{};
    std::string help{};
    FieldKind kind = FieldKind::choice;
    bool required = true;
    std::vector<Option> options{};     // choice
    std::vector<FileFilter> filters{}; // file
    std::optional<std::string> default_value{};
};

struct Step {
    std::string id{}; // the block id; also the locator namespace ("device", "volume")
    std::string title{};
    std::string help{};
    std::vector<Field> fields{};
    std::vector<Notice> notices{};
    bool may_prompt_for_privileges = false; // submitting may show a UAC / password prompt
    bool refreshable = false;               // refresh() re-reads the options (hot-plug)

    const Field *field(std::string_view key) const noexcept;
};

using Answers = std::map<std::string, std::string, std::less<>>;

// A failure pointing at the step and field it belongs to, so a UI can mark the field or jump back.
struct SubmitError {
    std::error_code code;
    std::string step;
    std::string field;
    std::string message;
};
template <class T> using Outcome = std::expected<T, SubmitError>;

// ---- extension points --------------------------------------------------------------------------

// Where devices come from. The default uses list_usb_devices() / open_device(); tests and special
// setups inject their own.
class DeviceSource {
  public:
    virtual ~DeviceSource() = default;
    virtual Result<std::vector<DeviceInfo>> list() = 0;
    virtual Result<std::shared_ptr<BlockDevice>> open(const DeviceInfo &info, Access access) = 0;
    // Held by the flow while it closes its read-only discovery handles and reopens for writing,
    // so privileged access survives the gap (the system source: hold_privileges(), i.e. no second
    // UAC prompt on Windows). Default: nothing to hold.
    virtual std::shared_ptr<void> hold() { return nullptr; }
};
std::shared_ptr<DeviceSource> system_device_source();

struct Context {
    std::shared_ptr<DeviceSource> devices; // null: system_device_source()
};

// What the pipeline has produced so far; the output of one block is the input of the next.
struct Stage {
    std::shared_ptr<BlockDevice> device; // root device, or the selected volume
    std::optional<DeviceInfo> usb;       // when the root is a USB device
    std::optional<VolumeSet> volumes;    // after a layout scheme, until a volume is selected
    std::string description;
    std::vector<Notice> notices;    // accumulated findings
    std::uint64_t offset_bytes = 0; // selected target offset within the root
};

struct BlockEnv {
    const Context &context;
    Access access;   // the access the flow will finally open with
    bool final_pass; // false: read-only discovery; true: the replay that opens for real
    CancellationToken stop;
};

// One building block of a pipeline. A Block instance belongs to one flow and may keep state
// between prepare() and apply(). A step without fields is applied automatically.
class Block {
  public:
    virtual ~Block() = default;
    virtual std::string_view id() const noexcept = 0;
    // Called when the block becomes current and on refresh(); may do I/O.
    virtual Outcome<Step> prepare(const BlockEnv &env, const Stage &input) = 0;
    // Answers are already validated against the prepared step (kinds, options, required, defaults).
    virtual Outcome<Stage> apply(const BlockEnv &env, const Stage &input, const Answers &answers) = 0;
};

struct ModeInfo {
    std::string id;   // stable, used in locators: "scsi2sd-v6"
    std::string name; // "SCSI2SD V6 card"
    std::string description;
};

class Mode {
  public:
    virtual ~Mode() = default;
    virtual const ModeInfo &info() const noexcept = 0;
    virtual std::vector<std::unique_ptr<Block>> pipeline() const = 0; // fresh blocks per call
};
std::shared_ptr<const Mode> make_mode(ModeInfo info, std::function<std::vector<std::unique_ptr<Block>>()> pipeline);

// ---- running a flow ----------------------------------------------------------------------------

struct Preview {
    std::string description;
    std::string locator;
    Access access = Access::ReadOnly;
    std::uint32_t sector_size = 0;
    std::uint64_t sector_count = 0;
    std::vector<Notice> notices;
    std::optional<DeviceInfo> usb{};
    std::uint64_t offset_bytes = 0;
};

struct Opened {
    std::shared_ptr<BlockDevice> device;
    std::string description;
    std::string locator; // replay with rawdisk::open::open()
    std::vector<Notice> notices;
    std::optional<DeviceInfo> usb{};
    std::uint64_t offset_bytes = 0;
};

// Single-threaded object: call it from one thread at a time (a worker thread is fine). Every
// call that may do I/O takes a CancellationToken.
class Flow {
  public:
    static Outcome<std::unique_ptr<Flow>> start(std::shared_ptr<const Mode> mode, Access access, Context context = {},
                                                CancellationToken stop = {});
    ~Flow();
    Flow(const Flow &) = delete;
    Flow &operator=(const Flow &) = delete;

    const ModeInfo &mode() const noexcept;
    Access access() const noexcept;

    bool ready() const noexcept;              // every step answered
    const Step &step() const noexcept;        // the current step (only while !ready())
    std::size_t step_number() const noexcept; // 1-based position among all blocks
    std::size_t step_total() const noexcept;

    Outcome<void> submit(const Answers &answers, CancellationToken stop = {});
    bool can_go_back() const noexcept;
    Outcome<void> back(CancellationToken stop = {}); // to the previous step with fields
    Outcome<void> refresh(CancellationToken stop = {});

    const Preview &preview() const noexcept; // only when ready()
    // Borrows the discovery handle for read-only preflight. Release it before open(ReadWrite).
    Outcome<Opened> read_only() const;
    Outcome<Opened> open(CancellationToken stop = {});

    Answers answers() const; // flat "step.field" -> value, answered so far
    std::string locator() const;

  private:
    struct Impl;
    explicit Flow(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Answers every step from `answers` (flat "step.field" keys) and opens. Missing, unknown or
// disallowed answers fail with the step and field they concern.
Outcome<Opened> run(std::shared_ptr<const Mode> mode, const Answers &answers, Access access, Context context = {},
                    CancellationToken stop = {});

} // namespace rawdisk::open
