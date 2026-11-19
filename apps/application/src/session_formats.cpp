#include "session_formats.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "formats/a_series/admission.hpp"
#include "formats/tx16w/admission.hpp"
#include "formats/typhoon/admission.hpp"

namespace axk::app {

void SessionFormats::retain_member_group(const std::string &format, std::vector<std::string> member_ids) {
    member_groups_[format].push_back(std::move(member_ids));
}

std::vector<std::vector<std::string>> SessionFormats::member_groups(const std::string &format) const {
    const auto found = member_groups_.find(format);
    return found == member_groups_.end() ? std::vector<std::vector<std::string>>{} : found->second;
}

Result<void> SessionFormats::add(native::FormatProvider provider, Bind bind_document) {
    if (!bind_document)
        return std::unexpected(Error{"document_invalid", "A native format needs a session binding"});
    const auto id = provider.descriptor.id;
    if (auto added = registry_.add(std::move(provider)); !added)
        return std::unexpected(Error{"document_invalid", added.error().message});
    bindings_.emplace(id, std::move(bind_document));
    return {};
}

Result<NativeDocumentAdmission> SessionFormats::bind(const std::shared_ptr<const native::Document> &document) const {
    const auto binding = bindings_.find(document->summary().format_id);
    if (binding == bindings_.end())
        return std::unexpected(Error{"document_invalid", "No session binding for the discovered format"});
    return binding->second(document);
}

Result<void> configure_builtin_session_formats(SessionFormats &formats, const SessionFormatContext &context) {
    if (auto added = formats::a_series::register_format(formats, context); !added)
        return added;
    if (auto added = formats::typhoon::register_format(formats, context); !added)
        return added;
    return formats::tx16w::register_format(formats, context);
}

} // namespace axk::app
