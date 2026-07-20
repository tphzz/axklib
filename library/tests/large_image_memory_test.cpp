#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include <sys/resource.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

#include "axklib/catalog.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

int main() {
    constexpr std::uint64_t kibibyte = 1024U;
    constexpr std::uint64_t memory_budget_bytes = 256U * 1024U * kibibyte;
    axk::HdsBuildManifest manifest{"1.1", axk::maximum_hds_size, {}};
    for (std::uint8_t index = 0; index < 8U; ++index) {
        axk::VolumeSpec volume;
        volume.name = "Volume " + std::to_string(index + 1U);
        manifest.partitions.push_back({"hd" + std::to_string(index + 1U), {std::move(volume)}});
    }

    const auto path =
        std::filesystem::temp_directory_path() / ("axklib-large-memory-" + std::to_string(::getpid()) + ".hds");
    std::error_code error;
    std::filesystem::remove(path, error);
    const auto written = axk::write_hds_image(manifest, path);
    if (!written) {
        std::cerr << axk::render_error(written.error()) << '\n';
        return 1;
    }
    const auto image = axk::open_image(path);
    if (!image) {
        std::cerr << axk::render_error(image.error()) << '\n';
        return 2;
    }
    const auto catalog = axk::build_object_catalog(*image);
    if (!catalog) {
        std::cerr << axk::render_error(catalog.error()) << '\n';
        return 3;
    }

#if defined(__APPLE__)
    // Darwin's peak RSS charges sparse-file writeback pages to this process.
    // The Mach physical footprint still catches a retained whole-image
    // allocation.
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
        std::cerr << "large-image memory exceeded " << memory_budget_bytes / kibibyte
                  << " KiB: " << measured_memory_bytes / kibibyte << " KiB\n";
        return 4;
    }
    std::cout << "2 GiB / 8-partition inventory memory: " << measured_memory_bytes / kibibyte << " KiB\n";
    return 0;
}
