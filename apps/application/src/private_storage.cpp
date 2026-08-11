#include "private_storage.hpp"

#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace axk::app::detail {
namespace {

Error storage_error(std::string message) { return {"private_storage_unavailable", std::move(message)}; }

#if defined(_WIN32)
struct LocalMemory {
    void *value{};
    LocalMemory() = default;
    explicit LocalMemory(void *pointer) : value(pointer) {}
    LocalMemory(const LocalMemory &) = delete;
    LocalMemory &operator=(const LocalMemory &) = delete;
    LocalMemory(LocalMemory &&other) noexcept : value(std::exchange(other.value, nullptr)) {}
    LocalMemory &operator=(LocalMemory &&other) noexcept {
        if (this != &other) {
            if (value != nullptr)
                LocalFree(value);
            value = std::exchange(other.value, nullptr);
        }
        return *this;
    }
    ~LocalMemory() {
        if (value != nullptr)
            LocalFree(value);
    }
};

Result<LocalMemory> current_user_sid() {
    HANDLE token{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0)
        return std::unexpected(storage_error("current user identity is unavailable"));
    DWORD size{};
    static_cast<void>(GetTokenInformation(token, TokenUser, nullptr, 0U, &size));
    LocalMemory memory{LocalAlloc(LPTR, size)};
    const auto ready = memory.value != nullptr && GetTokenInformation(token, TokenUser, memory.value, size, &size) != 0;
    CloseHandle(token);
    if (!ready)
        return std::unexpected(storage_error("current user identity is unavailable"));
    return memory;
}

Result<PACL> owner_only_acl(PSID owner, LocalMemory &storage) {
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<wchar_t *>(owner);
    PACL acl{};
    if (SetEntriesInAclW(1U, &access, nullptr, &acl) != ERROR_SUCCESS)
        return std::unexpected(storage_error("owner-only access control cannot be created"));
    storage.value = acl;
    return acl;
}

Result<void> verify_owner_only_acl(const std::filesystem::path &path, PSID owner) {
    PACL acl{};
    PSECURITY_DESCRIPTOR descriptor{};
    const auto status = GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                              &acl, nullptr, &descriptor);
    LocalMemory descriptor_memory{descriptor};
    if (status != ERROR_SUCCESS || acl == nullptr)
        return std::unexpected(storage_error("private storage access control is unavailable"));
    bool owner_allowed{};
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void *raw{};
        if (GetAce(acl, index, &raw) == 0)
            return std::unexpected(storage_error("private storage access control is malformed"));
        const auto *header = static_cast<ACE_HEADER *>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            continue;
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw);
        auto *sid = const_cast<DWORD *>(&ace->SidStart);
        if (EqualSid(sid, owner) == 0)
            return std::unexpected(storage_error("private storage permits another principal"));
        owner_allowed = true;
    }
    if (!owner_allowed)
        return std::unexpected(storage_error("private storage does not permit its owner"));
    return {};
}
#endif

} // namespace

Result<void> prepare_private_directory(const std::filesystem::path &path) {
    std::error_code error;
    const auto existed = std::filesystem::exists(path, error);
    if (error)
        return std::unexpected(storage_error("private storage directory cannot be inspected"));
#if defined(_WIN32)
    auto token_user = current_user_sid();
    if (!token_user)
        return std::unexpected(token_user.error());
    PSID owner = static_cast<TOKEN_USER *>(token_user->value)->User.Sid;
    if (!existed) {
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error)
                return std::unexpected(storage_error("private storage parent directory cannot be created"));
        }
        LocalMemory acl_memory;
        auto acl = owner_only_acl(owner, acl_memory);
        if (!acl)
            return std::unexpected(acl.error());
        SECURITY_DESCRIPTOR descriptor{};
        if (InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) == 0 ||
            SetSecurityDescriptorDacl(&descriptor, TRUE, *acl, FALSE) == 0 ||
            SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED) == 0) {
            return std::unexpected(storage_error("owner-only directory security cannot be initialized"));
        }
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
        if (CreateDirectoryW(path.c_str(), &security) == 0) {
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                return std::unexpected(storage_error("private storage directory cannot be created"));
        }
    }
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        return std::unexpected(storage_error("private storage path is not a local directory"));
    }
    return verify_owner_only_acl(path, owner);
#else
    if (!existed) {
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            if (error)
                return std::unexpected(storage_error("private storage parent directory cannot be created"));
        }
        if (::mkdir(path.c_str(), 0700) != 0) {
            if (errno != EEXIST)
                return std::unexpected(storage_error("private storage directory cannot be created"));
        }
    }
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid())
        return std::unexpected(storage_error("private storage path is not an owned local directory"));
    if ((status.st_mode & 0777U) != 0700U)
        return std::unexpected(storage_error("private storage directory is not owner-only"));
    return {};
#endif
}

Result<void> create_private_file(const std::filesystem::path &path) {
#if defined(_WIN32)
    auto token_user = current_user_sid();
    if (!token_user)
        return std::unexpected(token_user.error());
    PSID owner = static_cast<TOKEN_USER *>(token_user->value)->User.Sid;
    LocalMemory acl_memory;
    auto acl = owner_only_acl(owner, acl_memory);
    if (!acl)
        return std::unexpected(acl.error());
    SECURITY_DESCRIPTOR descriptor{};
    if (InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) == 0 ||
        SetSecurityDescriptorDacl(&descriptor, TRUE, *acl, FALSE) == 0 ||
        SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED) == 0) {
        return std::unexpected(storage_error("owner-only file security cannot be initialized"));
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    const auto file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0U, &attributes, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::unexpected(storage_error("private storage file cannot be created exclusively"));
    CloseHandle(file);
    return {};
#else
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
        return std::unexpected(storage_error("private storage file cannot be created exclusively"));
    const auto closed = ::close(descriptor);
    if (closed != 0)
        return std::unexpected(storage_error("private storage file cannot be finalized"));
    return {};
#endif
}

} // namespace axk::app::detail
