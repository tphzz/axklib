#include "detail.hpp"
#include "rawdisk/open/flow.hpp"
#include "rawdisk/open/locator.hpp"
#include "rawdisk/ownership.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <tuple>

namespace rawdisk::open {
namespace {

SubmitError error(std::error_code ec, std::string step, std::string field = {}, std::string message = {}) {
    if (message.empty())
        message = ec.message();
    return {ec, std::move(step), std::move(field), std::move(message)};
}

std::unexpected<SubmitError> fail(std::error_code ec, std::string step, std::string field = {},
                                  std::string message = {}) {
    return std::unexpected(error(ec, std::move(step), std::move(field), std::move(message)));
}

// What must not change between discovery and the final open.
struct Shape {
    std::uint32_t sector_size = 0;
    std::uint64_t sector_count = 0;
    std::optional<std::tuple<std::string, std::string, std::uint64_t, std::uint32_t>> usb; // not the path
    std::optional<std::vector<std::tuple<std::string, Range, std::uint32_t, std::uint64_t, bool>>> volumes;
    friend bool operator==(const Shape &, const Shape &) = default;
};

Shape shape_of(const Stage &s) {
    Shape sh;
    if (s.device) {
        sh.sector_size = s.device->sector_size();
        sh.sector_count = s.device->sector_count();
    }
    if (s.usb)
        sh.usb = std::tuple(s.usb->model, s.usb->serial, s.usb->size_bytes, s.usb->sector_size);
    if (s.volumes) {
        sh.volumes.emplace();
        for (const Volume &v : s.volumes->volumes)
            sh.volumes->emplace_back(v.id, v.range, v.sector_size, v.sector_count, v.usable);
    }
    return sh;
}

// Checks answers against a prepared step: unknown keys, required fields, option ids, confirm
// values. Returns the effective answers (defaults filled in).
Outcome<Answers> validate(const Step &step, const Answers &answers) {
    for (const auto &[key, value] : answers)
        if (!step.field(key))
            return fail(make_error_code(Errc::flow_invalid_answer), step.id, key,
                        std::format("step '{}' has no field '{}'", step.id, key));
    Answers out;
    for (const Field &f : step.fields) {
        std::optional<std::string> value;
        if (auto it = answers.find(f.key); it != answers.end())
            value = it->second;
        else if (f.default_value)
            value = f.default_value;
        if (!value) {
            if (f.required)
                return fail(make_error_code(Errc::flow_missing_answer), step.id, f.key,
                            std::format("'{}' is required", f.label.empty() ? f.key : f.label));
            continue;
        }
        if (value->find('\0') != std::string::npos)
            return fail(make_error_code(Errc::flow_invalid_answer), step.id, f.key, "contains a NUL character");
        switch (f.kind) {
        case FieldKind::choice: {
            const auto opt = std::ranges::find(f.options, *value, &Option::id);
            if (opt == f.options.end())
                return fail(make_error_code(Errc::flow_stale_selection), step.id, f.key,
                            std::format("'{}' is not (or no longer) available", *value));
            if (!opt->enabled)
                return fail(make_error_code(Errc::flow_invalid_answer), step.id, f.key,
                            opt->disabled_reason.empty() ? std::string("this option is disabled")
                                                         : opt->disabled_reason);
            break;
        }
        case FieldKind::confirm:
            if (*value != "yes" && *value != "no")
                return fail(make_error_code(Errc::flow_invalid_answer), step.id, f.key, "must be 'yes' or 'no'");
            break;
        case FieldKind::file:
            if (value->empty())
                return fail(make_error_code(Errc::flow_missing_answer), step.id, f.key, "no file given");
            break;
        }
        out[f.key] = *value;
    }
    return out;
}

class SystemDevices final : public DeviceSource {
  public:
    Result<std::vector<DeviceInfo>> list() override { return list_usb_devices(); }
    Result<std::shared_ptr<BlockDevice>> open(const DeviceInfo &info, Access access) override {
        auto d = open_owned_device(info, access);
        if (!d)
            return std::unexpected(d.error());
        return std::move(*d);
    }
    std::shared_ptr<void> hold() override { return hold_privileges(); }
};

class LambdaMode final : public Mode {
  public:
    LambdaMode(ModeInfo info, std::function<std::vector<std::unique_ptr<Block>>()> f)
        : info_(std::move(info)), make_(std::move(f)) {}
    const ModeInfo &info() const noexcept override { return info_; }
    std::vector<std::unique_ptr<Block>> pipeline() const override { return make_(); }

  private:
    ModeInfo info_;
    std::function<std::vector<std::unique_ptr<Block>>()> make_;
};

std::error_code canceled() { return std::make_error_code(std::errc::operation_canceled); }

} // namespace

std::string_view to_string(FieldKind k) noexcept {
    switch (k) {
    case FieldKind::choice:
        return "choice";
    case FieldKind::file:
        return "file";
    case FieldKind::confirm:
        return "confirm";
    }
    return "unknown";
}

const Field *Step::field(std::string_view key) const noexcept {
    for (const Field &f : fields)
        if (f.key == key)
            return &f;
    return nullptr;
}

std::shared_ptr<DeviceSource> system_device_source() { return std::make_shared<SystemDevices>(); }

std::shared_ptr<const Mode> make_mode(ModeInfo info, std::function<std::vector<std::unique_ptr<Block>>()> pipeline) {
    return std::make_shared<LambdaMode>(std::move(info), std::move(pipeline));
}

// ---- engine ------------------------------------------------------------------------------------

struct Flow::Impl {
    std::shared_ptr<const Mode> mode;
    Access access = Access::ReadOnly;
    Context ctx;
    std::vector<std::unique_ptr<Block>> blocks;

    // Discovery state. stages[i] is the input of block i; for every completed block i there is
    // answers[i], interactive[i] (it had fields) and shapes[i] (shape of its output).
    std::vector<Stage> stages{Stage{}};
    std::vector<Answers> answers;
    std::vector<bool> interactive;
    std::vector<Shape> shapes;
    std::size_t cur = 0;
    Step step;
    bool ready = false;
    bool released = false; // discovery handles were given up for the final open
    Preview preview;

    BlockEnv env(bool final_pass, CancellationToken stop) const { return {ctx, access, final_pass, std::move(stop)}; }
    std::string block_id(std::size_t i) const {
        return i < blocks.size() ? std::string(blocks[i]->id()) : std::string();
    }

    void pop_to(std::size_t i) {
        stages.resize(i + 1);
        answers.resize(i);
        interactive.resize(i);
        shapes.resize(i);
        cur = i;
        ready = false;
    }

    void commit(Answers a, bool had_fields, Stage out) {
        answers.push_back(std::move(a));
        interactive.push_back(had_fields);
        shapes.push_back(shape_of(out));
        stages.push_back(std::move(out));
        ++cur;
    }

    Answers flat_answers() const {
        Answers flat;
        for (std::size_t i = 0; i < answers.size(); ++i)
            for (const auto &[k, v] : answers[i])
                flat[std::format("{}.{}", blocks[i]->id(), k)] = v;
        return flat;
    }

    void make_preview() {
        const Stage &s = stages.back();
        preview = {};
        preview.description = s.description;
        preview.locator = format_locator({mode->info().id, flat_answers()});
        preview.access = access;
        if (s.device) {
            preview.sector_size = s.device->sector_size();
            preview.sector_count = s.device->sector_count();
        }
        preview.notices = s.notices;
        preview.usb = s.usb;
        preview.offset_bytes = s.offset_bytes;
    }

    // Prepares the current block; applies blocks without fields right away.
    Outcome<void> advance(const CancellationToken &stop) {
        for (;;) {
            if (stop.is_cancelled())
                return fail(canceled(), block_id(cur));
            if (cur == blocks.size()) {
                ready = true;
                make_preview();
                return {};
            }
            auto prepared = blocks[cur]->prepare(env(false, stop), stages[cur]);
            if (!prepared) {
                if (prepared.error().step.empty())
                    prepared.error().step = block_id(cur);
                return std::unexpected(prepared.error());
            }
            step = std::move(*prepared);
            step.id = block_id(cur);
            if (!step.fields.empty())
                return {};
            auto out = blocks[cur]->apply(env(false, stop), stages[cur], {});
            if (!out) {
                if (out.error().step.empty())
                    out.error().step = block_id(cur);
                return std::unexpected(out.error());
            }
            commit({}, false, std::move(*out));
        }
    }

    // Rebuilds the read-only discovery state by replaying recorded answers for blocks < `upto`.
    // Stops at the first block that no longer accepts its answers and leaves the flow there.
    Outcome<void> rediscover(std::size_t upto, const std::vector<Answers> &recorded, const CancellationToken &stop) {
        blocks = mode->pipeline();
        pop_to(0);
        released = false;
        for (;;) {
            if (auto r = advance(stop); !r)
                return r;
            if (ready || cur >= upto || cur >= recorded.size())
                return {};
            auto v = validate(step, recorded[cur]);
            if (!v)
                return {}; // stays at this step for a new answer
            auto out = blocks[cur]->apply(env(false, stop), stages[cur], *v);
            if (!out)
                return {};
            commit(std::move(*v), true, std::move(*out));
        }
    }
};

Flow::Flow(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Flow::~Flow() = default;

Outcome<std::unique_ptr<Flow>> Flow::start(std::shared_ptr<const Mode> mode, Access access, Context context,
                                           CancellationToken stop) {
    try {
        if (!mode)
            return fail(make_error_code(Errc::invalid_argument), "", "", "no mode");
        auto impl = std::make_unique<Impl>();
        impl->mode = std::move(mode);
        impl->access = access;
        impl->ctx = std::move(context);
        if (!impl->ctx.devices)
            impl->ctx.devices = system_device_source();
        impl->blocks = impl->mode->pipeline();
        if (impl->blocks.empty())
            return fail(make_error_code(Errc::invalid_argument), "", "", "empty pipeline");
        std::set<std::string_view> ids;
        for (const auto &b : impl->blocks)
            if (!b || b->id().empty() || !ids.insert(b->id()).second)
                return fail(make_error_code(Errc::invalid_argument), "", "", "block ids must be unique and non-empty");
        if (auto r = impl->advance(stop); !r)
            return std::unexpected(r.error());
        return std::unique_ptr<Flow>(new Flow(std::move(impl)));
    } catch (const std::bad_alloc &) {
        return fail(std::make_error_code(std::errc::not_enough_memory), "");
    } catch (...) {
        return fail(make_error_code(Errc::internal_error), "");
    }
}

const ModeInfo &Flow::mode() const noexcept { return impl_->mode->info(); }
Access Flow::access() const noexcept { return impl_->access; }
bool Flow::ready() const noexcept { return impl_->ready; }
const Step &Flow::step() const noexcept { return impl_->step; }
std::size_t Flow::step_number() const noexcept { return std::min(impl_->cur + 1, impl_->blocks.size()); }
std::size_t Flow::step_total() const noexcept { return impl_->blocks.size(); }
const Preview &Flow::preview() const noexcept { return impl_->preview; }
Answers Flow::answers() const { return impl_->flat_answers(); }
std::string Flow::locator() const { return format_locator({impl_->mode->info().id, impl_->flat_answers()}); }

bool Flow::can_go_back() const noexcept {
    const std::size_t n = std::min(impl_->cur, impl_->interactive.size());
    for (std::size_t i = 0; i < n; ++i)
        if (impl_->interactive[i])
            return true;
    return false;
}

Outcome<void> Flow::submit(const Answers &given, CancellationToken stop) {
    Impl &m = *impl_;
    try {
        if (m.ready)
            return fail(make_error_code(Errc::flow_invalid_answer), "", "", "the flow is complete");
        if (stop.is_cancelled())
            return fail(canceled(), m.step.id);
        auto v = validate(m.step, given);
        if (!v)
            return std::unexpected(v.error());
        const std::size_t at = m.cur;
        auto out = m.blocks[at]->apply(m.env(false, stop), m.stages[at], *v);
        if (!out) {
            if (out.error().step.empty())
                out.error().step = m.step.id;
            return std::unexpected(out.error());
        }
        m.commit(std::move(*v), true, std::move(*out));
        if (auto r = m.advance(stop); !r) {
            // A following automatic step failed: return to the step just answered.
            m.pop_to(at);
            if (auto again = m.blocks[at]->prepare(m.env(false, stop), m.stages[at])) {
                m.step = std::move(*again);
                m.step.id = m.block_id(at);
            }
            return r;
        }
        return {};
    } catch (const std::bad_alloc &) {
        return fail(std::make_error_code(std::errc::not_enough_memory), m.step.id);
    } catch (...) {
        return fail(make_error_code(Errc::internal_error), m.step.id);
    }
}

Outcome<void> Flow::back(CancellationToken stop) {
    Impl &m = *impl_;
    try {
        if (m.released) {
            const auto recorded = m.answers;
            if (auto r = m.rediscover(m.blocks.size(), recorded, stop); !r)
                return r;
        }
        std::optional<std::size_t> target;
        for (std::size_t i = std::min(m.cur, m.interactive.size()); i-- > 0;)
            if (m.interactive[i]) {
                target = i;
                break;
            }
        if (!target)
            return fail(make_error_code(Errc::flow_invalid_answer), m.step.id, "", "no previous step");
        m.pop_to(*target);
        return m.advance(stop);
    } catch (...) {
        return fail(make_error_code(Errc::internal_error), m.step.id);
    }
}

Outcome<void> Flow::refresh(CancellationToken stop) {
    Impl &m = *impl_;
    try {
        if (m.ready)
            return {};
        return m.advance(stop); // re-prepares the current block
    } catch (...) {
        return fail(make_error_code(Errc::internal_error), m.step.id);
    }
}

Outcome<Opened> Flow::read_only() const {
    const Impl &m = *impl_;
    if (!m.ready || m.released)
        return fail(make_error_code(Errc::flow_missing_answer), "");
    const Stage &s = m.stages.back();
    return Opened{s.device, s.description, m.preview.locator, s.notices, s.usb, s.offset_bytes};
}

Outcome<Opened> Flow::open(CancellationToken stop) {
    Impl &m = *impl_;
    try {
        if (m.released) {
            const auto recorded = m.answers;
            if (auto r = m.rediscover(m.blocks.size(), recorded, stop); !r)
                return std::unexpected(r.error());
        }
        if (!m.ready)
            return fail(make_error_code(Errc::flow_missing_answer), m.step.id, "", "not every step is answered");

        if (m.access == Access::ReadOnly) { // discovery already opened everything read-only
            const Stage &s = m.stages.back();
            return Opened{s.device, s.description, m.preview.locator, s.notices, s.usb, s.offset_bytes};
        }

        // ReadWrite: give up every discovery handle (an image's shared lock would block the
        // exclusive one), then replay all answers on fresh handles in the final pass.
        const auto recorded = m.answers;
        const auto shapes = m.shapes;
        const std::string locator = m.preview.locator;
        const auto privileges = m.ctx.devices->hold(); // keeps e.g. the UAC helper alive across the gap
        m.stages.resize(1);
        m.released = true;

        auto fail_at = [&](std::size_t i, SubmitError e) -> Outcome<Opened> {
            if (e.step.empty())
                e.step = m.block_id(i);
            (void)m.rediscover(i, recorded, stop); // leave the flow where the user must act
            return std::unexpected(std::move(e));
        };

        auto pipeline = m.mode->pipeline();
        Stage s;
        for (std::size_t i = 0; i < pipeline.size(); ++i) {
            if (stop.is_cancelled())
                return fail_at(i, error(canceled(), m.block_id(i)));
            const BlockEnv env = m.env(true, stop);
            auto prepared = pipeline[i]->prepare(env, s);
            if (!prepared)
                return fail_at(i, prepared.error());
            prepared->id = m.block_id(i);
            auto v = validate(*prepared, i < recorded.size() ? recorded[i] : Answers{});
            if (!v)
                return fail_at(i, v.error());
            auto out = pipeline[i]->apply(env, s, *v);
            if (!out)
                return fail_at(i, out.error());
            if (i < shapes.size() && shape_of(*out) != shapes[i])
                return fail_at(i, error(make_error_code(Errc::device_changed), m.block_id(i), "",
                                        "changed since it was selected; select again"));
            s = std::move(*out);
        }
        return Opened{s.device, s.description, locator, s.notices, s.usb, s.offset_bytes};
    } catch (const std::bad_alloc &) {
        return fail(std::make_error_code(std::errc::not_enough_memory), "");
    } catch (...) {
        return fail(make_error_code(Errc::internal_error), "");
    }
}

Outcome<Opened> run(std::shared_ptr<const Mode> mode, const Answers &answers, Access access, Context context,
                    CancellationToken stop) {
    auto flow = Flow::start(std::move(mode), access, std::move(context), stop);
    if (!flow)
        return std::unexpected(flow.error());
    std::set<std::string, std::less<>> used;
    while (!(*flow)->ready()) {
        const Step &st = (*flow)->step();
        Answers a;
        for (const Field &f : st.fields) {
            const std::string key = std::format("{}.{}", st.id, f.key);
            if (auto it = answers.find(key); it != answers.end()) {
                a[f.key] = it->second;
                used.insert(key);
            }
        }
        if (auto r = (*flow)->submit(a, stop); !r)
            return std::unexpected(r.error());
    }
    for (const auto &[key, value] : answers) {
        if (used.contains(key))
            continue;
        const auto dot = key.find('.');
        return fail(make_error_code(Errc::flow_invalid_answer), dot == std::string::npos ? key : key.substr(0, dot),
                    dot == std::string::npos ? std::string() : key.substr(dot + 1),
                    std::format("unknown answer '{}' for mode '{}'", key, (*flow)->mode().id));
    }
    return (*flow)->open(stop);
}

} // namespace rawdisk::open
