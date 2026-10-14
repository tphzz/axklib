#include "axklib/native/registry.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace axk::native {
namespace {

Error registry_error(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}

bool openable(RecognitionState state) {
    return state == RecognitionState::supported || state == RecognitionState::incomplete;
}

bool overlaps(const SourceLocation &left, const SourceLocation &right) {
    if (left.member_id != right.member_id)
        return false;
    // Raw-member claims cannot be compared with filesystem-relative offsets.
    if (left.root_id.empty() != right.root_id.empty())
        return true;
    if (left.root_id != right.root_id || left.entry_id != right.entry_id)
        return false;
    if (left.length == 0U || right.length == 0U)
        return left.offset == right.offset;
    return left.offset < right.offset + right.length && right.offset < left.offset + left.length;
}

} // namespace

Result<void> FormatRegistry::add(FormatProvider provider) {
    const auto &descriptor = provider.descriptor;
    if (descriptor.id.empty() || descriptor.name.empty() || descriptor.profiles.empty() || !provider.recognize ||
        !provider.open)
        return std::unexpected(registry_error("Format provider requires identity, profiles, recognition and opening"));
    std::set<std::string> profiles;
    for (const auto &profile : descriptor.profiles)
        if (profile.empty() || !profiles.insert(profile).second)
            return std::unexpected(registry_error("Format provider profiles must be nonempty and unique"));
    if (std::ranges::any_of(providers_, [&](const auto &existing) { return existing.descriptor.id == descriptor.id; }))
        return std::unexpected(registry_error("Format provider identity is already registered"));
    providers_.push_back(std::move(provider));
    return {};
}

std::vector<FormatDescriptor> FormatRegistry::formats() const {
    std::vector<FormatDescriptor> result;
    result.reserve(providers_.size());
    for (const auto &provider : providers_)
        result.push_back(provider.descriptor);
    return result;
}

Result<OpenResult> FormatRegistry::open(std::shared_ptr<const SourceSet> sources,
                                        const std::vector<std::string> &selected_formats,
                                        const CancellationToken &cancellation) const {
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(active.error());
    if (!sources)
        return std::unexpected(registry_error("Native discovery requires admitted sources"));
    std::set<std::string> selected_ids;
    for (const auto &format : selected_formats) {
        if (!selected_ids.insert(format).second)
            return std::unexpected(registry_error("Selected native formats must be unique"));
        if (std::ranges::none_of(providers_, [&](const auto &provider) { return provider.descriptor.id == format; }))
            return std::unexpected(registry_error("Selected native format is not registered"));
    }
    OpenResult result;
    std::vector<const FormatProvider *> candidates;
    for (const auto &provider : providers_) {
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        auto recognition = provider.recognize(*sources, cancellation);
        if (!recognition)
            return std::unexpected(recognition.error());
        if (recognition->format_id != provider.descriptor.id)
            return std::unexpected(registry_error("Format recognition returned a different provider identity"));
        if (recognition->state == RecognitionState::no_match)
            continue;
        if (openable(recognition->state) && recognition->claims.empty())
            return std::unexpected(registry_error("Openable recognition requires explicit source claims"));
        for (const auto &claim : recognition->claims) {
            const auto extent = sources->locate(claim.member_id, claim.root_id, claim.entry_id);
            if (!extent || claim.offset > extent->length || claim.length > extent->length - claim.offset)
                return std::unexpected(registry_error("Recognition claim is outside its admitted source"));
        }
        if (openable(recognition->state))
            candidates.push_back(&provider);
        result.candidates.push_back(std::move(*recognition));
    }
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(active.error());
    if (candidates.empty()) {
        if (!selected_formats.empty())
            return std::unexpected(registry_error("Selected native format cannot open these sources"));
        return result;
    }
    const auto competing = [&](const FormatProvider *left, const FormatProvider *right) {
        const auto &left_claims =
            std::ranges::find(result.candidates, left->descriptor.id, &Recognition::format_id)->claims;
        const auto &right_claims =
            std::ranges::find(result.candidates, right->descriptor.id, &Recognition::format_id)->claims;
        for (const auto &left_claim : left_claims)
            for (const auto &right_claim : right_claims)
                if (overlaps(left_claim, right_claim))
                    return true;
        return false;
    };
    std::vector<const FormatProvider *> chosen;
    for (const auto &format : selected_formats) {
        const auto selected =
            std::ranges::find_if(candidates, [&](const auto *candidate) { return candidate->descriptor.id == format; });
        if (selected == candidates.end())
            return std::unexpected(registry_error("Selected native format cannot open these sources"));
        if (std::ranges::any_of(chosen, [&](const auto *previous) { return competing(previous, *selected); }))
            return std::unexpected(registry_error("Selected native formats have conflicting source claims"));
        chosen.push_back(*selected);
    }
    std::erase_if(candidates, [&](const auto *candidate) {
        return !selected_ids.contains(candidate->descriptor.id) &&
               std::ranges::any_of(chosen, [&](const auto *selected) { return competing(selected, candidate); });
    });
    for (std::size_t left = 0U; left < candidates.size(); ++left) {
        for (std::size_t right = left + 1U; right < candidates.size(); ++right)
            if (competing(candidates[left], candidates[right])) {
                result.selection_required = true;
                return result;
            }
    }
    std::set<std::string> identities;
    for (const auto *provider : candidates) {
        auto documents = provider->open(sources, cancellation);
        if (!documents)
            return std::unexpected(documents.error());
        if (const auto active = cancellation.check(); !active)
            return std::unexpected(active.error());
        if (documents->empty())
            return std::unexpected(registry_error("Recognized format returned no native documents"));
        for (auto &document : *documents) {
            if (!document || document->sources() != sources)
                return std::unexpected(registry_error("Native document must retain its admitted sources"));
            const auto &summary = document->summary();
            if (summary.id.empty() || !identities.insert(summary.id).second ||
                summary.format_id != provider->descriptor.id ||
                std::ranges::find(provider->descriptor.profiles, summary.profile) ==
                    provider->descriptor.profiles.end())
                return std::unexpected(
                    registry_error("Native document identity or profile violates its provider contract"));
            result.documents.push_back(std::move(document));
        }
    }
    return result;
}

} // namespace axk::native
