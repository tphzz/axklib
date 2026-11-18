#include "axklib/sdk/source_set.hpp"

#include <utility>

#include "axklib/inspection/disk.hpp"
#include "sdk_internal.hpp"

namespace axk {
using namespace sdk_internal;
struct AXK_SDK_HIDDEN source_set::impl {
    inspection::DiskInspection catalog;
};
source_set::source_set(std::unique_ptr<impl> value) noexcept : impl_(std::move(value)) {}
source_set::source_set(source_set &&) noexcept = default;
source_set &source_set::operator=(source_set &&) noexcept = default;
source_set::~source_set() = default;
result<source_set> source_set::open(const std::vector<std::string> &paths, operation_context &context) noexcept {
    return protect<source_set>([&]() -> result<source_set> {
        if (!context.impl_)
            return invalid_argument("Operation context has been moved");
        std::vector<std::filesystem::path> files;
        for (const auto &value : paths) {
            auto path = checked_path(value, "disk member");
            if (!path)
                return path.error();
            files.push_back(std::move(*path));
        }
        auto opened = inspection::DiskInspection::open(files, context.impl_->cancellation.token());
        if (!opened)
            return public_error(opened.error());
        return source_set{std::make_unique<impl>(impl{std::move(*opened)})};
    });
}
result<std::string> source_set::summary_json() const noexcept {
    return protect<std::string>([&]() -> result<std::string> {
        if (!impl_)
            return invalid_argument("Source set has been moved");
        return impl_->catalog.summary().dump();
    });
}
result<std::string> source_set::objects_json(const std::string &id, std::size_t offset, std::size_t limit,
                                             operation_context &context) const noexcept {
    return protect<std::string>([&]() -> result<std::string> {
        if (!impl_ || !context.impl_)
            return invalid_argument("Source set or context has been moved");
        auto page = impl_->catalog.objects(id, offset, limit, context.impl_->cancellation.token());
        if (!page)
            return public_error(page.error());
        return page->dump();
    });
}
result<std::string> source_set::relationships_json(const std::string &id, std::size_t offset, std::size_t limit,
                                                   operation_context &context) const noexcept {
    return protect<std::string>([&]() -> result<std::string> {
        if (!impl_ || !context.impl_)
            return invalid_argument("Source set or context has been moved");
        auto page = impl_->catalog.relationships(id, offset, limit, context.impl_->cancellation.token());
        if (!page)
            return public_error(page.error());
        return page->dump();
    });
}
} // namespace axk
