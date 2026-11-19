#include "axklib/conversion/plan.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "axklib/package_archive.hpp"

namespace axk::conversion {
Result<void> NewImageSet::validate(std::uint64_t byte_limit) const {
    std::set<std::string> names;
    if (members.size() > maximum_members)
        return std::unexpected(
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "Too many image set members"));
    for (const auto &member : members) {
        const auto &name = member.filename;
        if (name.empty() || name.size() > 64 || name.front() == '.' || name.back() == '.' ||
            name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.") !=
                std::string::npos)
            return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::io,
                                              "Image member requires a portable filename"));
        auto key = name;
        std::ranges::transform(key, key.begin(),
                               [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
        const auto stem = key.substr(0, key.find('.'));
        if (key == "MANIFEST.JSON" || stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
            (stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem.back() >= '0' &&
             stem.back() <= '9') ||
            !names.insert(key).second || member.bytes.empty() || member.bytes.capacity() > byte_limit)
            return std::unexpected(make_error(ErrorCode::invalid_argument, ErrorCategory::io,
                                              "Image set contains an invalid or duplicate member"));
        byte_limit -= member.bytes.capacity();
    }
    return {};
}
PreparedPlan::PreparedPlan(Report report, NewImageSet images)
    : report_(std::move(report)), output_(std::move(images)), retained_bytes_(report_bytes(report_)) {
    auto &review = report_.destination.emplace<NewImageSetReview>();
    const auto &members = std::get<NewImageSet>(output_).members;
    review.members.reserve(members.size());
    retained_bytes_ += members.capacity() * sizeof(ImageMember) + review.members.capacity() * sizeof(ImageMemberReview);
    for (const auto &member : members) {
        auto &entry = review.members.emplace_back(
            ImageMemberReview{member.filename, member.bytes.size(),
                              package_internal::hex_digest(package_internal::sha256(member.bytes))});
        retained_bytes_ +=
            member.bytes.capacity() + member.filename.capacity() + entry.filename.capacity() + entry.sha256.capacity();
    }
}
} // namespace axk::conversion
