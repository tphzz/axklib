#include "axklib/sdk/devices.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <utility>

#include "axklib/application/device_sessions.hpp"
#include "axklib/application/device_xml.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/write_operations.hpp"
#include "axklib/media.hpp"
#include "sdk_internal.hpp"

namespace axk {
using namespace sdk_internal;
namespace {
constexpr std::string_view owner = "sdk";
error failure(const app::Error &value) {
    return {error_code::transaction_rejected, error_category::transaction, value.code + ": " + value.message, {}};
}
device_candidate candidate(const app::DeviceCandidate &value) {
    return {value.id, value.model, value.serial, value.size_bytes, value.sector_size};
}
device_session session(const app::DeviceSessionSummary &value, bool writes) {
    device_session result{value.session_id, candidate(value.candidate), {},
                          writes,           value.writes_authorized,    value.invalidated};
    for (const auto &target : value.targets) {
        device_target converted{target.id,        target.label,   target.size_bytes, target.sector_size,
                                target.read_only, target.notices, target.offset};
        if (target.scsi)
            converted.scsi =
                device_scsi_target{target.scsi->id,       static_cast<scsi_device_type>(target.scsi->device_type),
                                   target.scsi->vendor,   target.scsi->product,
                                   target.scsi->revision, target.scsi->serial};
        result.targets.push_back(std::move(converted));
    }
    return result;
}
device_operation operation(const app::DeviceOperationStatus &value) {
    return {value.operation_id, value.state, value.model, value.serial, value.target_label, value.target_size};
}
} // namespace

struct device_host::impl {
    std::mutex mutex;
    app::Sandbox sandbox;
    app::DeviceSessionManager devices;
    app::ImageSessionManager images;
    app::UploadStore uploads;
    app::AlterationJournalStore journals;
    app::OperationRegistry registry = app::make_operation_registry();
    std::map<std::pair<std::string, std::string>, app::ImageSessionSummary> opened;
    impl(app::Sandbox storage, const std::filesystem::path &directory, const std::filesystem::path &helper)
        : sandbox(std::move(storage)), devices(directory / "devices", true, app::native_device_writes_qualified(),
                                               app::native_device_provider(helper)),
          images(sandbox, 8U, 500U, std::chrono::hours{24}, std::chrono::steady_clock::now, nullptr,
                 128ULL * 1024ULL * 1024ULL, 256U, &devices),
          uploads(directory / "uploads", 1024U, 1024U, 1U, 1024U, std::chrono::minutes{1}),
          journals(directory / "file-journals") {}
};

device_host::device_host() = default;
device_host::~device_host() = default;
device_host::device_host(device_host &&) noexcept = default;
device_host &device_host::operator=(device_host &&) noexcept = default;
bool device_host::available() noexcept { return true; }

result<std::vector<scsi2sd_xml_target>> inspect_scsi2sd_xml(const std::string &xml) {
    return protect<std::vector<scsi2sd_xml_target>>([&]() -> result<std::vector<scsi2sd_xml_target>> {
        auto inspected = app::inspect_scsi2sd_xml(xml);
        if (!inspected)
            return failure(inspected.error());
        std::vector<scsi2sd_xml_target> result;
        for (const auto &target : *inspected)
            result.push_back({target.id, target.label, target.size_bytes, target.sector_size, target.enabled,
                              target.eligible, target.read_only, target.notices});
        return result;
    });
}

result<device_host> device_host::create(const std::string &directory, const std::string &helper) {
    return protect<device_host>([&]() -> result<device_host> {
        auto path = checked_path(directory, "device state directory");
        if (!path)
            return path.error();
        auto sandbox = app::Sandbox::create({});
        if (!sandbox)
            return failure(sandbox.error());
        std::filesystem::path helper_path;
        if (!helper.empty()) {
            auto parsed = checked_path(helper, "device helper path");
            if (!parsed)
                return parsed.error();
            helper_path = std::move(*parsed);
        }
        device_host host;
        host.impl_ = std::make_unique<impl>(std::move(*sandbox), *path, helper_path);
        if (!host.impl_->devices.enabled())
            return internal_error("private device state storage is unavailable");
        if (auto bound =
                app::bind_session_write_operations(host.impl_->registry, host.impl_->sandbox, host.impl_->uploads,
                                                   host.impl_->images, host.impl_->journals);
            !bound)
            return failure(bound.error());
        return host;
    });
}
result<std::vector<device_candidate>> device_host::list() {
    return protect<std::vector<device_candidate>>([&]() -> result<std::vector<device_candidate>> {
        if (!impl_)
            return invalid_argument("device host is not initialized");
        const std::scoped_lock lock{impl_->mutex};
        auto values = impl_->devices.list(owner);
        if (!values)
            return failure(values.error());
        std::vector<device_candidate> result;
        for (const auto &value : *values)
            result.push_back(candidate(value));
        return result;
    });
}
result<device_session> device_host::connect(const std::string &id, const std::string &layout, const std::string &xml) {
    return protect<device_session>([&]() -> result<device_session> {
        if (!impl_)
            return invalid_argument("device host is not initialized");
        const std::scoped_lock lock{impl_->mutex};
        auto value = impl_->devices.connect(id, owner, layout, xml);
        if (!value)
            return failure(value.error());
        return session(*value, impl_->devices.writes_supported());
    });
}
result<device_session> device_host::status(const std::string &id) {
    return protect<device_session>([&]() -> result<device_session> {
        if (!impl_)
            return invalid_argument("device host is not initialized");
        const std::scoped_lock lock{impl_->mutex};
        auto value = impl_->devices.status(id, owner);
        if (!value)
            return failure(value.error());
        return session(*value, impl_->devices.writes_supported());
    });
}
result<image> device_host::open_image(const std::string &id, const std::string &target, operation_context &context) {
    return protect<image>([&]() -> result<image> {
        if (!impl_ || !context.impl_)
            return invalid_argument("device host or operation context is not initialized");
        const std::scoped_lock lock{impl_->mutex};
        const auto key = std::pair{id, target};
        if (!impl_->opened.contains(key)) {
            app::ImageSourceRef source;
            source.kind = app::ImageSourceKind::device;
            source.device_session_id = id;
            source.device_target_id = target;
            auto opened = impl_->images.open({{source}, {}}, std::string{owner}, context.impl_->cancellation.token(),
                                             context.impl_.get());
            if (!opened)
                return failure(opened.error());
            impl_->opened.emplace(key, std::move(*opened));
        }
        const auto &opened = impl_->opened.at(key);
        auto access =
            impl_->images.begin_source_read({opened.image_id, opened.sources.front().id}, owner, opened.revision);
        if (!access)
            return failure(access.error());
        auto media = open_media(access->reader, "device.hds", context.impl_->cancellation.token());
        if (!media)
            return public_error(media.error());
        return image_factory::open(std::move(*media), context);
    });
}
result<void> device_host::authorize_writes(const std::string &id, const std::string &target, bool acknowledged) {
    return protect<void>([&]() -> result<void> {
        if (!impl_)
            return invalid_argument("device host is not initialized");
        const std::scoped_lock lock{impl_->mutex};
        auto authorized = impl_->devices.authorize_writes(id, target, owner, acknowledged);
        if (!authorized)
            return failure(authorized.error());
        return {};
    });
}
result<device_operation> device_host::apply(const std::string &id, const std::string &target,
                                            const std::string &operation_id, const std::string &manifest,
                                            operation_context &context) {
    return protect<device_operation>([&]() -> result<device_operation> {
        if (!impl_ || !context.impl_)
            return invalid_argument("device host or operation context is not initialized");
        const std::scoped_lock lock{impl_->mutex};
        const auto at = impl_->opened.find({id, target});
        if (at == impl_->opened.end())
            return invalid_argument("open the device target before editing");
        if (at->second.documents.size() != 1U || at->second.documents.front().format_id != "yamaha.a-series")
            return invalid_argument("device target has no unique editable A-series document");
        if (manifest.size() > 1024U * 1024U)
            return invalid_argument("alteration manifest exceeds 1 MiB");
        auto json = nlohmann::json::parse(manifest, nullptr, false);
        if (json.is_discarded() || !json.is_object())
            return invalid_argument("invalid alteration manifest JSON");
        app::OperationContext execution{std::string{owner},  operation_id, context.impl_->cancellation.token(),
                                        context.impl_.get(), {},           {},
                                        operation_id};
        auto applied = impl_->registry.invoke("images.alter",
                                              {{"imageId", at->second.image_id},
                                               {"documentId", at->second.documents.front().id},
                                               {"expectedRevision", at->second.revision},
                                               {"manifest", {{"inline", std::move(json)}}}},
                                              execution);
        if (!applied)
            return failure(applied.error());
        at->second.revision = applied->at("revision").get<std::uint64_t>();
        auto values = impl_->devices.operations(owner);
        if (!values)
            return failure(values.error());
        for (const auto &value : *values)
            if (value.operation_id == operation_id)
                return operation(value);
        return internal_error("device write status is unavailable; retain the operation identity and check status");
    });
}
result<std::vector<device_operation>> device_host::operations() {
    return protect<std::vector<device_operation>>([&]() -> result<std::vector<device_operation>> {
        if (!impl_)
            return invalid_argument("device host is not initialized");
        const std::scoped_lock lock{impl_->mutex};
        auto values = impl_->devices.operations(owner);
        if (!values)
            return failure(values.error());
        std::vector<device_operation> result;
        for (const auto &value : *values)
            result.push_back(operation(value));
        return result;
    });
}
result<void> device_host::recover(const std::string &operation_id, const std::string &id, const std::string &target,
                                  bool acknowledged) {
    return protect<void>([&]() -> result<void> {
        if (!impl_)
            return invalid_argument("device host is not initialized");
        const std::scoped_lock lock{impl_->mutex};
        if (impl_->opened.contains({id, target}))
            return invalid_argument("close device images before recovery");
        auto recovered = impl_->devices.recover(operation_id, id, target, owner, acknowledged);
        if (!recovered)
            return failure(recovered.error());
        return {};
    });
}
result<void> device_host::close(const std::string &id) {
    return protect<void>([&]() -> result<void> {
        if (!impl_)
            return invalid_argument("device host is not initialized");
        const std::scoped_lock lock{impl_->mutex};
        for (auto at = impl_->opened.begin(); at != impl_->opened.end();) {
            if (at->first.first != id) {
                ++at;
                continue;
            }
            auto closed = impl_->images.close(at->second.image_id, owner);
            if (!closed)
                return failure(closed.error());
            at = impl_->opened.erase(at);
        }
        auto closed = impl_->devices.close(id, owner);
        if (!closed)
            return failure(closed.error());
        return {};
    });
}
} // namespace axk
