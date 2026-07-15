#pragma once

namespace axk {

struct build_info {
    const char *source_identity;
    const char *package_basename;
    const char *git_tag;
    const char *git_branch;
    const char *git_sha_short;
    bool is_tagged_release;
    bool is_dirty;
};

} // namespace axk
