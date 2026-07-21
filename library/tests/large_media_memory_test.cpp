#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "writer_internal.hpp"

int main() {
    constexpr std::uint64_t kibibyte = 1024U;
    constexpr std::uint64_t mebibyte = 1024U * kibibyte;
    constexpr std::uint64_t payload_size = 48U * mebibyte;
    constexpr std::uint64_t memory_budget_bytes = 128U * mebibyte;

    axk::MediaBuildManifest manifest;
    manifest.schema_version = "1.1";
    manifest.format = axk::MediaImageFormat::iso9660;
    axk::detail::PreparedMediaImage prepared{manifest,
                                             axk::MediaBuildLimits{64U * mebibyte, 64U * mebibyte, 64U * mebibyte},
                                             {},
                                             {{"PAYLOAD", std::vector<std::byte>(payload_size, std::byte{0x5a})}},
                                             {}};

    const auto path =
        std::filesystem::temp_directory_path() / ("axklib-large-media-memory-" + std::to_string(::getpid()) + ".iso");
    std::error_code error;
    std::filesystem::remove(path, error);
    const auto written = axk::detail::write_prepared_media_image(prepared, path, false, {});
    if (!written) {
        std::cerr << axk::render_error(written.error()) << '\n';
        return 1;
    }

#if defined(__APPLE__)
    task_vm_info_data_t vm_info{};
    mach_msg_type_number_t vm_info_count = TASK_VM_INFO_COUNT;
    const auto usage_ok = ::task_info(::mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm_info),
                                      &vm_info_count) == KERN_SUCCESS;
    const auto measured_memory_bytes = static_cast<std::uint64_t>(vm_info.phys_footprint);
#else
    struct rusage usage{};
    const auto usage_ok = ::getrusage(RUSAGE_SELF, &usage) == 0;
    const auto measured_memory_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * kibibyte;
#endif
    std::filesystem::remove(path, error);
    if (!usage_ok || measured_memory_bytes > memory_budget_bytes) {
        std::cerr << "large-media memory exceeded " << memory_budget_bytes / kibibyte
                  << " KiB: " << measured_memory_bytes / kibibyte << " KiB\n";
        return 2;
    }
    std::cout << "48 MiB ISO retained-file memory: " << measured_memory_bytes / kibibyte << " KiB\n";
    return 0;
}
